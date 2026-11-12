/*
 * sd_sherpa - speech-dispatcher output module for sherpa-onnx TTS models
 *
 * The models are loaded into the module itself through the sherpa-onnx C API
 * and stay loaded, so only the first message after a (re)start waits for the
 * load.  One model is kept at a time; a voice of another model replaces it.
 * Models are the directories in SherpaModelsDir (and
 * ~/.local/share/sherpa-onnx/tts); the family is told by the files in them:
 *
 *   Supertonic (tts.json + voice.bin)   31 languages, the speakers of voice.bin
 *                                       (F1..F5, M1..M5) for each of them:
 *                                       supertonic-<lang>-<female|male>-<n>
 *   Pocket TTS (lm_main*.onnx)          English, a voice per reference WAV in
 *                                       test_wavs/ and voices/: pocket-en-<name>
 *   VITS with characters (MMS)          the language from the directory name
 *                                       (mms-ru): one voice, named as the dir
 *
 * The text is cut into sentences; each one is synthesized and handed to the
 * speech-dispatcher server at once, so the next one is synthesized while the
 * previous one plays.  The first piece is cut at a comma after
 * SherpaFirstChunk characters, so the first sound comes sooner.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sherpa-onnx/c-api/c-api.h>
#include <speech-dispatcher/spd_module_main.h>

#define DBG(...) do { if (debug_on) { fprintf (stderr, "sd_sherpa: " __VA_ARGS__); fputc ('\n', stderr); } } while (0)
#define ERR(...) do { fprintf (stderr, "sd_sherpa: " __VA_ARGS__); fputc ('\n', stderr); } while (0)

typedef enum { FAMILY_SUPERTONIC, FAMILY_VITS, FAMILY_POCKET } Family;

/* the order of the families is also the preference among voices of a language */
static const char *const family_names[] = { "supertonic", "vits", "pocket" };

static const char *const supertonic_langs[] = {
  "en", "ko", "ja", "ar", "bg", "cs", "da", "de", "el", "es", "et",
  "fi", "fr", "hi", "hr", "hu", "id", "it", "lt", "lv", "nl", "pl",
  "pt", "ro", "ru", "sk", "sl", "sv", "tr", "uk", "vi", NULL
};

typedef struct
{
  gchar *name;    /* supertonic-3 */
  gchar *dir;
  Family family;
  int speakers;
} Model;

typedef struct
{
  gchar *name;      /* supertonic-ru-male-1 */
  gchar *language;  /* ru */
  gchar *gender;    /* "male", "female" or NULL */
  Model *model;
  int sid;
  gchar *wav;       /* Pocket: the reference voice */
  const SherpaOnnxWave *ref;  /* read when first used */
} Voice;

/* configuration */
static gchar *cfg_models_dir = NULL;
static GHashTable *cfg_lang_voice = NULL;  /* SherpaVoice "<lang>" "<voice>" */
static int cfg_threads = 0;
static int cfg_steps = 5;
static int cfg_first_chunk = 40;
static gchar *cfg_preload = NULL;          /* a voice or model name */
static int debug_on = 0;

static GPtrArray *models = NULL;
static GPtrArray *voices = NULL;
static SPDVoice **spd_voices = NULL;

/* message settings */
static gchar *set_voice_name = NULL;
static const gchar *set_gender = NULL;
static int set_type_index = 0;
static gchar *set_language = NULL;
static int set_rate = 0;
static int set_volume = 0;

/* the loaded model */
static Model *loaded_model = NULL;
static const SherpaOnnxOfflineTts *tts = NULL;

static volatile int stop_requested = 0;
static gboolean quit_requested = FALSE;



static gchar *
unquote (gchar *s)
{
  gsize n;

  g_strstrip (s);
  n = strlen (s);
  if (n >= 2 && s[0] == '"' && s[n - 1] == '"')
    {
      s[n - 1] = '\0';
      return s + 1;
    }
  return s;
}



int
module_config (const char *configfile)
{
  gchar *contents = NULL;
  gchar **lines;

  cfg_models_dir = g_strdup ("/usr/share/sherpa-onnx/tts");

  if (configfile == NULL || !g_file_get_contents (configfile, &contents, NULL, NULL))
    return 0;

  lines = g_strsplit (contents, "\n", -1);
  for (gchar **l = lines; *l != NULL; l++)
    {
      gchar *line = g_strstrip (*l);
      gchar *value;

      if (*line == '\0' || *line == '#')
        continue;
      value = line;
      while (*value != '\0' && !isspace ((guchar) *value))
        value++;
      if (*value != '\0')
        *value++ = '\0';
      value = unquote (value);

      if (g_ascii_strcasecmp (line, "SherpaModelsDir") == 0)
        g_free (cfg_models_dir), cfg_models_dir = g_strdup (value);
      else if (g_ascii_strcasecmp (line, "SherpaThreads") == 0)
        cfg_threads = atoi (value);
      else if (g_ascii_strcasecmp (line, "SherpaSupertonicSteps") == 0)
        cfg_steps = atoi (value);
      else if (g_ascii_strcasecmp (line, "SherpaFirstChunk") == 0)
        cfg_first_chunk = atoi (value);
      else if (g_ascii_strcasecmp (line, "SherpaPreload") == 0)
        {
          g_free (cfg_preload);
          cfg_preload = (*value != '\0' && strcmp (value, "0") != 0) ? g_strdup (value) : NULL;
        }
      else if (g_ascii_strcasecmp (line, "SherpaVoice") == 0)
        {
          /* value was unquoted as one string: ru" "supertonic-ru-male-1 */
          gchar **parts = g_strsplit_set (value, "\" \t", -1);
          gchar *lang = NULL, *name = NULL;

          for (gchar **p = parts; *p != NULL; p++)
            if (**p != '\0')
              {
                if (lang == NULL)
                  lang = *p;
                else if (name == NULL)
                  name = *p;
              }
          if (lang != NULL && name != NULL)
            {
              if (cfg_lang_voice == NULL)
                cfg_lang_voice = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
              g_hash_table_replace (cfg_lang_voice, g_ascii_strdown (lang, -1), g_strdup (name));
            }
          g_strfreev (parts);
        }
      else if (g_ascii_strcasecmp (line, "Debug") == 0)
        debug_on = atoi (value);
    }
  g_strfreev (lines);
  g_free (contents);
  return 0;
}



/* ------------------------------------------------------------------ models */

/* the first file in dir matching "<prefix>*<suffix>" */
static gchar *
find_file (const gchar *dir, const gchar *prefix, const gchar *suffix)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const gchar *f;
  gchar *found = NULL;

  if (d == NULL)
    return NULL;
  while ((f = g_dir_read_name (d)) != NULL && found == NULL)
    if (g_str_has_prefix (f, prefix) && g_str_has_suffix (f, suffix))
      found = g_build_filename (dir, f, NULL);
  g_dir_close (d);
  return found;
}



static gboolean
has_file (const gchar *dir, const gchar *name)
{
  gchar *p = g_build_filename (dir, name, NULL);
  gboolean ok = g_file_test (p, G_FILE_TEST_EXISTS);
  g_free (p);
  return ok;
}



static Voice *
voice_new (Model *m, const gchar *name, const gchar *language, const gchar *gender, int sid)
{
  Voice *v = g_new0 (Voice, 1);

  v->name = g_strdup (name);
  v->language = g_strdup (language);
  v->gender = g_strdup (gender);
  v->model = m;
  v->sid = sid;
  g_ptr_array_add (voices, v);
  return v;
}



/* the number of speakers in a Supertonic voice.bin: an int64 at the start */
static int
supertonic_speakers (const gchar *dir)
{
  gchar *path = g_build_filename (dir, "voice.bin", NULL);
  FILE *f = fopen (path, "rb");
  int64_t n = 0;

  if (f != NULL)
    {
      if (fread (&n, sizeof n, 1, f) != 1)
        n = 0;
      fclose (f);
    }
  g_free (path);
  return (n > 0 && n < 1000) ? (int) n : 0;
}



static void
add_pocket_voices (Model *m, const gchar *sub)
{
  gchar *dir = g_build_filename (m->dir, sub, NULL);
  GDir *d = g_dir_open (dir, 0, NULL);
  const gchar *f;

  if (d != NULL)
    {
      while ((f = g_dir_read_name (d)) != NULL)
        {
          gchar *stem, *name;
          Voice *v;

          if (!g_str_has_suffix (f, ".wav"))
            continue;
          stem = g_strndup (f, strlen (f) - 4);
          /* sample_fr_hibiki_crepes -> hibiki; the synthesis is English only,
           * and a sample in another language (this one also starts quietly)
           * makes it mumble, so take only sample_en_* */
          if (g_str_has_prefix (stem, "sample_"))
            {
              gchar **parts = g_strsplit (stem, "_", -1);
              gboolean english = g_strv_length (parts) >= 3
                                 && g_ascii_strcasecmp (parts[1], "en") == 0;
              if (english)
                {
                  g_free (stem);
                  stem = g_strdup (parts[2]);
                }
              g_strfreev (parts);
              if (!english)
                {
                  DBG ("Pocket: %s is not an English sample", f);
                  g_free (stem);
                  continue;
                }
            }
          name = g_strconcat (m->name, "-en-", stem, NULL);
          v = voice_new (m, name, "en", NULL, 0);
          v->wav = g_build_filename (dir, f, NULL);
          g_free (name);
          g_free (stem);
        }
      g_dir_close (d);
    }
  g_free (dir);
}



static void
add_model (const gchar *parent, const gchar *name)
{
  gchar *dir = g_build_filename (parent, name, NULL);
  Model *m;
  gchar *f;

  /* a user's model hides a system one of the same name */
  for (guint i = 0; i < models->len; i++)
    if (strcmp (((Model *) g_ptr_array_index (models, i))->name, name) == 0)
      {
        g_free (dir);
        return;
      }

  m = g_new0 (Model, 1);
  m->name = g_strdup (name);
  m->dir = dir;

  if (has_file (dir, "tts.json") && has_file (dir, "voice.bin"))
    {
      m->family = FAMILY_SUPERTONIC;
      m->speakers = supertonic_speakers (dir);
      if (m->speakers == 0)
        goto skip;
      /* voice.bin is made from the sorted style files F1..F5, M1..M5 */
      for (const char *const *l = supertonic_langs; *l != NULL; l++)
        for (int s = 0; s < m->speakers; s++)
          {
            gchar *vname;
            const gchar *gender = NULL;
            int n = s + 1;

            if (m->speakers == 10)
              {
                gender = s < 5 ? "female" : "male";
                n = s % 5 + 1;
              }
            vname = gender != NULL
                    ? g_strdup_printf ("supertonic-%s-%s-%d", *l, gender, n)
                    : g_strdup_printf ("supertonic-%s-%d", *l, n);
            voice_new (m, vname, *l, gender, s);
            g_free (vname);
          }
    }
  else if ((f = find_file (dir, "lm_main", ".onnx")) != NULL)
    {
      g_free (f);
      m->family = FAMILY_POCKET;
      add_pocket_voices (m, "test_wavs");
      add_pocket_voices (m, "voices");
    }
  else if (has_file (dir, "tokens.txt") && !has_file (dir, "voices.bin")
           && !has_file (dir, "espeak-ng-data") && !has_file (dir, "lexicon.txt")
           && (f = find_file (dir, "", ".onnx")) != NULL)
    {
      /* a VITS model on characters, like MMS: the language from the name */
      const gchar *dash = strrchr (name, '-');
      g_free (f);
      if (dash == NULL || strlen (dash + 1) < 2 || strlen (dash + 1) > 3)
        goto skip;
      m->family = FAMILY_VITS;
      voice_new (m, name, dash + 1, NULL, 0);
    }
  else
    goto skip;   /* Kitten, Kokoro, Piper need a phonemizer: later */

  g_ptr_array_add (models, m);
  return;

skip:
  DBG ("skipping %s", dir);
  g_free (m->name);
  g_free (m->dir);
  g_free (m);
}



static void
scan_models (const gchar *parent)
{
  GDir *d = g_dir_open (parent, 0, NULL);
  const gchar *f;
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);

  if (d == NULL)
    return;
  while ((f = g_dir_read_name (d)) != NULL)
    g_ptr_array_add (names, g_strdup (f));
  g_dir_close (d);
  for (guint i = 0; i < names->len; i++)
    {
      gchar *p = g_build_filename (parent, g_ptr_array_index (names, i), NULL);
      if (g_file_test (p, G_FILE_TEST_IS_DIR))
        add_model (parent, g_ptr_array_index (names, i));
      g_free (p);
    }
  g_ptr_array_free (names, TRUE);
}



static gint
voice_compare (gconstpointer a, gconstpointer b)
{
  const Voice *va = *(Voice * const *) a, *vb = *(Voice * const *) b;

  if (va->model->family != vb->model->family)
    return (int) va->model->family - (int) vb->model->family;
  if (va->model != vb->model)
    return strcmp (va->model->name, vb->model->name);
  if (strcmp (va->language, vb->language) != 0)
    return strcmp (va->language, vb->language);
  return va->sid != vb->sid ? va->sid - vb->sid : g_utf8_collate (va->name, vb->name);
}



static void
load_voices (void)
{
  gchar *user = g_build_filename (g_get_user_data_dir (), "sherpa-onnx", "tts", NULL);

  models = g_ptr_array_new ();
  voices = g_ptr_array_new ();
  scan_models (user);
  scan_models (cfg_models_dir);
  g_free (user);
  g_ptr_array_sort (voices, voice_compare);

  spd_voices = g_new0 (SPDVoice *, voices->len + 1);
  for (guint i = 0; i < voices->len; i++)
    {
      Voice *v = g_ptr_array_index (voices, i);
      spd_voices[i] = g_new0 (SPDVoice, 1);
      spd_voices[i]->name = v->name;
      spd_voices[i]->language = v->language;
      spd_voices[i]->variant = v->gender;
    }
}



static int
wanted_threads (void)
{
  long n = sysconf (_SC_NPROCESSORS_ONLN);

  if (cfg_threads > 0)
    return cfg_threads;
  /* more threads were slower on a 4-core / 8-thread Ryzen */
  return n < 1 ? 1 : (n > 4 ? 4 : (int) n);
}



static gboolean
load_model (Model *m)
{
  SherpaOnnxOfflineTtsConfig c;
  GPtrArray *keep = g_ptr_array_new_with_free_func (g_free);
  gint64 t0 = g_get_monotonic_time ();

#define FILE_(prefix, suffix) ({ gchar *f_ = find_file (m->dir, prefix, suffix); \
                                  g_ptr_array_add (keep, f_); f_; })
#define PATH_(name) ({ gchar *p_ = g_build_filename (m->dir, name, NULL); \
                       g_ptr_array_add (keep, p_); p_; })

  if (loaded_model == m && tts != NULL)
    return TRUE;
  if (tts != NULL)
    {
      SherpaOnnxDestroyOfflineTts (tts);
      tts = NULL;
      loaded_model = NULL;
    }

  memset (&c, 0, sizeof c);
  c.model.num_threads = wanted_threads ();
  c.model.provider = "cpu";
  c.model.debug = debug_on > 1;
  c.max_num_sentences = 1;
  c.silence_scale = 0.2f;
  switch (m->family)
    {
    case FAMILY_SUPERTONIC:
      c.model.supertonic.duration_predictor = FILE_ ("duration_predictor", ".onnx");
      c.model.supertonic.text_encoder = FILE_ ("text_encoder", ".onnx");
      c.model.supertonic.vector_estimator = FILE_ ("vector_estimator", ".onnx");
      c.model.supertonic.vocoder = FILE_ ("vocoder", ".onnx");
      c.model.supertonic.tts_json = PATH_ ("tts.json");
      c.model.supertonic.unicode_indexer = PATH_ ("unicode_indexer.bin");
      c.model.supertonic.voice_style = PATH_ ("voice.bin");
      break;
    case FAMILY_POCKET:
      c.model.pocket.lm_flow = FILE_ ("lm_flow", ".onnx");
      c.model.pocket.lm_main = FILE_ ("lm_main", ".onnx");
      c.model.pocket.encoder = FILE_ ("encoder", ".onnx");
      c.model.pocket.decoder = FILE_ ("decoder", ".onnx");
      c.model.pocket.text_conditioner = FILE_ ("text_conditioner", ".onnx");
      c.model.pocket.vocab_json = PATH_ ("vocab.json");
      c.model.pocket.token_scores_json = PATH_ ("token_scores.json");
      c.model.pocket.voice_embedding_cache_capacity = 8;
      break;
    case FAMILY_VITS:
      c.model.vits.model = FILE_ ("", ".onnx");
      c.model.vits.tokens = PATH_ ("tokens.txt");
      c.model.vits.noise_scale = 0.667f;
      c.model.vits.noise_scale_w = 0.8f;
      c.model.vits.length_scale = 1.0f;
      break;
    }
#undef FILE_
#undef PATH_

  {
    /* Ubuntu's onnxruntime + libonnx print hundreds of "Schema error: ... already
     * registered" lines while a model is loaded; harmless, keep them out of the log */
    int saved = -1, null_fd;

    if (debug_on < 2 && (null_fd = open ("/dev/null", O_WRONLY)) >= 0)
      {
        fflush (stderr);
        saved = dup (STDERR_FILENO);
        dup2 (null_fd, STDERR_FILENO);
        close (null_fd);
      }
    tts = SherpaOnnxCreateOfflineTts (&c);
    if (saved >= 0)
      {
        fflush (stderr);
        dup2 (saved, STDERR_FILENO);
        close (saved);
      }
  }
  g_ptr_array_free (keep, TRUE);
  if (tts == NULL)
    {
      ERR ("cannot load the model %s", m->dir);
      return FALSE;
    }
  loaded_model = m;
  DBG ("loaded %s (%s, %d threads) in %.1f s", m->name, family_names[m->family],
       c.model.num_threads, (g_get_monotonic_time () - t0) / 1e6);
  return TRUE;
}



int
module_init (char **msg)
{
  load_voices ();
  if (voices->len == 0)
    {
      *msg = g_strdup_printf ("no sherpa-onnx TTS models in %s", cfg_models_dir);
      return -1;
    }
  module_audio_set_server ();
  *msg = g_strdup_printf ("sherpa-onnx ready, %u voices", voices->len);
  return 0;
}



SPDVoice **
module_list_voices (void)
{
  return spd_voices;
}



int
module_set (const char *var, const char *val)
{
  if (strcmp (var, "synthesis_voice") == 0)
    {
      g_free (set_voice_name);
      set_voice_name = (val != NULL && strcmp (val, "NULL") != 0) ? g_strdup (val) : NULL;
    }
  else if (strcmp (var, "language") == 0)
    {
      g_free (set_language);
      set_language = (val != NULL && strcmp (val, "NULL") != 0) ? g_strdup (val) : NULL;
    }
  else if (strcmp (var, "voice") == 0 || strcmp (var, "voice_type") == 0)
    {
      /* male1..3, female1..3, child_male, child_female */
      const gchar *t = val != NULL ? val : "";
      set_gender = NULL;
      set_type_index = 0;
      if (g_ascii_strncasecmp (t, "child_", 6) == 0)
        t += 6;
      if (g_ascii_strncasecmp (t, "female", 6) == 0)
        set_gender = "female", t += 6;
      else if (g_ascii_strncasecmp (t, "male", 4) == 0)
        set_gender = "male", t += 4;
      if (g_ascii_isdigit (*t) && atoi (t) > 0)
        set_type_index = atoi (t) - 1;
    }
  else if (strcmp (var, "rate") == 0)
    set_rate = atoi (val);
  else if (strcmp (var, "volume") == 0)
    set_volume = atoi (val);
  /* pitch, pitch_range, punctuation, spelling, capitals: not supported */
  return 0;
}



int
module_audio_set (const char *var, const char *val)
{
  if (strcmp (var, "audio_output_method") == 0)
    return strcmp (val, "server") == 0 ? 0 : -1;
  return 0;
}



int
module_audio_init (char **status)
{
  *status = strdup ("audio through the server");
  return 0;
}



int
module_loglevel_set (const char *var, const char *val)
{
  return 0;
}



int
module_debug (int enable, const char *file)
{
  debug_on = enable;
  return 0;
}



/* -------------------------------------------------------------------- text */

/* SSML (speech-dispatcher wraps all text in <speak>) to plain text */
static gchar *
ssml_to_text (const char *data, size_t bytes)
{
  GString *out = g_string_sized_new (bytes);
  const char *p = data, *end = data + bytes;

  while (p < end && *p != '\0')
    {
      if (*p == '<')
        {
          const char *close = memchr (p, '>', end - p);
          if (close == NULL)
            break;
          g_string_append_c (out, ' ');
          p = close + 1;
        }
      else if (*p == '&')
        {
          static const struct { const char *ent; char ch; } ents[] = {
            { "&lt;", '<' }, { "&gt;", '>' }, { "&amp;", '&' },
            { "&quot;", '"' }, { "&apos;", '\'' }
          };
          gboolean done = FALSE;
          for (guint i = 0; i < G_N_ELEMENTS (ents); i++)
            if (strncmp (p, ents[i].ent, strlen (ents[i].ent)) == 0)
              {
                g_string_append_c (out, ents[i].ch);
                p += strlen (ents[i].ent);
                done = TRUE;
                break;
              }
          if (!done)
            g_string_append_c (out, *p++);
        }
      else
        g_string_append_c (out, *p++);
    }
  return g_strstrip (g_string_free (out, FALSE));
}



/* Cuts the text after . ! ? … ; and line breaks; long pieces also after commas.
 * The first piece is cut at a comma already after cfg_first_chunk characters. */
static GPtrArray *
split_sentences (const gchar *text)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  GString *cur = g_string_new (NULL);

  for (const gchar *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      gunichar next = g_utf8_get_char (g_utf8_next_char (p));
      gboolean end_here = FALSE;

      if (c == '\n')
        {
          end_here = TRUE;
          c = ' ';
        }
      g_string_append_unichar (cur, c);
      if (!end_here && (next == ' ' || next == '\n' || next == '\0'))
        {
          if (c == '.' || c == '!' || c == '?' || c == 0x2026 || c == ';')
            end_here = TRUE;
          else if ((c == ',' || c == ':' || c == 0x2014)
                   && g_utf8_strlen (cur->str, -1)
                      > (out->len == 0 && cfg_first_chunk > 0 ? cfg_first_chunk : 200))
            end_here = TRUE;
        }
      if (end_here)
        {
          g_strstrip (cur->str);
          if (*cur->str != '\0')
            g_ptr_array_add (out, g_strdup (cur->str));
          g_string_truncate (cur, 0);
        }
    }
  g_strstrip (cur->str);
  if (*cur->str != '\0')
    g_ptr_array_add (out, g_strdup (cur->str));
  g_string_free (cur, TRUE);
  return out;
}



static Voice *
voice_by_name (const gchar *name)
{
  for (guint i = 0; name != NULL && i < voices->len; i++)
    {
      Voice *v = g_ptr_array_index (voices, i);
      if (g_ascii_strcasecmp (v->name, name) == 0)
        return v;
    }
  return NULL;
}



static Voice *
pick_voice (void)
{
  gchar *lang = NULL;
  Voice *found = voice_by_name (set_voice_name);

  if (set_language != NULL)
    lang = g_ascii_strdown (set_language, strcspn (set_language, "-_"));

  /* the voice chosen for the language in the configuration */
  if (found == NULL && cfg_lang_voice != NULL && lang != NULL)
    found = voice_by_name (g_hash_table_lookup (cfg_lang_voice, lang));

  /* the voices of the language (else of English, else all), in the order of
   * the families; among them the n-th one of the voice type's gender */
  if (found == NULL)
    {
      const gchar *langs[] = { lang, "en" };
      GPtrArray *cand = g_ptr_array_new ();
      GPtrArray *same = g_ptr_array_new ();
      Model *first = NULL;

      for (guint l = 0; l < 2 && cand->len == 0; l++)
        for (guint i = 0; i < voices->len && langs[l] != NULL; i++)
          {
            Voice *v = g_ptr_array_index (voices, i);
            if (strcmp (v->language, langs[l]) == 0)
              g_ptr_array_add (cand, v);
          }
      if (cand->len == 0)
        for (guint i = 0; i < voices->len; i++)
          g_ptr_array_add (cand, g_ptr_array_index (voices, i));
      /* only the first (preferred) model of the language */
      first = ((Voice *) g_ptr_array_index (cand, 0))->model;
      for (guint i = 0; i < cand->len; i++)
        {
          Voice *v = g_ptr_array_index (cand, i);
          if (v->model == first && set_gender != NULL && g_strcmp0 (v->gender, set_gender) == 0)
            g_ptr_array_add (same, v);
        }
      if (same->len > 0)
        found = g_ptr_array_index (same, set_type_index % same->len);
      else
        {
          guint n = 0;
          while (n < cand->len && ((Voice *) g_ptr_array_index (cand, n))->model == first)
            n++;
          found = g_ptr_array_index (cand, set_type_index % n);
        }
      g_ptr_array_free (same, TRUE);
      g_ptr_array_free (cand, TRUE);
    }
  g_free (lang);
  return found;
}



/* ------------------------------------------------------------------ speech */

/* Lets the speech-dispatcher server interrupt us (STOP) while we synthesize. */
static void
process_input (void)
{
  struct pollfd fd = { STDIN_FILENO, POLLIN, 0 };

  while (!quit_requested && poll (&fd, 1, 0) > 0 && (fd.revents & (POLLIN | POLLHUP)))
    if (module_process (STDIN_FILENO, 0) == 0)
      {
        /* QUIT while we speak: finish the message, module_loop then quits */
        quit_requested = TRUE;
        stop_requested = 1;
      }
}



static int32_t
progress_cb (const float *samples, int32_t n, float p, void *arg)
{
  process_input ();
  return stop_requested ? 0 : 1;
}



static void
send_audio (const float *f, size_t n, int sample_rate)
{
  short *s;
  double gain = CLAMP ((set_volume + 100) / 100.0, 0.0, 2.0);
  AudioTrack track;

  if (n == 0)
    return;
  s = g_new (short, n);
  for (size_t i = 0; i < n; i++)
    s[i] = (short) (CLAMP (f[i] * gain, -1.0, 1.0) * 32767.0);
  track.bits = 16;
  track.num_channels = 1;
  track.sample_rate = sample_rate;
  track.num_samples = (int) n;
  track.samples = s;
  module_tts_output_server (&track, SPD_AUDIO_LE);
  g_free (s);
}



static void
speak_sentence (const gchar *text, Voice *v)
{
  SherpaOnnxGenerationConfig g;
  const SherpaOnnxGeneratedAudio *audio;
  gchar *extra = NULL, *lowered = NULL;
  /* rate -100..100 -> half .. double speed */
  float speed = (float) pow (2.0, CLAMP (set_rate, -100, 100) / 100.0);
  gint64 t0 = g_get_monotonic_time ();

  memset (&g, 0, sizeof g);
  g.sid = v->sid;
  g.silence_scale = 0.2f;
  switch (v->model->family)
    {
    case FAMILY_SUPERTONIC:
      /* 1.05 is Supertonic's own normal speed */
      g.speed = 1.05f * speed;
      g.num_steps = cfg_steps > 0 ? cfg_steps : 5;
      extra = g_strdup_printf ("{\"lang\": \"%s\"}", v->language);
      g.extra = extra;
      break;
    case FAMILY_POCKET:
      g.speed = speed;
      if (v->ref == NULL)
        v->ref = SherpaOnnxReadWave (v->wav);
      if (v->ref == NULL)
        {
          ERR ("cannot read %s", v->wav);
          return;
        }
      g.reference_audio = v->ref->samples;
      g.reference_audio_len = v->ref->num_samples;
      g.reference_sample_rate = v->ref->sample_rate;
      break;
    case FAMILY_VITS:
      g.speed = speed;
      /* MMS knows only lower case */
      text = lowered = g_utf8_strdown (text, -1);
      break;
    }

  audio = SherpaOnnxOfflineTtsGenerateWithConfig (tts, text, &g, progress_cb, NULL);
  if (audio != NULL)
    {
      DBG ("%.2f s of speech in %.2f s: %s", audio->n / (double) audio->sample_rate,
           (g_get_monotonic_time () - t0) / 1e6, text);
      if (!stop_requested)
        send_audio (audio->samples, audio->n, audio->sample_rate);
      SherpaOnnxDestroyOfflineTtsGeneratedAudio (audio);
    }
  else
    ERR ("synthesis failed: %s", text);
  g_free (extra);
  g_free (lowered);
}



void
module_speak_sync (const char *data, size_t bytes, SPDMessageType msgtype)
{
  gchar *text;
  GPtrArray *sentences;
  Voice *voice;

  stop_requested = 0;
  text = ssml_to_text (data, bytes);
  if (*text == '\0')
    {
      /* nothing to say: finish the message at once, an error would leave
       * clients waiting for its end */
      g_free (text);
      module_speak_ok ();
      module_report_event_begin ();
      module_report_event_end ();
      return;
    }
  module_speak_ok ();
  module_report_event_begin ();

  voice = pick_voice ();
  sentences = split_sentences (text);
  DBG ("speaking %u sentence(s) with %s: %s", sentences->len, voice->name, text);

  if (load_model (voice->model))
    for (guint i = 0; i < sentences->len && !stop_requested; i++)
      {
        process_input ();
        if (!stop_requested)
          speak_sentence (g_ptr_array_index (sentences, i), voice);
      }

  g_ptr_array_free (sentences, TRUE);
  g_free (text);
  if (stop_requested)
    module_report_event_stop ();
  else
    module_report_event_end ();
}



size_t
module_pause (void)
{
  stop_requested = 1;
  return 0;
}



int
module_stop (void)
{
  stop_requested = 1;
  return 0;
}



int
module_close (void)
{
  if (tts != NULL)
    SherpaOnnxDestroyOfflineTts (tts);
  tts = NULL;
  return 0;
}



int
module_loop (void)
{
  if (cfg_preload != NULL)
    {
      /* SherpaPreload names a voice or a model */
      Voice *v = voice_by_name (cfg_preload);
      Model *m = v != NULL ? v->model : NULL;

      for (guint i = 0; i < models->len && m == NULL; i++)
        if (strcmp (((Model *) g_ptr_array_index (models, i))->name, cfg_preload) == 0)
          m = g_ptr_array_index (models, i);
      if (m != NULL)
        load_model (m);
      else
        ERR ("SherpaPreload: no voice or model %s", cfg_preload);
    }
  for (;;)
    {
      struct pollfd fd = { STDIN_FILENO, POLLIN, 0 };
      int ret = poll (&fd, 1, -1);

      if (ret < 0 && errno != EINTR)
        return -1;
      if (ret > 0)
        {
          if ((fd.revents & POLLIN) && module_process (STDIN_FILENO, 0) == 0)
            quit_requested = TRUE;
          if (quit_requested)
            {
              module_close ();
              return 0;
            }
          if (fd.revents & (POLLHUP | POLLERR))
            {
              module_close ();
              return -1;
            }
        }
    }
}

/*
 * sd_kokoro - speech-dispatcher output module for Kokoro voices
 *
 * The models are the directories in KokoroModelsDir (and
 * ~/.local/share/kokoro-tts) named kokoro-<lang>[-...] with config.json
 * (the "vocab"), model.onnx and voices/<name>.bin.  voices.txt lists the
 * voices as "<name> <male|female|-> [<onnx file>]" (a voice may need its own
 * fine-tuned model); without it every voices/<name>.bin is a voice of
 * model.onnx.  A voice is called kokoro-<lang>-<name>.  Only Russian has a
 * text front end so far (ru-g2p.c: stress marks from the Vosk dictionary,
 * espeak-ng with an acute-aware ru_dict).
 *
 * Loading a model takes a few seconds, so the model of the default voice
 * starts loading in a thread when the module starts; a message waits for
 * it.  One model is kept at a time.
 *
 * The text is cut into sentences; each one is synthesized and handed to the
 * speech-dispatcher server at once, so the next one is synthesized while the
 * previous one plays.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <speech-dispatcher/spd_module_main.h>

#include "kokoro.h"
#include "ru-g2p.h"

#define DBG(...) do { if (debug_on) { fprintf (stderr, "sd_kokoro: " __VA_ARGS__); fputc ('\n', stderr); } } while (0)
#define ERR(...) do { fprintf (stderr, "sd_kokoro: " __VA_ARGS__); fputc ('\n', stderr); } while (0)

typedef struct
{
  gchar *onnx;              /* path */
  gchar *config;            /* config.json */
  KokoroModel *km;          /* loaded, guarded by load_lock */
} Model;

typedef struct
{
  gchar *name;              /* kokoro-ru-sveta */
  gchar *language;          /* ru */
  gchar *gender;            /* "male", "female" or NULL */
  Model *model;
  gfloat *style;            /* [510][256] */
} Voice;

/* configuration */
static gchar *cfg_models_dir = NULL;
static gchar *cfg_dictionary = NULL;
static gchar *cfg_espeak_data = NULL;
static GHashTable *cfg_lang_voice = NULL;  /* KokoroVoice "<lang>" "<voice>" */
static int cfg_threads = 0;
static int cfg_first_chunk = 40;
static gboolean cfg_preload = TRUE;
static int debug_on = 0;
/* module_close called by module_loop, not as the reply to QUIT */
static gboolean closing_from_loop;

static GPtrArray *models = NULL;           /* Model */
static GPtrArray *voices = NULL;           /* Voice */
static SPDVoice **spd_voices = NULL;
static RuG2p *g2p = NULL;

/* message settings */
static gchar *set_voice_name = NULL;
static const gchar *set_gender = NULL;
static int set_type_index = 0;
static gchar *set_language = NULL;
static int set_rate = 0;
static int set_volume = 0;

/* loading: the model being loaded or loaded, guarded by load_lock */
static GMutex load_lock;
static GCond load_cond;
static Model *wanted_model = NULL;    /* what the thread loads */
static Model *loaded_model = NULL;
static gboolean loading = FALSE;
static GThread *load_thread = NULL;

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



static gboolean
parse_bool (const gchar *v)
{
  return g_ascii_strcasecmp (v, "1") == 0 || g_ascii_strcasecmp (v, "yes") == 0
         || g_ascii_strcasecmp (v, "true") == 0 || g_ascii_strcasecmp (v, "on") == 0;
}



int
module_config (const char *configfile)
{
  gchar *contents = NULL;
  gchar **lines;

  cfg_models_dir = g_strdup ("/usr/share/kokoro-tts");
  cfg_dictionary = g_strdup ("/usr/share/vosk-tts/vosk-model-tts-ru-0.9-multi/dictionary.sorted");
  cfg_espeak_data = g_strdup ("/usr/share/speech-dispatcher-kokoro");

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

      if (g_ascii_strcasecmp (line, "KokoroModelsDir") == 0)
        g_free (cfg_models_dir), cfg_models_dir = g_strdup (value);
      else if (g_ascii_strcasecmp (line, "KokoroDictionary") == 0)
        g_free (cfg_dictionary), cfg_dictionary = g_strdup (value);
      else if (g_ascii_strcasecmp (line, "KokoroEspeakData") == 0)
        g_free (cfg_espeak_data), cfg_espeak_data = g_strdup (value);
      else if (g_ascii_strcasecmp (line, "KokoroThreads") == 0)
        cfg_threads = atoi (value);
      else if (g_ascii_strcasecmp (line, "KokoroFirstChunk") == 0)
        cfg_first_chunk = atoi (value);
      else if (g_ascii_strcasecmp (line, "KokoroPreload") == 0)
        cfg_preload = parse_bool (value);
      else if (g_ascii_strcasecmp (line, "KokoroVoice") == 0)
        {
          /* value was unquoted as one string: ru" "kokoro-ru-sveta */
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

static Model *
model_get (const gchar *onnx, const gchar *config)
{
  Model *m;

  for (guint i = 0; i < models->len; i++)
    {
      m = g_ptr_array_index (models, i);
      if (strcmp (m->onnx, onnx) == 0)
        return m;
    }
  m = g_new0 (Model, 1);
  m->onnx = g_strdup (onnx);
  m->config = g_strdup (config);
  g_ptr_array_add (models, m);
  return m;
}



static gboolean
voice_exists (const gchar *name)
{
  for (guint i = 0; i < voices->len; i++)
    if (strcmp (((Voice *) g_ptr_array_index (voices, i))->name, name) == 0)
      return TRUE;
  return FALSE;
}



static void
add_voice (const gchar *dir, const gchar *lang, const gchar *name,
           const gchar *gender, const gchar *onnx_file)
{
  GError *error = NULL;
  gchar *full = g_strdup_printf ("kokoro-%s-%s", lang, name);
  gchar *bin = g_strdup_printf ("%s.bin", name);
  gchar *style_path = g_build_filename (dir, "voices", bin, NULL);
  gchar *onnx = g_build_filename (dir, onnx_file != NULL ? onnx_file : "model.onnx", NULL);
  gchar *config = g_build_filename (dir, "config.json", NULL);
  gfloat *style = NULL;
  Voice *v;

  /* a user's voice hides a system one of the same name */
  if (voice_exists (full))
    goto out;
  if (!g_file_test (onnx, G_FILE_TEST_EXISTS))
    {
      DBG ("skipping %s: no %s", full, onnx);
      goto out;
    }
  style = kokoro_voice_load (style_path, &error);
  if (style == NULL)
    {
      DBG ("skipping %s: %s", full, error->message);
      g_error_free (error);
      goto out;
    }
  v = g_new0 (Voice, 1);
  v->name = full;
  full = NULL;
  v->language = g_strdup (lang);
  if (gender != NULL && (strcmp (gender, "male") == 0 || strcmp (gender, "female") == 0))
    v->gender = g_strdup (gender);
  v->model = model_get (onnx, config);
  v->style = style;
  g_ptr_array_add (voices, v);
out:
  g_free (full);
  g_free (bin);
  g_free (style_path);
  g_free (onnx);
  g_free (config);
}



static void
add_model_dir (const gchar *dir, const gchar *base)
{
  gchar *lang, *list, *contents = NULL;
  const gchar *p = base + strlen ("kokoro-");

  lang = g_strndup (p, strcspn (p, "-_"));
  if (strcmp (lang, "ru") != 0)
    {
      DBG ("skipping %s: no text front end for \"%s\"", dir, lang);
      g_free (lang);
      return;
    }
  list = g_build_filename (dir, "voices.txt", NULL);
  if (g_file_get_contents (list, &contents, NULL, NULL))
    {
      gchar **lines = g_strsplit (contents, "\n", -1);
      for (gchar **l = lines; *l != NULL; l++)
        {
          gchar **f;
          gint n;
          g_strstrip (*l);
          if (**l == '\0' || **l == '#')
            continue;
          if (!g_shell_parse_argv (*l, &n, &f, NULL))
            continue;
          add_voice (dir, lang, f[0], n > 1 ? f[1] : NULL, n > 2 ? f[2] : NULL);
          g_strfreev (f);
        }
      g_strfreev (lines);
      g_free (contents);
    }
  else
    {
      gchar *vdir = g_build_filename (dir, "voices", NULL);
      GDir *d = g_dir_open (vdir, 0, NULL);
      GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
      const gchar *f;
      while (d != NULL && (f = g_dir_read_name (d)) != NULL)
        if (g_str_has_suffix (f, ".bin"))
          g_ptr_array_add (names, g_strndup (f, strlen (f) - 4));
      if (d != NULL)
        g_dir_close (d);
      g_ptr_array_sort (names, (GCompareFunc) g_strcmp0);
      for (guint i = 0; i < names->len; i++)
        add_voice (dir, lang, g_ptr_array_index (names, i), NULL, NULL);
      g_ptr_array_free (names, TRUE);
      g_free (vdir);
    }
  g_free (list);
  g_free (lang);
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
    if (g_str_has_prefix (f, "kokoro-"))
      g_ptr_array_add (names, g_strdup (f));
  g_dir_close (d);
  g_ptr_array_sort (names, (GCompareFunc) g_strcmp0);
  for (guint i = 0; i < names->len; i++)
    {
      gchar *p = g_build_filename (parent, g_ptr_array_index (names, i), NULL);
      gchar *cfg = g_build_filename (p, "config.json", NULL);
      if (g_file_test (cfg, G_FILE_TEST_EXISTS))
        add_model_dir (p, g_ptr_array_index (names, i));
      g_free (cfg);
      g_free (p);
    }
  g_ptr_array_free (names, TRUE);
}



static void
load_voices (void)
{
  gchar *user = g_build_filename (g_get_user_data_dir (), "kokoro-tts", NULL);

  models = g_ptr_array_new ();
  voices = g_ptr_array_new ();
  scan_models (user);
  scan_models (cfg_models_dir);
  g_free (user);

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



static Voice *
first_voice_of (Model *m)
{
  for (guint i = 0; i < voices->len; i++)
    if (((Voice *) g_ptr_array_index (voices, i))->model == m)
      return g_ptr_array_index (voices, i);
  return NULL;
}



/* runs in load_thread: loads wanted_model, unloading the previous one */
static gpointer
load_worker (gpointer data)
{
  g_mutex_lock (&load_lock);
  for (;;)
    {
      Model *m = wanted_model, *old = loaded_model;
      GError *error = NULL;
      KokoroModel *km;
      gint64 t0 = g_get_monotonic_time ();
      int saved = -1, null_fd;

      if (m == NULL || m == loaded_model)
        break;
      loaded_model = NULL;
      g_mutex_unlock (&load_lock);

      if (old != NULL)
        {
          kokoro_model_free (old->km);
          old->km = NULL;
        }
      /* Ubuntu's onnxruntime + libonnx print hundreds of "Schema error: ... already
       * registered" lines; harmless, keep them out of the log */
      if (debug_on < 2 && (null_fd = open ("/dev/null", O_WRONLY)) >= 0)
        {
          fflush (stderr);
          saved = dup (STDERR_FILENO);
          dup2 (null_fd, STDERR_FILENO);
          close (null_fd);
        }
      km = kokoro_model_new (m->onnx, m->config, wanted_threads (), &error);
      if (saved >= 0)
        {
          fflush (stderr);
          dup2 (saved, STDERR_FILENO);
          close (saved);
        }
      if (km != NULL)
        {
          /* the first run allocates the buffers; pay it here, not on the
           * first message ("привет, как дела?") */
          gsize n;
          g_free (kokoro_synth (km, first_voice_of (m)->style, "prʲivʲˈet, kˈɑk dʲɪlˈɑ?",
                                1.0f, &n, NULL));
          DBG ("loaded %s (%d threads) in %.1f s", m->onnx, wanted_threads (),
               (g_get_monotonic_time () - t0) / 1e6);
        }
      else
        {
          ERR ("cannot load %s: %s", m->onnx, error->message);
          g_error_free (error);
        }

      g_mutex_lock (&load_lock);
      m->km = km;
      if (km != NULL)
        loaded_model = m;
      else if (wanted_model == m)
        wanted_model = NULL;   /* do not retry it for every message */
      g_cond_broadcast (&load_cond);
    }
  loading = FALSE;
  g_cond_broadcast (&load_cond);
  g_mutex_unlock (&load_lock);
  return NULL;
}



/* asks for m to be loaded; the caller holds load_lock */
static void
request_model_locked (Model *m)
{
  wanted_model = m;
  if (m == loaded_model || loading)
    return;
  if (load_thread != NULL)
    g_thread_join (load_thread);
  loading = TRUE;
  load_thread = g_thread_new ("kokoro-load", load_worker, NULL);
}



int
module_init (char **msg)
{
  GError *error = NULL;
  gchar *dict;

  load_voices ();
  if (voices->len == 0)
    {
      *msg = g_strdup_printf ("no Kokoro voices in %s", cfg_models_dir);
      return -1;
    }
  dict = g_file_test (cfg_dictionary, G_FILE_TEST_EXISTS) ? cfg_dictionary : NULL;
  if (dict == NULL)
    ERR ("no stress dictionary %s, the stress will be guessed", cfg_dictionary);
  g2p = ru_g2p_new (dict, cfg_espeak_data, &error);
  if (g2p == NULL)
    {
      *msg = g_strdup (error->message);
      g_error_free (error);
      return -1;
    }
  module_audio_set_server ();
  *msg = g_strdup_printf ("Kokoro ready, %u voices", voices->len);
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



/* the synthesis voice, else the language's voice from the configuration,
 * else the n-th voice of the voice type's gender of the language (else of
 * any language) */
static Voice *
pick_voice (void)
{
  gchar *lang = NULL;
  Voice *found = voice_by_name (set_voice_name);

  if (set_language != NULL)
    lang = g_ascii_strdown (set_language, strcspn (set_language, "-_"));

  if (found == NULL && cfg_lang_voice != NULL && lang != NULL)
    found = voice_by_name (g_hash_table_lookup (cfg_lang_voice, lang));

  if (found == NULL)
    {
      GPtrArray *cand = g_ptr_array_new ();
      GPtrArray *same = g_ptr_array_new ();

      for (guint i = 0; i < voices->len && lang != NULL; i++)
        {
          Voice *v = g_ptr_array_index (voices, i);
          if (strcmp (v->language, lang) == 0)
            g_ptr_array_add (cand, v);
        }
      if (cand->len == 0)
        for (guint i = 0; i < voices->len; i++)
          g_ptr_array_add (cand, g_ptr_array_index (voices, i));
      for (guint i = 0; i < cand->len; i++)
        {
          Voice *v = g_ptr_array_index (cand, i);
          if (set_gender != NULL && g_strcmp0 (v->gender, set_gender) == 0)
            g_ptr_array_add (same, v);
        }
      if (same->len > 0)
        found = g_ptr_array_index (same, set_type_index % same->len);
      else
        found = g_ptr_array_index (cand, 0);
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
    if (module_process (STDIN_FILENO, 0) == 0 || (fd.revents & (POLLHUP | POLLERR)))
      {
        /* QUIT while we speak, or the server is gone (the pipe stays
         * readable at its end, so polling again would spin): stop the
         * message, module_loop then quits */
        quit_requested = TRUE;
        stop_requested = 1;
      }
}



/* waits until m is loaded; FALSE if it failed or the message was stopped */
static gboolean
wait_for_model (Model *m)
{
  gboolean ok;

  g_mutex_lock (&load_lock);
  request_model_locked (m);
  while (loaded_model != m && wanted_model == m && !stop_requested)
    {
      gint64 until = g_get_monotonic_time () + 100 * G_TIME_SPAN_MILLISECOND;
      if (!loading)
        break;
      g_cond_wait_until (&load_cond, &load_lock, until);
      g_mutex_unlock (&load_lock);
      process_input ();
      g_mutex_lock (&load_lock);
    }
  ok = loaded_model == m;
  g_mutex_unlock (&load_lock);
  return ok;
}



static void
send_audio (const float *f, size_t n)
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
  track.sample_rate = KOKORO_SAMPLE_RATE;
  track.num_samples = (int) n;
  track.samples = s;
  module_tts_output_server (&track, SPD_AUDIO_LE);
  g_free (s);
}



/* where to cut a text that is too long: the last comma-like mark before the
 * middle, else the last space before it, else the middle */
static glong
cut_point (const gchar *text)
{
  glong n = g_utf8_strlen (text, -1), half = n / 2, space = -1, mark = -1, i = 0;

  for (const gchar *p = text; *p != '\0' && i < half; p = g_utf8_next_char (p), i++)
    {
      gunichar c = g_utf8_get_char (p);
      if (c == ' ')
        {
          space = i;
          if (i > 0)
            {
              gunichar prev = g_utf8_get_char (g_utf8_prev_char (p));
              if (prev == ',' || prev == ':' || prev == ';' || prev == 0x2014)
                mark = i;
            }
        }
    }
  if (mark > n / 4)
    return mark;
  return space > 0 ? space : MAX (half, 1);
}



static void
speak_text (const gchar *text, Voice *v)
{
  GError *error = NULL;
  /* rate -100..100 -> half .. double speed */
  float speed = (float) pow (2.0, CLAMP (set_rate, -100, 100) / 100.0);
  gint64 t0 = g_get_monotonic_time ();
  gchar *ipa = ru_g2p_phonemize (g2p, text);
  glong len = g_utf8_strlen (ipa, -1);
  gsize n = 0;
  float *wav;

  if (len > KOKORO_MAX_PHONEMES && g_utf8_strlen (text, -1) > 1)
    {
      /* Kokoro takes at most 510 phonemes: say it in two halves */
      glong cut = cut_point (text);
      const gchar *mid = g_utf8_offset_to_pointer (text, cut);
      gchar *first = g_strndup (text, mid - text);
      g_free (ipa);
      speak_text (g_strstrip (first), v);
      g_free (first);
      if (!stop_requested)
        {
          gchar *rest = g_strstrip (g_strdup (mid));
          process_input ();
          if (!stop_requested && *rest != '\0')
            speak_text (rest, v);
          g_free (rest);
        }
      return;
    }
  if (len == 0)
    {
      g_free (ipa);
      return;
    }
  if (len > KOKORO_MAX_PHONEMES)
    *g_utf8_offset_to_pointer (ipa, KOKORO_MAX_PHONEMES) = '\0';
  wav = kokoro_synth (v->model->km, v->style, ipa, speed, &n, &error);
  if (wav == NULL)
    {
      ERR ("synthesis failed: %s: %s", error->message, text);
      g_error_free (error);
      g_free (ipa);
      return;
    }
  DBG ("%.2f s of speech in %.2f s: %s [%s]", n / (double) KOKORO_SAMPLE_RATE,
       (g_get_monotonic_time () - t0) / 1e6, text, ipa);
  if (!stop_requested)
    send_audio (wav, n);
  g_free (wav);
  g_free (ipa);
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

  if (wait_for_model (voice->model))
    for (guint i = 0; i < sentences->len && !stop_requested; i++)
      {
        process_input ();
        if (!stop_requested)
          speak_text (g_ptr_array_index (sentences, i), voice);
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
  gboolean busy;

  g_mutex_lock (&load_lock);
  wanted_model = loaded_model;   /* no further loads */
  busy = loading;
  g_mutex_unlock (&load_lock);
  if (busy)
    {
      /* ORT cannot be interrupted while it loads a model; speechd waits for
       * the reply to QUIT meanwhile (every SIGHUP closes all modules), so
       * answer now and leave the memory to the kernel */
      DBG ("quitting while a model loads");
      if (!closing_from_loop)
        fputs ("210 OK QUIT\n", stdout);
      fflush (stdout);
      _exit (0);
    }
  if (load_thread != NULL)
    g_thread_join (load_thread);
  load_thread = NULL;
  if (loaded_model != NULL)
    {
      kokoro_model_free (loaded_model->km);
      loaded_model->km = NULL;
    }
  loaded_model = NULL;
  ru_g2p_free (g2p);
  g2p = NULL;
  return 0;
}



int
module_loop (void)
{
  if (cfg_preload)
    {
      /* the voice that a message without a voice would get */
      Voice *v = NULL;
      g_free (set_language);
      set_language = NULL;
      if (cfg_lang_voice != NULL)
        {
          GHashTableIter it;
          gpointer value;
          g_hash_table_iter_init (&it, cfg_lang_voice);
          while (v == NULL && g_hash_table_iter_next (&it, NULL, &value))
            v = voice_by_name (value);
        }
      if (v == NULL)
        v = pick_voice ();
      g_mutex_lock (&load_lock);
      request_model_locked (v->model);
      g_mutex_unlock (&load_lock);
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
              closing_from_loop = TRUE;
              module_close ();
              return 0;
            }
          if (fd.revents & (POLLHUP | POLLERR))
            {
              closing_from_loop = TRUE;
              module_close ();
              return -1;
            }
        }
    }
}

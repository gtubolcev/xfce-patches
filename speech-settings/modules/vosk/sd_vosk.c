/*
 * sd_vosk - speech-dispatcher output module for Vosk TTS models (alphacep)
 *
 * The models are the directories in VoskModelsDir (and
 * ~/.local/share/vosk-tts) with a config.json of the "multistream_v1" type,
 * like vosk-model-tts-ru-0.9-multi.  The language comes from the directory
 * name (tts-ru- -> ru); each speaker of the model is a voice:
 * vosk-ru-female-1 for female_0.
 *
 * Loading a model takes long (ONNX Runtime prepares a big graph), so the
 * model of the default voice starts loading in a thread when the module
 * starts; a message waits for it.  One model is kept at a time.
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <speech-dispatcher/spd_module_main.h>

#include "vosk.h"

#define DBG(...) do { if (debug_on) { fprintf (stderr, "sd_vosk: " __VA_ARGS__); fputc ('\n', stderr); } } while (0)
#define ERR(...) do { fprintf (stderr, "sd_vosk: " __VA_ARGS__); fputc ('\n', stderr); } while (0)

typedef struct
{
  gchar *name;      /* vosk-ru-female-1 */
  gchar *language;  /* ru */
  gchar *gender;    /* "male", "female" or NULL */
  VoskModel *model;
  int sid;
} Voice;

/* configuration */
static gchar *cfg_models_dir = NULL;
static GHashTable *cfg_lang_voice = NULL;  /* VoskVoice "<lang>" "<voice>" */
static int cfg_threads = 0;
/* module_close called by module_loop, not as the reply to QUIT */
static gboolean closing_from_loop;
static gboolean cfg_bert = TRUE;
static int cfg_first_chunk = 40;
static gboolean cfg_preload = TRUE;
static int debug_on = 0;

static GPtrArray *models = NULL;           /* VoskModel */
static GPtrArray *voices = NULL;
static SPDVoice **spd_voices = NULL;

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
static VoskModel *wanted_model = NULL;    /* what the thread loads */
static VoskModel *loaded_model = NULL;
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

  cfg_models_dir = g_strdup ("/usr/share/vosk-tts");

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

      if (g_ascii_strcasecmp (line, "VoskModelsDir") == 0)
        g_free (cfg_models_dir), cfg_models_dir = g_strdup (value);
      else if (g_ascii_strcasecmp (line, "VoskThreads") == 0)
        cfg_threads = atoi (value);
      else if (g_ascii_strcasecmp (line, "VoskBert") == 0)
        cfg_bert = parse_bool (value);
      else if (g_ascii_strcasecmp (line, "VoskFirstChunk") == 0)
        cfg_first_chunk = atoi (value);
      else if (g_ascii_strcasecmp (line, "VoskPreload") == 0)
        cfg_preload = parse_bool (value);
      else if (g_ascii_strcasecmp (line, "VoskVoice") == 0)
        {
          /* value was unquoted as one string: ru" "vosk-ru-female-1 */
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

static void
add_model (const gchar *dir)
{
  GError *error = NULL;
  gchar *base = g_path_get_basename (dir);
  VoskModel *m;
  GPtrArray *speakers;
  gboolean first_of_language = TRUE;

  /* a user's model hides a system one of the same name */
  for (guint i = 0; i < models->len; i++)
    {
      gchar *other = g_path_get_basename (vosk_model_dir (g_ptr_array_index (models, i)));
      gboolean same = strcmp (other, base) == 0;
      g_free (other);
      if (same)
        {
          g_free (base);
          return;
        }
    }

  m = vosk_model_open (dir, &error);
  if (m == NULL)
    {
      DBG ("skipping %s: %s", dir, error->message);
      g_error_free (error);
      g_free (base);
      return;
    }
  for (guint i = 0; i < models->len; i++)
    if (strcmp (vosk_model_language (g_ptr_array_index (models, i)), vosk_model_language (m)) == 0)
      first_of_language = FALSE;
  g_ptr_array_add (models, m);

  speakers = vosk_model_speakers (m);
  for (guint i = 0; i < speakers->len; i++)
    {
      VoskSpeaker *s = g_ptr_array_index (speakers, i);
      Voice *v = g_new0 (Voice, 1);
      const gchar *us = strrchr (s->name, '_');
      gchar *stem;

      v->language = g_strdup (vosk_model_language (m));
      v->model = m;
      v->sid = s->id;
      /* female_0 -> female-1 */
      if (us != NULL && g_ascii_isdigit (us[1]))
        {
          gchar *g = g_strndup (s->name, us - s->name);
          if (strcmp (g, "female") == 0 || strcmp (g, "male") == 0)
            v->gender = g_strdup (g);
          stem = g_strdup_printf ("%s-%d", g, atoi (us + 1) + 1);
          g_free (g);
        }
      else
        stem = g_strdup (s->name);
      /* the second model of a language gets its directory name in the voices */
      v->name = first_of_language
                ? g_strdup_printf ("vosk-%s-%s", v->language, stem)
                : g_strdup_printf ("vosk-%s-%s-%s", v->language, base, stem);
      g_free (stem);
      g_ptr_array_add (voices, v);
    }
  g_free (base);
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
  g_ptr_array_sort (names, (GCompareFunc) g_strcmp0);
  for (guint i = 0; i < names->len; i++)
    {
      gchar *p = g_build_filename (parent, g_ptr_array_index (names, i), NULL);
      gchar *cfg = g_build_filename (p, "config.json", NULL);
      if (g_file_test (cfg, G_FILE_TEST_EXISTS))
        add_model (p);
      g_free (cfg);
      g_free (p);
    }
  g_ptr_array_free (names, TRUE);
}



static void
load_voices (void)
{
  gchar *user = g_build_filename (g_get_user_data_dir (), "vosk-tts", NULL);

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



/* runs in load_thread: loads wanted_model, unloading the previous one */
static gpointer
load_worker (gpointer data)
{
  g_mutex_lock (&load_lock);
  for (;;)
    {
      VoskModel *m = wanted_model, *old = loaded_model;
      GError *error = NULL;
      gchar *cache;
      gint64 t0 = g_get_monotonic_time ();
      gboolean ok;
      int saved = -1, null_fd;

      if (m == NULL || m == loaded_model)
        break;
      loaded_model = NULL;
      g_mutex_unlock (&load_lock);

      if (old != NULL)
        vosk_model_unload (old);
      cache = g_build_filename (g_get_user_cache_dir (), "sd_vosk", NULL);
      /* Ubuntu's onnxruntime + libonnx print hundreds of "Schema error: ... already
       * registered" lines; harmless, keep them out of the log */
      if (debug_on < 2 && (null_fd = open ("/dev/null", O_WRONLY)) >= 0)
        {
          fflush (stderr);
          saved = dup (STDERR_FILENO);
          dup2 (null_fd, STDERR_FILENO);
          close (null_fd);
        }
      ok = vosk_model_load (m, wanted_threads (), cfg_bert, cache, &error);
      if (saved >= 0)
        {
          fflush (stderr);
          dup2 (saved, STDERR_FILENO);
          close (saved);
        }
      g_free (cache);
      if (ok)
        {
          /* the first run of the parallel executor is ~50 % slower (it
           * allocates every buffer); pay it here, not on the first message */
          gsize n;
          g_free (vosk_synth (m, g_str_equal (vosk_model_language (m), "ru")
                                 ? "Привет, как дела?" : "Hello, how are you?",
                              0, 1.0f, &n, NULL, NULL));
        }
      if (ok)
        DBG ("loaded %s (%d threads%s) in %.1f s", vosk_model_dir (m), wanted_threads (),
             cfg_bert ? ", BERT" : "", (g_get_monotonic_time () - t0) / 1e6);
      else
        {
          ERR ("cannot load %s: %s", vosk_model_dir (m), error->message);
          g_error_free (error);
        }

      g_mutex_lock (&load_lock);
      if (ok)
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
request_model_locked (VoskModel *m)
{
  wanted_model = m;
  if (m == loaded_model || loading)
    return;
  if (load_thread != NULL)
    g_thread_join (load_thread);
  loading = TRUE;
  load_thread = g_thread_new ("vosk-load", load_worker, NULL);
}



int
module_init (char **msg)
{
  load_voices ();
  if (voices->len == 0)
    {
      *msg = g_strdup_printf ("no Vosk TTS models in %s", cfg_models_dir);
      return -1;
    }
  module_audio_set_server ();
  *msg = g_strdup_printf ("Vosk TTS ready, %u voices", voices->len);
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
 * else the n-th voice of the voice type's gender of the language's first
 * model (else of any model) */
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
      VoskModel *first;

      for (guint i = 0; i < voices->len && lang != NULL; i++)
        {
          Voice *v = g_ptr_array_index (voices, i);
          if (strcmp (v->language, lang) == 0)
            g_ptr_array_add (cand, v);
        }
      if (cand->len == 0)
        for (guint i = 0; i < voices->len; i++)
          g_ptr_array_add (cand, g_ptr_array_index (voices, i));
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
wait_for_model (VoskModel *m)
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
  GError *error = NULL;
  /* rate -100..100 -> half .. double speed */
  float speed = (float) pow (2.0, CLAMP (set_rate, -100, 100) / 100.0);
  gint64 t0 = g_get_monotonic_time ();
  gsize n = 0;
  float *wav = vosk_synth (v->model, text, v->sid, speed, &n, NULL, &error);

  if (wav == NULL)
    {
      if (error != NULL)
        {
          ERR ("synthesis failed: %s: %s", error->message, text);
          g_error_free (error);
        }
      return;
    }
  DBG ("%.2f s of speech in %.2f s: %s", n / (double) vosk_model_sample_rate (v->model),
       (g_get_monotonic_time () - t0) / 1e6, text);
  if (!stop_requested)
    send_audio (wav, n, vosk_model_sample_rate (v->model));
  g_free (wav);
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
  gboolean busy;

  g_mutex_lock (&load_lock);
  wanted_model = loaded_model;   /* no further loads */
  busy = loading;
  g_mutex_unlock (&load_lock);
  if (busy)
    {
      /* a model takes up to ~15 s to load and ORT cannot be interrupted;
       * speechd waits for the reply to QUIT meanwhile (every SIGHUP closes
       * all modules), so answer now and leave the memory to the kernel */
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
    vosk_model_unload (loaded_model);
  loaded_model = NULL;
  return 0;
}



int
module_loop (void)
{
  if (cfg_preload)
    {
      /* the voice that a message without a voice would get */
      Voice *v;
      g_free (set_language);
      set_language = NULL;
      if (cfg_lang_voice != NULL && g_hash_table_size (cfg_lang_voice) > 0)
        {
          GHashTableIter it;
          gpointer value;
          g_hash_table_iter_init (&it, cfg_lang_voice);
          v = NULL;
          while (v == NULL && g_hash_table_iter_next (&it, NULL, &value))
            v = voice_by_name (value);
          if (v == NULL)
            v = pick_voice ();
        }
      else
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

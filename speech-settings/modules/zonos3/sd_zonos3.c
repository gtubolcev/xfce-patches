/*
 * sd_zonos3 - speech-dispatcher output module for ZONOS3-Mini
 *
 * The model is too big to load for every message, so the module runs
 * zonos3-server on a free localhost port the first time it has to speak and
 * stops it again after Zonos3IdleTimeout seconds without speech.  The server
 * outlives the module: speech-dispatcher restarts its modules on every reload,
 * and a new module adopts the running server (state file in
 * $XDG_RUNTIME_DIR) instead of loading the model again.  A small watcher
 * process started with the server stops it when no living module has owned it
 * for Zonos3IdleTimeout seconds (also when the module was killed).  The first piece of a
 * message is cut short (Zonos3FirstChunk) so the first sound comes sooner.
 * Zonos3Preload starts it with the module instead of with the first message.
 * With Zonos3Gpu the server runs on the GPU (Vulkan); if it can't start there,
 * the module starts it again on the CPU.  The text is
 * cut into sentences; each one is synthesized by POST /v2/generate and handed
 * to the speech-dispatcher server as soon as it is complete, so the next one
 * is synthesized while the previous one is playing.
 *
 * Voices are speaker embeddings, Zonos3VoicesDir/<lang>-<name>.embedding.npy,
 * offered as "zonos3-<lang>-<name>".  A "male" or "female" part of the name
 * gives the voice its gender (reported as the variant); the voice types
 * MALEn / FEMALEn pick the n-th voice of that gender of the language.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <speech-dispatcher/spd_module_main.h>

#define DBG(...) do { if (debug_on) { fprintf (stderr, "sd_zonos3: " __VA_ARGS__); fputc ('\n', stderr); } } while (0)
#define ERR(...) do { fprintf (stderr, "sd_zonos3: " __VA_ARGS__); fputc ('\n', stderr); } while (0)

/* configuration */
static gchar *cfg_server = NULL;
static gchar *cfg_model = NULL;
static gchar *cfg_voices_dir = NULL;
/* Zonos3Voice "<lang>" "<voice>": the voice of a language, before the voice type */
static GHashTable *cfg_lang_voice = NULL;
static int cfg_idle_timeout = 3600;
static double cfg_loudness = -20.0;
static unsigned cfg_seed = 99;
static int cfg_threads = 0;
static gboolean cfg_gpu = TRUE;
static gboolean cfg_preload = FALSE;
static int cfg_load_timeout = 120;
static int cfg_first_chunk = 40;
static int debug_on = 0;

typedef struct
{
  gchar *name;      /* zonos3-ru-1 */
  gchar *language;  /* ru */
  gchar *gender;    /* "male", "female" or NULL */
  gchar *path;
  gchar *b64;       /* the .npy file, base64; read when first used */
} Voice;

static GPtrArray *voices = NULL;
static SPDVoice **spd_voices = NULL;

/* message settings */
static gchar *set_voice_name = NULL;
static const gchar *set_gender = NULL;   /* from the voice type */
static int set_type_index = 0;           /* MALE2 -> 1 */
static gchar *set_language = NULL;
static int set_rate = 0;
static int set_volume = 0;

/* server */
static pid_t server_pid = 0;
static int server_port = 0;
static gboolean server_ready = FALSE;
static pid_t server_child = 0;   /* server_pid when we forked it */
static gboolean server_gpu = FALSE;  /* the server we started was asked to use the GPU */
static gboolean gpu_failed = FALSE;  /* it could not start there, use the CPU */
static pid_t watcher_pid = 0;        /* see server_watch */
static gboolean quit_requested = FALSE;
static time_t last_use = 0;

static volatile int stop_requested = 0;



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

  cfg_server = g_strdup ("/usr/bin/zonos3-server");
  cfg_model = g_strdup ("/usr/share/zonos3/gguf/zonos3-ptq.gguf");
  cfg_voices_dir = g_strdup ("/usr/share/zonos3/voices");

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

      if (g_ascii_strcasecmp (line, "Zonos3Server") == 0)
        g_free (cfg_server), cfg_server = g_strdup (value);
      else if (g_ascii_strcasecmp (line, "Zonos3Model") == 0)
        g_free (cfg_model), cfg_model = g_strdup (value);
      else if (g_ascii_strcasecmp (line, "Zonos3VoicesDir") == 0)
        g_free (cfg_voices_dir), cfg_voices_dir = g_strdup (value);
      else if (g_ascii_strcasecmp (line, "Zonos3IdleTimeout") == 0)
        cfg_idle_timeout = atoi (value);
      else if (g_ascii_strcasecmp (line, "Zonos3Loudness") == 0)
        cfg_loudness = g_ascii_strtod (value, NULL);
      else if (g_ascii_strcasecmp (line, "Zonos3Seed") == 0)
        cfg_seed = (unsigned) strtoul (value, NULL, 10);
      else if (g_ascii_strcasecmp (line, "Zonos3Threads") == 0)
        cfg_threads = atoi (value);
      else if (g_ascii_strcasecmp (line, "Zonos3LoadTimeout") == 0)
        cfg_load_timeout = atoi (value);
      else if (g_ascii_strcasecmp (line, "Zonos3Preload") == 0)
        cfg_preload = atoi (value) != 0;
      else if (g_ascii_strcasecmp (line, "Zonos3Gpu") == 0)
        cfg_gpu = atoi (value) != 0;
      else if (g_ascii_strcasecmp (line, "Zonos3FirstChunk") == 0)
        cfg_first_chunk = atoi (value);
      else if (g_ascii_strcasecmp (line, "Zonos3Voice") == 0)
        {
          /* value was unquoted as one string: ru" "zonos3-ru-male-1 */
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



static gint
voice_compare (gconstpointer a, gconstpointer b)
{
  const Voice *va = *(Voice * const *) a, *vb = *(Voice * const *) b;
  return g_utf8_collate (va->name, vb->name);
}



static void
load_voices (void)
{
  GDir *dir;
  const gchar *file;

  voices = g_ptr_array_new ();
  dir = g_dir_open (cfg_voices_dir, 0, NULL);
  if (dir == NULL)
    return;
  while ((file = g_dir_read_name (dir)) != NULL)
    {
      Voice *v;
      gchar *stem;
      const gchar *dash;

      if (!g_str_has_suffix (file, ".embedding.npy"))
        continue;
      stem = g_strndup (file, strlen (file) - strlen (".embedding.npy"));
      dash = strchr (stem, '-');
      v = g_new0 (Voice, 1);
      v->name = g_strconcat ("zonos3-", stem, NULL);
      v->language = dash != NULL ? g_ascii_strdown (stem, dash - stem) : g_strdup ("en");
      v->path = g_build_filename (cfg_voices_dir, file, NULL);
      {
        gchar **parts = g_strsplit (stem, "-", -1);
        for (guint p = 1; parts[p] != NULL && v->gender == NULL; p++)
          if (g_ascii_strcasecmp (parts[p], "female") == 0)
            v->gender = g_strdup ("female");
          else if (g_ascii_strcasecmp (parts[p], "male") == 0)
            v->gender = g_strdup ("male");
        g_strfreev (parts);
      }
      g_ptr_array_add (voices, v);
      g_free (stem);
    }
  g_dir_close (dir);
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



int
module_init (char **msg)
{
  load_voices ();
  if (voices->len == 0)
    {
      *msg = strdup ("no voices (*.embedding.npy) in Zonos3VoicesDir");
      return -1;
    }
  if (!g_file_test (cfg_server, G_FILE_TEST_IS_EXECUTABLE)
      || !g_file_test (cfg_model, G_FILE_TEST_EXISTS))
    {
      *msg = strdup ("Zonos3Server or Zonos3Model is missing");
      return -1;
    }
  module_audio_set_server ();
  *msg = strdup (cfg_preload ? "ZONOS3 ready (the server is loading)"
                             : "ZONOS3 ready (the server starts on the first message)");
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



/* ------------------------------------------------------------------ server */

static int
wanted_threads (void)
{
  /* more than 4 decoder threads made it slower on a 4-core/8-thread laptop */
  return cfg_threads > 0 ? cfg_threads : MIN ((int) g_get_num_processors (), 4);
}



static gchar *
state_path (void)
{
  return g_build_filename (g_get_user_runtime_dir (), "sd_zonos3.state", NULL);
}



/* pid port threads gpu owner released, then the model path on the second line;
 * gpu = Zonos3Gpu of the module that started it (also when it fell back to the CPU),
 * owner 0 = no module uses the server, released = when the last one let go. */
static void
state_write (long owner, long released)
{
  gchar *path = state_path ();
  gchar *text = g_strdup_printf ("%d %d %d %d %ld %ld\n%s\n", server_pid, server_port,
                                 wanted_threads (), cfg_gpu, owner, released, cfg_model);

  if (!g_file_set_contents_full (path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL))
    ERR ("can't write %s", path);
  g_free (text);
  g_free (path);
}



typedef struct
{
  pid_t pid;
  int port, threads, gpu;
  long owner, released;
  gchar model[1024];
} State;

static gboolean
state_read (State *st)
{
  gchar *path = state_path ();
  gchar *text = NULL;
  gboolean ok = FALSE;

  if (g_file_get_contents (path, &text, NULL, NULL))
    {
      int pid;
      ok = sscanf (text, "%d %d %d %d %ld %ld\n%1023[^\n]", &pid, &st->port, &st->threads,
                   &st->gpu, &st->owner, &st->released, st->model) == 7;
      st->pid = pid;
    }
  g_free (text);
  g_free (path);
  return ok;
}



static void
state_remove (void)
{
  gchar *path = state_path ();
  g_unlink (path);
  g_free (path);
}



/* The server may be our child or one adopted from an earlier module. */
static gboolean
pid_is_server (pid_t pid)
{
  gchar *path = g_strdup_printf ("/proc/%d/comm", (int) pid);
  gchar *comm = NULL;
  gboolean ok;

  if (pid > 0 && pid == server_child)
    {
      /* our child: right after fork its comm is still ours */
      g_free (path);
      if (waitpid (pid, NULL, WNOHANG) == 0)
        return TRUE;
      server_child = 0;
      return FALSE;
    }
  ok = pid > 0 && kill (pid, 0) == 0
       && g_file_get_contents (path, &comm, NULL, NULL)
       && g_str_has_prefix (comm, "zonos3-server");
  g_free (comm);
  g_free (path);
  return ok;
}



static void
kill_server (pid_t pid)
{
  kill (pid, SIGTERM);
  for (int i = 0; i < 50 && pid_is_server (pid); i++)
    g_usleep (100000);
  if (pid_is_server (pid))
    kill (pid, SIGKILL);
  waitpid (pid, NULL, WNOHANG);
}



static void
server_stop (void)
{
  if (server_pid <= 0)
    return;
  DBG ("stopping zonos3-server (pid %d)", server_pid);
  kill_server (server_pid);
  server_pid = 0;
  server_ready = FALSE;
  state_remove ();
}



static gboolean
server_alive (void)
{
  if (server_pid <= 0)
    return FALSE;
  if (!pid_is_server (server_pid))
    {
      ERR ("zonos3-server exited");
      server_pid = 0;
      server_ready = FALSE;
      state_remove ();
      return FALSE;
    }
  return TRUE;
}



/* Takes over a server left behind by an earlier instance of the module. */
static gboolean
server_adopt (void)
{
  State st;

  if (!state_read (&st))
    return FALSE;
  if (!pid_is_server (st.pid))
    {
      state_remove ();
      return FALSE;
    }
  if (st.threads != wanted_threads () || st.gpu != cfg_gpu || strcmp (st.model, cfg_model) != 0)
    {
      DBG ("zonos3-server %d runs with other settings, stopping it", st.pid);
      kill_server (st.pid);
      state_remove ();
      return FALSE;
    }
  server_pid = st.pid;
  server_port = st.port;
  server_ready = FALSE;   /* it may still be loading */
  server_gpu = FALSE;     /* don't know; no CPU retry if it fails while loading */
  state_write (getpid (), 0);
  DBG ("adopted zonos3-server pid %d port %d", server_pid, server_port);
  return TRUE;
}



/* Is the module that owns the server still running? */
static gboolean
owner_alive (long owner)
{
  gchar *path = g_strdup_printf ("/proc/%ld/comm", owner);
  gchar *comm = NULL;
  gboolean ok = owner > 0 && g_file_get_contents (path, &comm, NULL, NULL)
                && g_str_has_prefix (comm, "sd_zonos3");

  g_free (comm);
  g_free (path);
  return ok;
}



/* A watcher process lives as long as the server and stops it when no module
 * has owned it for Zonos3IdleTimeout seconds: after the last module released
 * it, or after the owner died without doing so (killed by speech-dispatcher). */
static void
server_watch (void)
{
  pid_t pid = server_pid;
  time_t dead_since = 0;

  watcher_pid = fork ();
  if (watcher_pid != 0)
    {
      if (watcher_pid < 0)
        watcher_pid = 0;
      return;
    }

  /* no ties to speech-dispatcher; the server is not our child */
  server_child = 0;
  setsid ();
  {
    int null = open ("/dev/null", O_RDWR);
    dup2 (null, STDIN_FILENO);
    dup2 (null, STDOUT_FILENO);
    dup2 (null, STDERR_FILENO);
    closefrom (3);
  }
  for (;;)
    {
      State st;
      time_t since;

      sleep (5);
      if (!state_read (&st) || st.pid != pid)
        _exit (0);   /* stopped by a module */
      if (!pid_is_server (pid))
        {
          state_remove ();
          _exit (0);
        }
      if (st.owner != 0 && owner_alive (st.owner))
        {
          dead_since = 0;
          continue;
        }
      if (st.owner == 0)
        since = st.released, dead_since = 0;
      else
        since = dead_since != 0 ? dead_since : (dead_since = time (NULL));
      if (time (NULL) - since >= MAX (cfg_idle_timeout, 0))
        {
          kill_server (pid);
          state_remove ();
          _exit (0);
        }
    }
}



/* Called when the module goes away: the server stays for the next module,
 * the watcher stops it if none comes within the idle timeout. */
static void
server_release (void)
{
  if (!server_alive ())
    return;
  if (cfg_idle_timeout <= 0)
    server_stop ();
  else
    state_write (0, (long) time (NULL));
}



static int
free_port (void)
{
  struct sockaddr_in addr = { 0 };
  socklen_t len = sizeof (addr);
  int fd, port = -1;

  fd = socket (AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  if (bind (fd, (struct sockaddr *) &addr, sizeof (addr)) == 0
      && getsockname (fd, (struct sockaddr *) &addr, &len) == 0)
    port = ntohs (addr.sin_port);
  close (fd);
  return port;
}



static int
server_connect (void)
{
  struct sockaddr_in addr = { 0 };
  int fd;

  fd = socket (AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  addr.sin_family = AF_INET;
  addr.sin_port = htons (server_port);
  addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  if (connect (fd, (struct sockaddr *) &addr, sizeof (addr)) != 0)
    {
      close (fd);
      return -1;
    }
  return fd;
}



/* Lets the speech-dispatcher server interrupt us (STOP) while we wait. */
static void
process_input (void)
{
  if (module_process (STDIN_FILENO, 0) == 0)
    {
      /* QUIT while we wait: finish the message, module_loop then quits */
      quit_requested = TRUE;
      stop_requested = 1;
    }
}



static gboolean
server_health (void)
{
  static const char req[] = "GET /v2/health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  char buf[64] = "";
  int fd = server_connect ();
  gboolean ok = FALSE;

  if (fd < 0)
    return FALSE;
  if (write (fd, req, sizeof (req) - 1) == (ssize_t) sizeof (req) - 1
      && read (fd, buf, sizeof (buf) - 1) > 12)
    ok = strncmp (buf + 9, "200", 3) == 0;
  close (fd);
  return ok;
}



static gboolean
server_spawn (void)
{
  gchar threads[16];

  server_port = free_port ();
  if (server_port <= 0)
    {
      ERR ("no free port");
      return FALSE;
    }
  g_snprintf (threads, sizeof (threads), "%d", wanted_threads ());
  server_gpu = cfg_gpu && !gpu_failed;

  server_pid = fork ();
  if (server_pid < 0)
    {
      server_pid = 0;
      return FALSE;
    }
  if (server_pid == 0)
    {
      gchar port[16];
      int out;

      /* its own session, so it outlives the module (see server_release) */
      setsid ();
      g_snprintf (port, sizeof (port), "%d", server_port);
      setenv ("ZAC_THREADS", threads, 0);
      out = open ("/dev/null", O_RDWR);
      dup2 (out, STDIN_FILENO);
      if (debug_on)
        {
          gchar *log = g_build_filename (g_get_user_runtime_dir (), "zonos3-server.log", NULL);
          out = open (log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        }
      dup2 (out, STDOUT_FILENO);
      dup2 (out, STDERR_FILENO);
      closefrom (3);
      execl (cfg_server, cfg_server, cfg_model, server_gpu ? "--gpu" : "--cpu",
             "--host", "127.0.0.1", "--port", port,
             "--batch", "1", "--tts-default-voices-dir", "", (char *) NULL);
      _exit (127);
    }

  server_child = server_pid;
  server_ready = FALSE;
  state_write (getpid (), 0);
  server_watch ();
  DBG ("started zonos3-server pid %d port %d on the %s", server_pid, server_port,
       server_gpu ? "GPU" : "CPU");
  return TRUE;
}



static gboolean
server_spawn_cpu (void)
{
  ERR ("zonos3-server failed on the GPU, starting it on the CPU");
  gpu_failed = TRUE;
  return server_spawn ();
}



static gboolean
server_start (void)
{
  time_t start;

  if (!server_alive () && !server_adopt () && !server_spawn ())
    return FALSE;
  if (server_ready)
    return TRUE;

  start = time (NULL);
  while (time (NULL) - start < cfg_load_timeout)
    {
      if (!server_alive ())
        {
          /* e.g. no Vulkan device or not enough video memory */
          if (!server_gpu || !server_spawn_cpu ())
            return FALSE;
          start = time (NULL);
          continue;
        }
      if (server_health ())
        {
          DBG ("zonos3-server ready after %ld s", (long) (time (NULL) - start));
          server_ready = TRUE;
          return TRUE;
        }
      process_input ();
      if (stop_requested)
        return FALSE;   /* keep it loading for the next message */
      g_usleep (200000);
    }
  ERR ("zonos3-server did not start in %d s", cfg_load_timeout);
  server_stop ();
  return FALSE;
}



/* ------------------------------------------------------------------- text */

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
          /* <break .../> and closing block tags separate words */
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
  return g_strstrip (g_string_free (out, FALSE)) ;
}



/* Cuts the text after . ! ? … ; and line breaks; long pieces also after commas.
 * The first piece is cut at a comma already after cfg_first_chunk characters:
 * nothing plays before it is synthesized. */
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



static gchar *
json_escape (const gchar *s)
{
  GString *out = g_string_new (NULL);

  for (; *s != '\0'; s++)
    {
      guchar c = (guchar) *s;
      if (c == '"' || c == '\\')
        g_string_append_printf (out, "\\%c", c);
      else if (c < 0x20)
        g_string_append_printf (out, "\\u%04x", c);
      else
        g_string_append_c (out, c);
    }
  return g_string_free (out, FALSE);
}



static Voice *
pick_voice (void)
{
  gchar *lang = NULL;
  Voice *found = NULL;

  if (set_voice_name != NULL)
    for (guint i = 0; i < voices->len && found == NULL; i++)
      {
        Voice *v = g_ptr_array_index (voices, i);
        if (g_ascii_strcasecmp (v->name, set_voice_name) == 0)
          found = v;
      }

  /* the voice chosen for the language in the configuration */
  if (found == NULL && cfg_lang_voice != NULL && set_language != NULL)
    {
      gchar *l = g_ascii_strdown (set_language, strcspn (set_language, "-_"));
      const gchar *name = g_hash_table_lookup (cfg_lang_voice, l);

      for (guint i = 0; i < voices->len && found == NULL && name != NULL; i++)
        {
          Voice *v = g_ptr_array_index (voices, i);
          if (g_ascii_strcasecmp (v->name, name) == 0)
            found = v;
        }
      g_free (l);
    }

  /* the voices of the language, else of English, else all of them; among
   * them the n-th one of the gender of the voice type, else the n-th one */
  if (found == NULL)
    {
      const gchar *langs[] = { NULL, "en", NULL };
      GPtrArray *cand = g_ptr_array_new ();
      GPtrArray *same = g_ptr_array_new ();

      if (set_language != NULL)
        lang = g_ascii_strdown (set_language, strcspn (set_language, "-_"));
      langs[0] = lang;
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
      for (guint i = 0; i < cand->len && set_gender != NULL; i++)
        {
          Voice *v = g_ptr_array_index (cand, i);
          if (g_strcmp0 (v->gender, set_gender) == 0)
            g_ptr_array_add (same, v);
        }
      if (same->len > 0)
        found = g_ptr_array_index (same, set_type_index % same->len);
      else if (cand->len > 0)
        found = g_ptr_array_index (cand, set_type_index % cand->len);
      g_ptr_array_free (same, TRUE);
      g_ptr_array_free (cand, TRUE);
    }
  g_free (lang);
  if (found == NULL)
    found = g_ptr_array_index (voices, 0);

  if (found->b64 == NULL)
    {
      gchar *bytes;
      gsize len;
      if (g_file_get_contents (found->path, &bytes, &len, NULL))
        {
          found->b64 = g_base64_encode ((const guchar *) bytes, len);
          g_free (bytes);
        }
    }
  return found;
}



/* ------------------------------------------------------------------- HTTP */

static gboolean
write_all (int fd, const char *buf, size_t len)
{
  while (len > 0)
    {
      ssize_t n = write (fd, buf, len);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return FALSE;
      buf += n;
      len -= n;
    }
  return TRUE;
}



typedef struct
{
  int fd;
  char buf[65536];
  size_t start, end;
} Reader;



/* Reads more data; while waiting lets STOP in. 1 = data, 0 = EOF, -1 = error or stop. */
static int
reader_fill (Reader *r)
{
  if (r->start > 0)
    {
      memmove (r->buf, r->buf + r->start, r->end - r->start);
      r->end -= r->start;
      r->start = 0;
    }
  if (r->end == sizeof (r->buf))
    return -1;
  for (;;)
    {
      struct pollfd fds[2] = { { r->fd, POLLIN, 0 }, { STDIN_FILENO, POLLIN, 0 } };
      ssize_t n;

      if (poll (fds, 2, 200) < 0 && errno != EINTR)
        return -1;
      if (fds[1].revents & POLLIN)
        process_input ();
      if (stop_requested)
        return -1;
      if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR)))
        continue;
      n = read (r->fd, r->buf + r->end, sizeof (r->buf) - r->end);
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0)
        return -1;
      if (n == 0)
        return 0;
      r->end += n;
      return 1;
    }
}



/* One line (without CRLF) into line, at most size - 1 bytes */
static gboolean
reader_line (Reader *r, char *line, size_t size)
{
  for (;;)
    {
      char *nl = memchr (r->buf + r->start, '\n', r->end - r->start);
      if (nl != NULL)
        {
          size_t len = nl - (r->buf + r->start);
          size_t copy = MIN (len, size - 1);
          memcpy (line, r->buf + r->start, copy);
          line[copy] = '\0';
          if (copy > 0 && line[copy - 1] == '\r')
            line[copy - 1] = '\0';
          r->start += len + 1;
          return TRUE;
        }
      if (reader_fill (r) <= 0)
        return FALSE;
    }
}



static gboolean
reader_bytes (Reader *r, GByteArray *out, size_t len)
{
  while (len > 0)
    {
      size_t have = r->end - r->start;
      if (have == 0)
        {
          if (reader_fill (r) <= 0)
            return FALSE;
          continue;
        }
      have = MIN (have, len);
      if (out != NULL)
        g_byte_array_append (out, (const guint8 *) r->buf + r->start, have);
      r->start += have;
      len -= have;
    }
  return TRUE;
}



/* Synthesizes one sentence into float samples. */
static gboolean
generate (const gchar *text, Voice *voice, GByteArray *pcm, int *sample_rate)
{
  gchar *esc, *body, *req, *conditions;
  Reader *r;
  char line[1024];
  gboolean ok = FALSE, chunked = FALSE;
  long content_length = -1;
  int status = 0;

  esc = json_escape (text);
  g_ascii_formatd (line, sizeof (line), "%.1f", cfg_loudness);   /* no decimal comma */
  /* speaking_rate is in UTF-8 bytes per second (Cyrillic takes 2 bytes a letter), so the
   * target is about 14 characters a second times 2^(rate/100); the model follows it only
   * roughly (Russian: ~0.85x .. 1.2x) */
  if (set_rate != 0)
    {
      gchar rate[G_ASCII_DTOSTR_BUF_SIZE];
      double per_char = (double) strlen (text) / MAX (1, g_utf8_strlen (text, -1));
      g_ascii_formatd (rate, sizeof (rate), "%.2f", 14.0 * per_char * pow (2.0, set_rate / 100.0));
      conditions = g_strdup_printf ("\"condition_values\": {\"loudness\": %s, \"speaking_rate\": %s}",
                                    line, rate);
    }
  else
    conditions = g_strdup_printf ("\"condition_values\": {\"loudness\": %s}", line);
  body = g_strdup_printf ("{\"text\": \"%s\", \"language\": \"en_us\", \"text_normalization\": false, "
                          "\"stream\": true, \"seed\": %u, \"speaker_mode\": \"embedding\", "
                          "\"speaker_embedding_base64\": \"%s\", %s}",
                          esc, cfg_seed, voice->b64, conditions);
  req = g_strdup_printf ("POST /v2/generate HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"
                         "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n", strlen (body));
  g_free (esc);
  g_free (conditions);

  r = g_new0 (Reader, 1);
  r->fd = server_connect ();
  if (r->fd < 0)
    goto out;
  if (!write_all (r->fd, req, strlen (req)) || !write_all (r->fd, body, strlen (body)))
    goto out;

  if (!reader_line (r, line, sizeof (line)) || sscanf (line, "HTTP/%*s %d", &status) != 1)
    goto out;
  *sample_rate = 44100;
  while (reader_line (r, line, sizeof (line)) && *line != '\0')
    {
      if (g_ascii_strncasecmp (line, "X-Audio-Sample-Rate:", 20) == 0)
        *sample_rate = atoi (line + 20);
      else if (g_ascii_strncasecmp (line, "Content-Length:", 15) == 0)
        content_length = atol (line + 15);
      else if (g_ascii_strncasecmp (line, "Transfer-Encoding:", 18) == 0 && strstr (line, "chunked"))
        chunked = TRUE;
    }

  if (status != 200)
    {
      GByteArray *err = g_byte_array_new ();
      if (content_length > 0 && content_length < 65536)
        reader_bytes (r, err, content_length);
      g_byte_array_append (err, (const guint8 *) "", 1);
      ERR ("zonos3-server: HTTP %d %s", status, (const char *) err->data);
      g_byte_array_free (err, TRUE);
      goto out;
    }

  if (chunked)
    {
      for (;;)
        {
          long size;
          if (!reader_line (r, line, sizeof (line)))
            goto out;
          size = strtol (line, NULL, 16);
          if (size == 0)
            break;
          if (!reader_bytes (r, pcm, size) || !reader_line (r, line, sizeof (line)))
            goto out;
        }
    }
  else if (content_length >= 0)
    {
      if (!reader_bytes (r, pcm, content_length))
        goto out;
    }
  else
    while (reader_fill (r) > 0)
      ;
  ok = TRUE;

out:
  if (r->fd >= 0)
    close (r->fd);
  g_free (r);
  g_free (req);
  g_free (body);
  return ok && !stop_requested;
}



static void
send_audio (GByteArray *pcm, int sample_rate)
{
  size_t n = pcm->len / sizeof (float);
  const float *f = (const float *) pcm->data;
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
      /* nothing to say (e.g. only spaces): finish the message at once,
       * an error would leave clients waiting for its end */
      g_free (text);
      module_speak_ok ();
      module_report_event_begin ();
      module_report_event_end ();
      return;
    }
  module_speak_ok ();
  module_report_event_begin ();
  last_use = time (NULL);

  voice = pick_voice ();
  sentences = split_sentences (text);
  DBG ("speaking %u sentence(s) with %s: %s", sentences->len, voice->name, text);

  if (voice->b64 != NULL && server_start ())
    for (guint i = 0; i < sentences->len && !stop_requested; i++)
      {
        GByteArray *pcm = g_byte_array_new ();
        int rate = 44100;
        if (generate (g_ptr_array_index (sentences, i), voice, pcm, &rate))
          send_audio (pcm, rate);
        g_byte_array_free (pcm, TRUE);
      }

  last_use = time (NULL);
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
  server_release ();
  return 0;
}



/* Main loop: the protocol, plus stopping an idle server. */
int
module_loop (void)
{
  if (cfg_preload)
    {
      /* start loading now, server_start waits for it with the first message */
      last_use = time (NULL);
      if (!server_alive () && !server_adopt ())
        server_spawn ();
    }
  for (;;)
    {
      struct pollfd fd = { STDIN_FILENO, POLLIN, 0 };
      int ret = poll (&fd, 1, 5000);

      if (ret < 0 && errno != EINTR)
        return -1;
      if (ret > 0)
        {
          if ((fd.revents & POLLIN) && module_process (STDIN_FILENO, 0) == 0)
            quit_requested = TRUE;
          if (quit_requested)
            {
              server_release ();
              return 0;
            }
          if (fd.revents & (POLLHUP | POLLERR))
            {
              server_release ();
              return -1;
            }
        }
      if (watcher_pid > 0 && waitpid (watcher_pid, NULL, WNOHANG) == watcher_pid)
        watcher_pid = 0;
      if (server_pid > 0 && cfg_idle_timeout > 0 && time (NULL) - last_use > cfg_idle_timeout)
        server_stop ();
      else
        server_alive ();
    }
}

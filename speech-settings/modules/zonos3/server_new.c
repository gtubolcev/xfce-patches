static int
wanted_threads (void)
{
  return cfg_threads > 0 ? cfg_threads : (int) g_get_num_processors ();
}



static gchar *
state_path (void)
{
  return g_build_filename (g_get_user_runtime_dir (), "sd_zonos3.state", NULL);
}



/* pid port threads owner released, then the model path on the second line;
 * owner 0 = no module uses the server, released = when the last one let go. */
static void
state_write (long owner, long released)
{
  gchar *path = state_path ();
  gchar *text = g_strdup_printf ("%d %d %d %ld %ld\n%s\n", server_pid, server_port,
                                 wanted_threads (), owner, released, cfg_model);

  if (!g_file_set_contents_full (path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL))
    ERR ("can't write %s", path);
  g_free (text);
  g_free (path);
}



typedef struct
{
  pid_t pid;
  int port, threads;
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
      ok = sscanf (text, "%d %d %d %ld %ld\n%1023[^\n]", &pid, &st->port, &st->threads,
                   &st->owner, &st->released, st->model) == 6;
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

  waitpid (pid, NULL, WNOHANG);   /* reap it if it is our child and has exited */
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
  if (st.threads != wanted_threads () || strcmp (st.model, cfg_model) != 0)
    {
      DBG ("zonos3-server %d runs with other settings, stopping it", st.pid);
      kill_server (st.pid);
      state_remove ();
      return FALSE;
    }
  server_pid = st.pid;
  server_port = st.port;
  server_ready = FALSE;   /* it may still be loading */
  state_write (getpid (), 0);
  DBG ("adopted zonos3-server pid %d port %d", server_pid, server_port);
  return TRUE;
}



/* Called when the module goes away: the server stays for the next module,
 * a reaper stops it if none comes within the idle timeout. */
static void
server_release (void)
{
  long released = (long) time (NULL);
  pid_t reaper;

  if (!server_alive ())
    return;
  if (cfg_idle_timeout <= 0)
    {
      server_stop ();
      return;
    }
  state_write (0, released);
  reaper = fork ();
  if (reaper != 0)
    return;

  /* the reaper: no ties to speech-dispatcher */
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

      sleep (5);
      if (!state_read (&st) || st.pid != server_pid || st.owner != 0 || st.released != released)
        _exit (0);   /* adopted (or gone) */
      if (!pid_is_server (st.pid))
        {
          state_remove ();
          _exit (0);
        }
      if (time (NULL) - released > cfg_idle_timeout)
        {
          kill_server (st.pid);
          state_remove ();
          _exit (0);
        }
    }
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
  module_process (STDIN_FILENO, 0);
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
      execl (cfg_server, cfg_server, cfg_model, "--host", "127.0.0.1", "--port", port,
             "--batch", "1", "--tts-default-voices-dir", "", (char *) NULL);
      _exit (127);
    }

  server_ready = FALSE;
  state_write (getpid (), 0);
  DBG ("started zonos3-server pid %d port %d", server_pid, server_port);
  return TRUE;
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
        return FALSE;
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

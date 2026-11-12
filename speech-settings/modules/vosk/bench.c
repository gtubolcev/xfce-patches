/* bench MODEL_DIR TEXTS_FILE REPS THREADS [--no-bert]: load time, per-text
 * median synth time, peak memory; same output as bench.py */
#include "vosk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

static double
now (void)
{
  struct timespec t;
  clock_gettime (CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

static int
cmp_d (const void *a, const void *b)
{
  double x = *(const double *) a, y = *(const double *) b;
  return x < y ? -1 : x > y;
}

int
main (int argc, char **argv)
{
  GError *error = NULL;
  VoskModel *m;
  gchar *data, **lines;
  int reps, threads;
  double t0, t_load, total_audio = 0, total_synth = 0;
  struct rusage ru;

  if (argc < 5)
    return 2;
  reps = atoi (argv[3]);
  threads = atoi (argv[4]);
  t0 = now ();
  if ((m = vosk_model_open (argv[1], &error)) == NULL
      || !vosk_model_load (m, threads, argc < 6, "cache", &error))
    {
      fprintf (stderr, "%s\n", error->message);
      return 1;
    }
  t_load = now () - t0;
  getrusage (RUSAGE_SELF, &ru);
  printf ("load %.2f s, rss after load %ld MB\n", t_load, ru.ru_maxrss / 1024);
  g_file_get_contents (argv[2], &data, NULL, NULL);
  lines = g_strsplit (data, "\n", -1);
  for (int i = 0; lines[i]; i++)
    {
      double *t = g_new (double, reps), first = 0;
      gsize n = 0;
      if (!*lines[i])
        continue;
      for (int r = 0; r < reps; r++)
        {
          double a = now ();
          g_free (vosk_synth (m, lines[i], 0, 1.0f, &n, NULL, NULL));
          t[r] = now () - a;
          if (r == 0)
            first = t[r];
        }
      qsort (t, reps, sizeof *t, cmp_d);
      printf ("text %d: audio %.2f s, first %.2f s, median %.2f s, RTF %.3f\n",
              i, n / 22050.0, first, t[reps / 2], t[reps / 2] / (n / 22050.0));
      total_audio += n / 22050.0;
      total_synth += t[reps / 2];
      g_free (t);
    }
  getrusage (RUSAGE_SELF, &ru);
  printf ("total RTF %.3f, peak rss %ld MB\n", total_synth / total_audio, ru.ru_maxrss / 1024);
  return 0;
}

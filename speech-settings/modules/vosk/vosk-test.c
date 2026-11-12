/* vosk-test MODEL_DIR SID OUT.wav TEXT [--no-bert]: prints the trace, writes a WAV */
#include "vosk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void
put16 (FILE *f, guint16 v) { fputc (v & 0xff, f); fputc (v >> 8, f); }
static void
put32 (FILE *f, guint32 v) { put16 (f, v & 0xffff); put16 (f, v >> 16); }

int
main (int argc, char **argv)
{
  GError *error = NULL;
  VoskModel *m;
  GString *trace = g_string_new (NULL);
  float *wav;
  gsize n;
  FILE *f;
  struct timespec t0, t1, t2;
  gboolean bert = argc < 6 || strcmp (argv[5], "--no-bert") != 0;

  if (argc == 4 && strcmp (argv[1], "--sort-dictionary") == 0)
    {
      if (vosk_make_sorted_dictionary (argv[2], argv[3], &error))
        return 0;
      fprintf (stderr, "%s\n", error->message);
      return 1;
    }
  if (argc < 5)
    {
      fprintf (stderr, "usage: vosk-test MODEL_DIR SID OUT.wav TEXT [--no-bert]\n");
      return 2;
    }
  if ((m = vosk_model_open (argv[1], &error)) == NULL)
    goto fail;
  clock_gettime (CLOCK_MONOTONIC, &t0);
  if (!vosk_model_load (m, 4, bert, "cache", &error))
    goto fail;
  clock_gettime (CLOCK_MONOTONIC, &t1);
  fprintf (stdout, "load cpu %.2f s\n", (double) clock () / CLOCKS_PER_SEC);
  for (int r = 1; getenv ("VOSK_REPS") && r < atoi (getenv ("VOSK_REPS")); r++) { struct timespec a, b; gsize k; clock_gettime (CLOCK_MONOTONIC, &a); g_free (vosk_synth (m, argv[4], atoi (argv[2]), 1.0f, &k, NULL, NULL)); clock_gettime (CLOCK_MONOTONIC, &b); printf ("rep %d: %.2f s\n", r, (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9); }
  wav = vosk_synth (m, argv[4], atoi (argv[2]), 1.0f, &n, trace, &error);
  clock_gettime (CLOCK_MONOTONIC, &t2);
  if (wav == NULL && error != NULL)
    goto fail;
  fputs (trace->str, stdout);
  printf ("load %.2f s, synth %.2f s, audio %.2f s\n",
          (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9,
          (t2.tv_sec - t1.tv_sec) + (t2.tv_nsec - t1.tv_nsec) / 1e9,
          (double) n / vosk_model_sample_rate (m));
  f = fopen (argv[3], "wb");
  fwrite ("RIFF", 1, 4, f); put32 (f, 36 + n * 2); fwrite ("WAVEfmt ", 1, 8, f);
  put32 (f, 16); put16 (f, 1); put16 (f, 1); put32 (f, vosk_model_sample_rate (m));
  put32 (f, vosk_model_sample_rate (m) * 2); put16 (f, 2); put16 (f, 16);
  fwrite ("data", 1, 4, f); put32 (f, n * 2);
  for (gsize i = 0; i < n; i++)
    {
      float v = CLAMP (wav[i], -1.0f, 1.0f) * 32767.0f;
      put16 (f, (guint16) (gint16) v);
    }
  fclose (f);
  g_free (wav);
  vosk_model_free (m);
  return 0;
fail:
  fprintf (stderr, "error: %s\n", error->message);
  return 1;
}

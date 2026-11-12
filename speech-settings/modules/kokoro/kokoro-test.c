/*
 * kokoro-test - try the Russian Kokoro
 *
 *   kokoro-test ipa   <espeak-path> <dict|-> < marked-or-plain lines
 *   kokoro-test mark  <espeak-path> <dict>   < lines
 *   kokoro-test raw   <espeak-path>          < acute-marked lines (IPA, no dictionary)
 *   kokoro-test synth <espeak-path> <dict> <onnx> <config> <voice.bin> <out-prefix> [speed]
 *       < lines: writes <out-prefix>NN.wav per line
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include "kokoro.h"
#include "ru-g2p.h"

#include <stdio.h>
#include <string.h>

static void
put_le (FILE *f, guint32 v, int n)
{
  for (int i = 0; i < n; i++)
    fputc ((v >> (8 * i)) & 0xff, f);
}

static gboolean
write_wav (const gchar *path, const gfloat *s, gsize n)
{
  FILE *f = fopen (path, "wb");
  if (f == NULL)
    return FALSE;
  fwrite ("RIFF", 1, 4, f); put_le (f, 36 + n * 2, 4);
  fwrite ("WAVEfmt ", 1, 8, f); put_le (f, 16, 4); put_le (f, 1, 2); put_le (f, 1, 2);
  put_le (f, KOKORO_SAMPLE_RATE, 4); put_le (f, KOKORO_SAMPLE_RATE * 2, 4);
  put_le (f, 2, 2); put_le (f, 16, 2);
  fwrite ("data", 1, 4, f); put_le (f, n * 2, 4);
  for (gsize i = 0; i < n; i++)
    {
      gfloat v = CLAMP (s[i], -1.0f, 1.0f);
      put_le (f, (guint16) (gint16) (v * 32767.0f), 2);
    }
  return fclose (f) == 0;
}

int
main (int argc, char **argv)
{
  GError *error = NULL;
  RuG2p *g;
  KokoroModel *m = NULL;
  gfloat *voice = NULL;
  char line[8192];
  int n = 0;
  const gchar *mode = argc > 1 ? argv[1] : "";
  gboolean synth = strcmp (mode, "synth") == 0;

  if (argc < 3 || (synth && argc < 8))
    {
      fprintf (stderr, "usage: see kokoro-test.c\n");
      return 2;
    }
  g = ru_g2p_new (argc > 3 && strcmp (argv[3], "-") != 0 ? argv[3] : NULL, argv[2], &error);
  if (g == NULL)
    goto err;
  if (synth)
    {
      if ((m = kokoro_model_new (argv[4], argv[5], 4, &error)) == NULL
          || (voice = kokoro_voice_load (argv[6], &error)) == NULL)
        goto err;
    }
  while (fgets (line, sizeof line, stdin) != NULL)
    {
      gchar *r;
      line[strcspn (line, "\n")] = '\0';
      if (strcmp (mode, "mark") == 0)
        r = ru_g2p_accentuate (g, line);
      else if (strcmp (mode, "raw") == 0)
        r = ru_g2p_phonemize_marked (g, line);
      else
        r = ru_g2p_phonemize (g, line);
      if (synth)
        {
          gsize ns = 0;
          gint64 t = g_get_monotonic_time ();
          gfloat *wav = kokoro_synth (m, voice, r, argc > 8 ? g_ascii_strtod (argv[8], NULL) : 1.0f,
                                      &ns, &error);
          gchar *path;
          if (wav == NULL)
            {
              fprintf (stderr, "line %d: %s\n", n + 1, error->message);
              g_clear_error (&error);
              g_free (r);
              n++;
              continue;
            }
          path = g_strdup_printf ("%s%02d.wav", argv[7], ++n);
          write_wav (path, wav, ns);
          fprintf (stderr, "%s  %.2f s audio in %.2f s  %s\n", path,
                   (double) ns / KOKORO_SAMPLE_RATE, (g_get_monotonic_time () - t) / 1e6, r);
          g_free (path);
          g_free (wav);
        }
      else
        printf ("%s\n", r);
      g_free (r);
    }
  kokoro_model_free (m);
  g_free (voice);
  ru_g2p_free (g);
  return 0;
err:
  fprintf (stderr, "kokoro-test: %s\n", error->message);
  return 1;
}

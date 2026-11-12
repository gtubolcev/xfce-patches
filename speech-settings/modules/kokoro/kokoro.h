/*
 * kokoro - the Russian Kokoro (zaakirio/kokoro-ru, onnx-community layout)
 * through the onnxruntime C API
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#ifndef KOKORO_H
#define KOKORO_H

#include <glib.h>

#define KOKORO_SAMPLE_RATE 24000
#define KOKORO_MAX_PHONEMES 510

typedef struct KokoroModel KokoroModel;

/* onnx: the model file; config: config.json with "vocab" */
KokoroModel *kokoro_model_new (const gchar *onnx, const gchar *config,
                               int threads, GError **error);
void kokoro_model_free (KokoroModel *m);

/* voice: raw float32 [510][256] (voices/<name>.bin); returned as floats,
 * free with g_free */
gfloat *kokoro_voice_load (const gchar *path, GError **error);

/* ipa: at most KOKORO_MAX_PHONEMES characters; returns float samples at
 * KOKORO_SAMPLE_RATE (g_free), *n_samples set */
gfloat *kokoro_synth (KokoroModel *m, const gfloat *voice, const gchar *ipa,
                      gfloat speed, gsize *n_samples, GError **error);

#endif

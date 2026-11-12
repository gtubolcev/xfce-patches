/*
 * vosk - Vosk TTS (alphacep) models through the ONNX Runtime C API
 *
 * A port of the Python vosk_tts package (Apache-2.0): the dictionary with
 * stress marks, g2p for the words missing in it, the WordPiece tokenizer of
 * the BERT model and the "multistream_v1" input of the acoustic model.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#ifndef VOSK_H
#define VOSK_H

#include <glib.h>

typedef struct
{
  gchar *name;    /* female_0 */
  int id;         /* sid */
} VoskSpeaker;

typedef struct VoskModel VoskModel;

/* Reads config.json only (cheap); the ONNX models load in vosk_model_load. */
VoskModel *vosk_model_open (const gchar *dir, GError **error);
void vosk_model_free (VoskModel *m);

/* language from the directory name: vosk-model-tts-ru-0.9-multi -> ru */
const gchar *vosk_model_language (VoskModel *m);
const gchar *vosk_model_dir (VoskModel *m);
int vosk_model_sample_rate (VoskModel *m);
/* VoskSpeaker, ordered by id */
GPtrArray *vosk_model_speakers (VoskModel *m);

/* Loads the ONNX models, the vocabulary and the dictionary (sorted copy
 * made in cache_dir on first use unless the model has dictionary.sorted).
 * use_bert FALSE leaves the BERT model out (zeros; less memory). */
gboolean vosk_model_load (VoskModel *m, int threads, gboolean use_bert,
                          const gchar *cache_dir, GError **error);
gboolean vosk_model_loaded (VoskModel *m);
void vosk_model_unload (VoskModel *m);

/* Synthesizes one sentence; returns float samples (g_free), NULL on error.
 * speech_rate 1.0 is normal.  If trace is not NULL, the tokens and the
 * phoneme ids are appended to it (for the tests). */
float *vosk_synth (VoskModel *m, const gchar *text, int sid, float speech_rate,
                   gsize *n_samples, GString *trace, GError **error);

/* Writes the best entry of each word of a Vosk dictionary, sorted by bytes:
 * "word\tphonemes\n".  Ties keep the first entry, like vosk_tts. */
gboolean vosk_make_sorted_dictionary (const gchar *src, const gchar *dst, GError **error);

#endif

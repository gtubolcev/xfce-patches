/*
 * kokoro - see kokoro.h
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include "kokoro.h"

#include <json-glib/json-glib.h>
#include <onnxruntime/onnxruntime_c_api.h>
#include <string.h>

#define STYLE_DIM 256
#define KOKORO_ERROR (g_quark_from_static_string ("kokoro-error"))

struct KokoroModel
{
  OrtSession *session;
  GHashTable *vocab;            /* gunichar -> id + 1 */
  gchar *output;
};

static const OrtApi *ort = NULL;
static OrtEnv *ort_env = NULL;
static OrtMemoryInfo *ort_memory = NULL;

static gboolean
ort_ok (OrtStatus *status, GError **error)
{
  if (status == NULL)
    return TRUE;
  g_set_error (error, KOKORO_ERROR, 1, "onnxruntime: %s", ort->GetErrorMessage (status));
  ort->ReleaseStatus (status);
  return FALSE;
}

static gboolean
ort_init (GError **error)
{
  if (ort != NULL)
    return TRUE;
  ort = OrtGetApiBase ()->GetApi (ORT_API_VERSION);
  if (ort == NULL)
    {
      g_set_error (error, KOKORO_ERROR, 1, "onnxruntime: API version %d not available", ORT_API_VERSION);
      return FALSE;
    }
  if (!ort_ok (ort->CreateEnv (ORT_LOGGING_LEVEL_ERROR, "kokoro", &ort_env), error))
    return FALSE;
  return ort_ok (ort->CreateCpuMemoryInfo (OrtArenaAllocator, OrtMemTypeDefault, &ort_memory), error);
}

static gboolean
vocab_load (KokoroModel *m, const gchar *path, GError **error)
{
  JsonParser *p = json_parser_new ();
  JsonNode *root;
  JsonObject *vocab;
  GList *members;

  if (!json_parser_load_from_file (p, path, error))
    {
      g_object_unref (p);
      return FALSE;
    }
  root = json_parser_get_root (p);
  if (!JSON_NODE_HOLDS_OBJECT (root)
      || !json_object_has_member (json_node_get_object (root), "vocab"))
    {
      g_set_error (error, KOKORO_ERROR, 1, "%s: no \"vocab\"", path);
      g_object_unref (p);
      return FALSE;
    }
  vocab = json_object_get_object_member (json_node_get_object (root), "vocab");
  m->vocab = g_hash_table_new (NULL, NULL);
  members = json_object_get_members (vocab);
  for (GList *l = members; l != NULL; l = l->next)
    {
      const gchar *sym = l->data;
      if (g_utf8_strlen (sym, -1) != 1)
        continue;
      g_hash_table_insert (m->vocab, GUINT_TO_POINTER (g_utf8_get_char (sym)),
                           GINT_TO_POINTER (json_object_get_int_member (vocab, sym) + 1));
    }
  g_list_free (members);
  g_object_unref (p);
  return TRUE;
}

KokoroModel *
kokoro_model_new (const gchar *onnx, const gchar *config, int threads, GError **error)
{
  KokoroModel *m;
  OrtSessionOptions *so = NULL;
  OrtAllocator *alloc;
  char *name = NULL;
  gboolean ok;

  if (!ort_init (error))
    return NULL;
  m = g_new0 (KokoroModel, 1);
  if (!vocab_load (m, config, error))
    goto fail;
  if (!ort_ok (ort->CreateSessionOptions (&so), error))
    goto fail;
  ok = ort_ok (ort->SetIntraOpNumThreads (so, threads), error)
       && ort_ok (ort->SetInterOpNumThreads (so, 1), error)
       && ort_ok (ort->SetSessionGraphOptimizationLevel (so, ORT_ENABLE_ALL), error)
       && ort_ok (ort->CreateSession (ort_env, onnx, so, &m->session), error);
  ort->ReleaseSessionOptions (so);
  if (!ok)
    goto fail;
  if (!ort_ok (ort->GetAllocatorWithDefaultOptions (&alloc), error)
      || !ort_ok (ort->SessionGetOutputName (m->session, 0, alloc, &name), error))
    goto fail;
  m->output = g_strdup (name);
  alloc->Free (alloc, name);
  return m;
fail:
  kokoro_model_free (m);
  return NULL;
}

void
kokoro_model_free (KokoroModel *m)
{
  if (m == NULL)
    return;
  if (m->session != NULL)
    ort->ReleaseSession (m->session);
  if (m->vocab != NULL)
    g_hash_table_unref (m->vocab);
  g_free (m->output);
  g_free (m);
}

gfloat *
kokoro_voice_load (const gchar *path, GError **error)
{
  gchar *data;
  gsize len;

  if (!g_file_get_contents (path, &data, &len, error))
    return NULL;
  if (len != (gsize) KOKORO_MAX_PHONEMES * STYLE_DIM * sizeof (gfloat))
    {
      g_set_error (error, KOKORO_ERROR, 1, "%s: %zu bytes, not a voice", path, len);
      g_free (data);
      return NULL;
    }
  return (gfloat *) data;
}

gfloat *
kokoro_synth (KokoroModel *m, const gfloat *voice, const gchar *ipa,
              gfloat speed, gsize *n_samples, GError **error)
{
  glong len = g_utf8_strlen (ipa, -1);
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  const char *in_names[] = { "input_ids", "style", "speed" };
  const char *out_names[] = { m->output };
  OrtValue *in[3] = { NULL, NULL, NULL }, *out = NULL;
  OrtTensorTypeAndShapeInfo *info = NULL;
  gint64 zero = 0, ids_shape[2], style_shape[2] = { 1, STYLE_DIM }, speed_shape[1] = { 1 };
  gfloat *wav = NULL, *data;
  size_t count;

  if (len == 0 || len > KOKORO_MAX_PHONEMES)
    {
      g_set_error (error, KOKORO_ERROR, 1, "%ld phonemes (1..%d)", len, KOKORO_MAX_PHONEMES);
      g_array_free (ids, TRUE);
      return NULL;
    }
  /* [0, ids..., 0]; unknown symbols dropped, the style row still counts them */
  g_array_append_val (ids, zero);
  for (const gchar *p = ipa; *p != '\0'; p = g_utf8_next_char (p))
    {
      gint64 id = GPOINTER_TO_INT (g_hash_table_lookup (m->vocab,
                                   GUINT_TO_POINTER (g_utf8_get_char (p))));
      if (id > 0)
        {
          id--;
          g_array_append_val (ids, id);
        }
    }
  g_array_append_val (ids, zero);
  ids_shape[0] = 1;
  ids_shape[1] = ids->len;

  if (ort_ok (ort->CreateTensorWithDataAsOrtValue (ort_memory, ids->data, ids->len * sizeof (gint64),
                                                   ids_shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[0]), error)
      && ort_ok (ort->CreateTensorWithDataAsOrtValue (ort_memory, (void *) (voice + (len - 1) * STYLE_DIM),
                                                      STYLE_DIM * sizeof (gfloat), style_shape, 2,
                                                      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[1]), error)
      && ort_ok (ort->CreateTensorWithDataAsOrtValue (ort_memory, &speed, sizeof (gfloat), speed_shape, 1,
                                                      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[2]), error)
      && ort_ok (ort->Run (m->session, NULL, in_names, (const OrtValue *const *) in, 3,
                           out_names, 1, &out), error)
      && ort_ok (ort->GetTensorTypeAndShape (out, &info), error)
      && ort_ok (ort->GetTensorShapeElementCount (info, &count), error)
      && ort_ok (ort->GetTensorMutableData (out, (void **) &data), error))
    {
      wav = g_memdup2 (data, count * sizeof (gfloat));
      *n_samples = count;
    }
  if (info != NULL)
    ort->ReleaseTensorTypeAndShapeInfo (info);
  if (out != NULL)
    ort->ReleaseValue (out);
  for (int i = 0; i < 3; i++)
    if (in[i] != NULL)
      ort->ReleaseValue (in[i]);
  g_array_free (ids, TRUE);
  return wav;
}

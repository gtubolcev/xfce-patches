/*
 * vosk - Vosk TTS (alphacep) models through the ONNX Runtime C API
 *
 * Follows vosk_tts/synth.py, model.py and g2p.py (alphacep/vosk-tts,
 * Apache-2.0); see vosk.h.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include "vosk.h"

#include <errno.h>
#include <fcntl.h>
#include <json-glib/json-glib.h>
#include <onnxruntime/onnxruntime_c_api.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define BERT_DIM 768
#define BERT_MAX_TOKENS 512

#define VOSK_ERROR (g_quark_from_static_string ("vosk"))

struct VoskModel
{
  gchar *dir;
  gchar *language;
  gchar *type;              /* model_type */
  int sample_rate;
  float noise_level, duration_noise_level, scale;
  GHashTable *phoneme_ids;  /* phoneme -> id + 1 (GINT_TO_POINTER) */
  GPtrArray *speakers;

  /* loaded */
  OrtSession *acoustic;
  OrtSession *bert;
  GHashTable *vocab;        /* WordPiece -> id + 1 */
  int unk_id, cls_id, sep_id;
  const char *dict;         /* mmap of the sorted dictionary */
  gsize dict_size;
};

static const OrtApi *ort = NULL;
static OrtEnv *ort_env = NULL;
static OrtMemoryInfo *ort_memory = NULL;



static gboolean
ort_ok (OrtStatus *status, GError **error)
{
  if (status == NULL)
    return TRUE;
  g_set_error (error, VOSK_ERROR, 1, "onnxruntime: %s", ort->GetErrorMessage (status));
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
      g_set_error (error, VOSK_ERROR, 1, "onnxruntime: API version %d not available", ORT_API_VERSION);
      return FALSE;
    }
  if (!ort_ok (ort->CreateEnv (ORT_LOGGING_LEVEL_ERROR, "vosk", &ort_env), error))
    return FALSE;
  return ort_ok (ort->CreateCpuMemoryInfo (OrtArenaAllocator, OrtMemTypeDefault, &ort_memory), error);
}



/* ------------------------------------------------------------------ config */

static int
phoneme_id (VoskModel *m, const gchar *p)
{
  gpointer v = g_hash_table_lookup (m->phoneme_ids, p);
  return v != NULL ? GPOINTER_TO_INT (v) - 1 : -1;
}



static gint
speaker_compare (gconstpointer a, gconstpointer b)
{
  return (*(VoskSpeaker * const *) a)->id - (*(VoskSpeaker * const *) b)->id;
}



static void
speaker_free (gpointer p)
{
  VoskSpeaker *s = p;
  g_free (s->name);
  g_free (s);
}



VoskModel *
vosk_model_open (const gchar *dir, GError **error)
{
  gchar *path = g_build_filename (dir, "config.json", NULL);
  JsonParser *parser = json_parser_new ();
  JsonObject *root, *o;
  VoskModel *m;
  gchar *base, *p;

  if (!json_parser_load_from_file (parser, path, error))
    {
      g_free (path);
      g_object_unref (parser);
      return NULL;
    }
  g_free (path);
  root = json_node_get_object (json_parser_get_root (parser));
  if (root == NULL || !json_object_has_member (root, "phoneme_id_map"))
    {
      g_set_error (error, VOSK_ERROR, 1, "%s: not a Vosk TTS model", dir);
      g_object_unref (parser);
      return NULL;
    }

  m = g_new0 (VoskModel, 1);
  m->dir = g_strdup (dir);
  m->type = g_strdup (json_object_get_string_member_with_default (root, "model_type", ""));
  m->sample_rate = 22050;
  m->noise_level = 0.8f;
  m->duration_noise_level = 0.8f;
  m->scale = 1.0f;
  if ((o = json_object_get_object_member (root, "audio")) != NULL)
    m->sample_rate = (int) json_object_get_int_member_with_default (o, "sample_rate", 22050);
  if (json_object_has_member (root, "inference")
      && (o = json_object_get_object_member (root, "inference")) != NULL)
    {
      m->noise_level = json_object_get_double_member_with_default (o, "noise_level", 0.8);
      m->duration_noise_level = json_object_get_double_member_with_default (o, "duration_noise_level", 0.8);
      m->scale = json_object_get_double_member_with_default (o, "scale", 1.0);
    }

  m->phoneme_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  o = json_object_get_object_member (root, "phoneme_id_map");
  {
    GList *members = json_object_get_members (o);
    for (GList *l = members; l != NULL; l = l->next)
      {
        JsonNode *n = json_object_get_member (o, l->data);
        gint64 id;
        if (JSON_NODE_HOLDS_ARRAY (n))   /* older models: a list, the first id */
          id = json_array_get_int_element (json_node_get_array (n), 0);
        else
          id = json_node_get_int (n);
        g_hash_table_insert (m->phoneme_ids, g_strdup (l->data), GINT_TO_POINTER ((int) id + 1));
      }
    g_list_free (members);
  }

  m->speakers = g_ptr_array_new_with_free_func (speaker_free);
  if (json_object_has_member (root, "speaker_id_map")
      && (o = json_object_get_object_member (root, "speaker_id_map")) != NULL)
    {
      GList *members = json_object_get_members (o);
      for (GList *l = members; l != NULL; l = l->next)
        {
          VoskSpeaker *s = g_new0 (VoskSpeaker, 1);
          s->name = g_strdup (l->data);
          s->id = (int) json_object_get_int_member (o, l->data);
          g_ptr_array_add (m->speakers, s);
        }
      g_list_free (members);
      g_ptr_array_sort (m->speakers, speaker_compare);
    }
  if (m->speakers->len == 0)
    {
      VoskSpeaker *s = g_new0 (VoskSpeaker, 1);
      s->name = g_strdup ("default");
      g_ptr_array_add (m->speakers, s);
    }

  /* vosk-model-tts-ru-0.9-multi -> ru */
  base = g_path_get_basename (dir);
  p = strstr (base, "tts-");
  if (p != NULL)
    {
      p += 4;
      m->language = g_strndup (p, strcspn (p, "-_"));
    }
  else
    m->language = g_strdup ("ru");
  g_free (base);
  g_object_unref (parser);

  if (strcmp (m->type, "multistream_v1") != 0)
    {
      g_set_error (error, VOSK_ERROR, 1, "%s: model type \"%s\" is not supported (only multistream_v1)",
                   dir, m->type);
      vosk_model_free (m);
      return NULL;
    }
  return m;
}



void
vosk_model_free (VoskModel *m)
{
  if (m == NULL)
    return;
  vosk_model_unload (m);
  g_free (m->dir);
  g_free (m->language);
  g_free (m->type);
  g_hash_table_unref (m->phoneme_ids);
  g_ptr_array_unref (m->speakers);
  g_free (m);
}



const gchar *vosk_model_language (VoskModel *m) { return m->language; }
const gchar *vosk_model_dir (VoskModel *m) { return m->dir; }
int vosk_model_sample_rate (VoskModel *m) { return m->sample_rate; }
GPtrArray *vosk_model_speakers (VoskModel *m) { return m->speakers; }
gboolean vosk_model_loaded (VoskModel *m) { return m->acoustic != NULL; }



/* -------------------------------------------------------------- dictionary */

typedef struct
{
  const char *data;
  guint32 *lines;
} SortCtx;

static SortCtx sort_ctx;

/* the word of the line starting at off: up to the first space */
static gsize
word_len (const char *data, guint32 off, gsize size)
{
  gsize n = 0;
  while (off + n < size && data[off + n] != ' ' && data[off + n] != '\t' && data[off + n] != '\n')
    n++;
  return n;
}

static gsize sort_size;

static int
line_compare (const void *a, const void *b)
{
  guint32 x = *(const guint32 *) a, y = *(const guint32 *) b;
  gsize lx = word_len (sort_ctx.data, x, sort_size), ly = word_len (sort_ctx.data, y, sort_size);
  int c = memcmp (sort_ctx.data + x, sort_ctx.data + y, MIN (lx, ly));

  if (c != 0)
    return c;
  if (lx != ly)
    return lx < ly ? -1 : 1;
  return x < y ? -1 : (x > y ? 1 : 0);   /* stable: the file order */
}



gboolean
vosk_make_sorted_dictionary (const gchar *src, const gchar *dst, GError **error)
{
  GMappedFile *mf = g_mapped_file_new (src, FALSE, error);
  const char *data;
  gsize size;
  GArray *lines;
  gchar *tmp;
  FILE *out;
  gboolean ok = TRUE;

  if (mf == NULL)
    return FALSE;
  data = g_mapped_file_get_contents (mf);
  size = g_mapped_file_get_length (mf);
  if (size >= G_MAXUINT32)
    {
      g_set_error (error, VOSK_ERROR, 1, "%s: too large", src);
      g_mapped_file_unref (mf);
      return FALSE;
    }

  lines = g_array_new (FALSE, FALSE, sizeof (guint32));
  for (gsize off = 0; off < size;)
    {
      const char *nl = memchr (data + off, '\n', size - off);
      gsize end = nl != NULL ? (gsize) (nl - data) : size;
      if (end > off)
        {
          guint32 o = (guint32) off;
          g_array_append_val (lines, o);
        }
      off = end + 1;
    }
  sort_ctx.data = data;
  sort_size = size;
  qsort (lines->data, lines->len, sizeof (guint32), line_compare);

  tmp = g_strconcat (dst, ".tmp", NULL);
  out = fopen (tmp, "w");
  if (out == NULL)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno), "%s: %s", tmp, g_strerror (errno));
      ok = FALSE;
    }
  for (guint i = 0; ok && i < lines->len;)
    {
      guint32 first = g_array_index (lines, guint32, i);
      gsize wl = word_len (data, first, size);
      guint32 best = first;
      double best_prob = -1.0;
      guint j = i;

      /* the entries of this word, in the file order: keep the first best one */
      for (; j < lines->len; j++)
        {
          guint32 o = g_array_index (lines, guint32, j);
          gchar *endp;
          double prob;
          if (word_len (data, o, size) != wl || memcmp (data + o, data + first, wl) != 0)
            break;
          prob = g_ascii_strtod (data + o + wl, &endp);
          if (prob > best_prob)
            {
              best_prob = prob;
              best = o;
            }
        }
      {
        /* word \t phonemes (after the probability) */
        const char *p = data + best + wl;
        const char *nl = memchr (p, '\n', size - (p - data));
        const char *end = nl != NULL ? nl : data + size;
        while (p < end && (*p == ' ' || *p == '\t'))
          p++;
        while (p < end && *p != ' ' && *p != '\t')   /* the probability */
          p++;
        while (p < end && (*p == ' ' || *p == '\t'))
          p++;
        while (end > p && (end[-1] == ' ' || end[-1] == '\r' || end[-1] == '\t'))
          end--;
        fwrite (data + best, 1, wl, out);
        fputc ('\t', out);
        fwrite (p, 1, end - p, out);
        fputc ('\n', out);
      }
      i = j;
    }
  if (out != NULL && fclose (out) != 0 && ok)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno), "%s: %s", tmp, g_strerror (errno));
      ok = FALSE;
    }
  if (ok && rename (tmp, dst) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno), "%s: %s", dst, g_strerror (errno));
      ok = FALSE;
    }
  if (!ok)
    unlink (tmp);
  g_free (tmp);
  g_array_free (lines, TRUE);
  g_mapped_file_unref (mf);
  return ok;
}



/* the phonemes of a word (a newly allocated string) or NULL */
static gchar *
dict_lookup (VoskModel *m, const gchar *word)
{
  gsize lo = 0, hi = m->dict_size, wl = strlen (word);

  if (m->dict == NULL || wl == 0)
    return NULL;
  /* binary search over the bytes: back up to the start of the line */
  while (lo < hi)
    {
      gsize mid = lo + (hi - lo) / 2, start = mid, tab;
      int c;

      while (start > lo && m->dict[start - 1] != '\n')
        start--;
      tab = start;
      while (tab < m->dict_size && m->dict[tab] != '\t' && m->dict[tab] != '\n')
        tab++;
      c = memcmp (m->dict + start, word, MIN (tab - start, wl));
      if (c == 0 && tab - start != wl)
        c = tab - start < wl ? -1 : 1;
      if (c == 0)
        {
          const char *p = m->dict + tab + 1;
          const char *nl = memchr (p, '\n', m->dict_size - (p - m->dict));
          return g_strndup (p, (nl != NULL ? nl : m->dict + m->dict_size) - p);
        }
      if (c < 0)
        {
          /* the word is after this line */
          const char *nl = memchr (m->dict + tab, '\n', m->dict_size - tab);
          lo = nl != NULL ? (gsize) (nl - m->dict) + 1 : m->dict_size;
        }
      else
        hi = start;
    }
  return NULL;
}



static gboolean
dict_open (VoskModel *m, const gchar *cache_dir, GError **error)
{
  gchar *sorted = g_build_filename (m->dir, "dictionary.sorted", NULL);
  gchar *src = g_build_filename (m->dir, "dictionary", NULL);
  struct stat ss, ds;
  int fd;
  gboolean ok = FALSE;

  if (!g_file_test (sorted, G_FILE_TEST_EXISTS))
    {
      gchar *base = g_path_get_basename (m->dir);
      gchar *name = g_strconcat (base, ".dictionary", NULL);

      g_free (sorted);
      sorted = g_build_filename (cache_dir, name, NULL);
      g_free (name);
      g_free (base);
      if (stat (src, &ss) != 0)
        {
          /* no dictionary: g2p only */
          ok = TRUE;
          goto out;
        }
      if (stat (sorted, &ds) != 0 || ds.st_mtime < ss.st_mtime)
        {
          g_mkdir_with_parents (cache_dir, 0700);
          if (!vosk_make_sorted_dictionary (src, sorted, error))
            goto out;
        }
    }
  fd = open (sorted, O_RDONLY | O_CLOEXEC);
  if (fd < 0 || fstat (fd, &ds) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno), "%s: %s", sorted, g_strerror (errno));
      if (fd >= 0)
        close (fd);
      goto out;
    }
  if (ds.st_size > 0)
    {
      void *p = mmap (NULL, ds.st_size, PROT_READ, MAP_SHARED, fd, 0);
      if (p == MAP_FAILED)
        {
          g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno), "%s: %s", sorted, g_strerror (errno));
          close (fd);
          goto out;
        }
      m->dict = p;
      m->dict_size = ds.st_size;
    }
  close (fd);
  ok = TRUE;
out:
  g_free (sorted);
  g_free (src);
  return ok;
}



/* --------------------------------------------------------------------- g2p */

/* vosk_tts/g2p.py: convert(): Russian letters to phonemes, '+' marks stress */

static const char *
softhard (gunichar c)
{
  switch (c)
    {
    case 0x0431: return "b";   case 0x0432: return "v";   case 0x0433: return "g";
    case 0x0413: return "g";   case 0x0434: return "d";   case 0x0437: return "z";
    case 0x043A: return "k";   case 0x043B: return "l";   case 0x043C: return "m";
    case 0x043D: return "n";   case 0x043F: return "p";   case 0x0440: return "r";
    case 0x0441: return "s";   case 0x0442: return "t";   case 0x0444: return "f";
    case 0x0445: return "h";
    default: return NULL;
    }
}

static const char *
other_cons (gunichar c)
{
  switch (c)
    {
    case 0x0436: return "zh";  case 0x0446: return "c";   case 0x0447: return "ch";
    case 0x0448: return "sh";  case 0x0449: return "sch"; case 0x0439: return "j";
    default: return NULL;
    }
}

static const char *
vowel (gunichar c)
{
  switch (c)
    {
    case 0x0430: case 0x044F: return "a";   /* а я */
    case 0x0443: case 0x044E: return "u";   /* у ю */
    case 0x043E: case 0x0451: return "o";   /* о ё */
    case 0x044D: case 0x0435: return "e";   /* э е */
    case 0x0438: return "i";
    case 0x044B: return "y";
    default: return NULL;
    }
}

/* я ё ю и ь е */
static gboolean
is_softletter (gunichar c)
{
  return c == 0x044F || c == 0x0451 || c == 0x044E || c == 0x0438 || c == 0x044C || c == 0x0435;
}

/* # ъ ь а я о ё у ю э е и ы - */
static gboolean
is_startsyl (gunichar c)
{
  return c == '#' || c == '-' || c == 0x044A || c == 0x044C || vowel (c) != NULL;
}

/* # + - ь ъ: dropped */
static gboolean
is_other (gunichar c)
{
  return c == '#' || c == '+' || c == '-' || c == 0x044C || c == 0x044A;
}

static gboolean
is_iotated (gunichar c)   /* я ю е ё */
{
  return c == 0x044F || c == 0x044E || c == 0x0435 || c == 0x0451;
}

typedef struct
{
  gunichar c;
  const char *mapped;   /* set by the palatalization */
  int stress;
} G2pPhone;

/* appends the phonemes of word to out (space separated) */
static void
g2p_convert (const gchar *word, GPtrArray *out)
{
  GArray *ph = g_array_new (FALSE, TRUE, sizeof (G2pPhone));
  G2pPhone hash = { '#', NULL, 0 };
  int stress = 0;
  gboolean prev_startsyl = FALSE;

  g_array_append_val (ph, hash);
  for (const gchar *p = word; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (c == '+')
        stress = 1;
      else
        {
          G2pPhone x = { c, NULL, stress };
          g_array_append_val (ph, x);
          stress = 0;
        }
    }
  g_array_append_val (ph, hash);

  /* pallatize */
  for (guint i = 0; i + 1 < ph->len; i++)
    {
      G2pPhone *x = &g_array_index (ph, G2pPhone, i);
      const char *s = softhard (x->c);
      if (s != NULL)
        {
          if (is_softletter (g_array_index (ph, G2pPhone, i + 1).c))
            x->mapped = g_intern_string (g_strconcat (s, "j", NULL));  /* few distinct */
          else
            x->mapped = s;
          x->stress = 0;
        }
      if ((s = other_cons (x->c)) != NULL)
        {
          x->mapped = s;
          x->stress = 0;
        }
    }

  /* convert_vowels; prev is the phone after the palatalization, so a mapped
   * consonant never starts a syllable */
  for (guint i = 0; i < ph->len; i++)
    {
      G2pPhone *x = &g_array_index (ph, G2pPhone, i);
      const char *v;

      if (prev_startsyl && is_iotated (x->c))
        g_ptr_array_add (out, g_strdup ("j"));
      if (x->mapped == NULL && (v = vowel (x->c)) != NULL)
        g_ptr_array_add (out, g_strdup_printf ("%s%d", v, x->stress));
      else if (x->mapped != NULL)
        g_ptr_array_add (out, g_strdup (x->mapped));
      else if (!is_other (x->c))
        {
          gchar buf[8];
          buf[g_unichar_to_utf8 (x->c, buf)] = '\0';
          g_ptr_array_add (out, g_strdup (buf));
        }
      prev_startsyl = x->mapped == NULL && is_startsyl (x->c);
    }
  g_array_free (ph, TRUE);
}



/* --------------------------------------------------------------- tokenizer */

/* BertWordPieceTokenizer(lowercase=True): clean the text, strip the accents
 * (NFD, drop the marks: й -> и, ё -> е), lower case, split at whitespace and
 * punctuation, WordPiece with "##".  Returns the token ids; tokens gets the
 * token strings. */

static gboolean
bert_is_punct (gunichar c)
{
  if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126))
    return TRUE;
  switch (g_unichar_type (c))
    {
    case G_UNICODE_CONNECT_PUNCTUATION: case G_UNICODE_DASH_PUNCTUATION:
    case G_UNICODE_CLOSE_PUNCTUATION: case G_UNICODE_FINAL_PUNCTUATION:
    case G_UNICODE_INITIAL_PUNCTUATION: case G_UNICODE_OTHER_PUNCTUATION:
    case G_UNICODE_OPEN_PUNCTUATION:
      return TRUE;
    default:
      return FALSE;
    }
}



static int
vocab_id (VoskModel *m, const gchar *piece)
{
  gpointer v = g_hash_table_lookup (m->vocab, piece);
  return v != NULL ? GPOINTER_TO_INT (v) - 1 : -1;
}



static void
wordpiece (VoskModel *m, const gchar *word, GArray *ids, GPtrArray *tokens)
{
  glong n = g_utf8_strlen (word, -1);
  GArray *wids = g_array_new (FALSE, FALSE, sizeof (int));
  GPtrArray *wtok = g_ptr_array_new_with_free_func (g_free);
  const gchar *start = word;
  gboolean bad = n > 100;

  while (!bad && *start != '\0')
    {
      const gchar *end = start + strlen (start);
      int id = -1;
      gchar *piece = NULL;

      while (end > start)
        {
          gchar *sub = g_strndup (start, end - start);
          piece = start == word ? sub : g_strconcat ("##", sub, NULL);
          if (piece != sub)
            g_free (sub);
          if ((id = vocab_id (m, piece)) >= 0)
            break;
          g_free (piece);
          piece = NULL;
          end = g_utf8_prev_char (end);
        }
      if (id < 0)
        bad = TRUE;
      else
        {
          g_array_append_val (wids, id);
          g_ptr_array_add (wtok, piece);
          start = end;
        }
    }
  if (bad)
    {
      g_array_append_val (ids, m->unk_id);
      g_ptr_array_add (tokens, g_strdup ("[UNK]"));
    }
  else
    {
      g_array_append_vals (ids, wids->data, wids->len);
      for (guint i = 0; i < wtok->len; i++)
        g_ptr_array_add (tokens, g_strdup (g_ptr_array_index (wtok, i)));
    }
  g_array_free (wids, TRUE);
  g_ptr_array_free (wtok, TRUE);
}



static GArray *
bert_tokenize (VoskModel *m, const gchar *text, GPtrArray *tokens)
{
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (int));
  gchar *nfd = g_utf8_normalize (text, -1, G_NORMALIZE_NFD);
  GString *clean = g_string_new (NULL);
  GString *word = g_string_new (NULL);
  gchar *lower;

  /* clean_text + strip_accents */
  for (const gchar *p = nfd; p != NULL && *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (c == 0 || c == 0xFFFD || g_unichar_type (c) == G_UNICODE_NON_SPACING_MARK)
        continue;
      if (c == '\t' || c == '\n' || c == '\r' || g_unichar_isspace (c))
        g_string_append_c (clean, ' ');
      else if (g_unichar_iscntrl (c))
        continue;
      else
        g_string_append_unichar (clean, c);
    }
  lower = g_utf8_strdown (clean->str, -1);

  g_array_append_val (ids, m->cls_id);
  g_ptr_array_add (tokens, g_strdup ("[CLS]"));
  for (const gchar *p = lower;; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      gboolean space = c == ' ' || c == 0, punct = c != 0 && bert_is_punct (c);

      if ((space || punct) && word->len > 0)
        {
          wordpiece (m, word->str, ids, tokens);
          g_string_truncate (word, 0);
        }
      if (punct)
        {
          gchar buf[8];
          buf[g_unichar_to_utf8 (c, buf)] = '\0';
          wordpiece (m, buf, ids, tokens);
        }
      else if (!space)
        g_string_append_unichar (word, c);
      if (c == 0)
        break;
    }
  g_array_append_val (ids, m->sep_id);
  g_ptr_array_add (tokens, g_strdup ("[SEP]"));

  g_free (lower);
  g_free (nfd);
  g_string_free (clean, TRUE);
  g_string_free (word, TRUE);
  return ids;
}



/* --------------------------------------------------------------- the models */

static gboolean
session_new (const gchar *path, int threads, gboolean parallel, OrtSession **out, GError **error)
{
  OrtSessionOptions *so = NULL;
  gboolean ok;

  if (!ort_ok (ort->CreateSessionOptions (&so), error))
    return FALSE;
  /* The parallel executor: in the sequential one the memory planner of ORT
   * 1.23 (FindReusableTensor walks the whole free list for every output)
   * takes 35-70 s on the 17000 nodes of the acoustic model; the parallel
   * one plans no buffer reuse and loads in ~4 s, synthesizing as fast once
   * warm.  It needs more than one inter-op thread, else ORT silently falls
   * back to sequential.  ORT_ENABLE_ALL only adds ~2 s of load here and no
   * speed, so the basic optimizations are enough.  The BERT model is
   * small and plain: sequential with all optimizations, which was faster
   * for short sentences. */
  ok = ort_ok (ort->SetIntraOpNumThreads (so, threads), error)
       && ort_ok (ort->SetInterOpNumThreads (so, parallel ? 2 : 1), error)
       && ort_ok (ort->SetSessionExecutionMode (so, parallel ? ORT_PARALLEL : ORT_SEQUENTIAL), error)
       && ort_ok (ort->SetSessionGraphOptimizationLevel (so, parallel ? ORT_ENABLE_BASIC : ORT_ENABLE_ALL), error)
       && ort_ok (ort->CreateSession (ort_env, path, so, out), error);
  ort->ReleaseSessionOptions (so);
  return ok;
}



static gboolean
vocab_load (VoskModel *m, const gchar *path, GError **error)
{
  gchar *contents;
  gchar **lines;
  int id = 0;

  if (!g_file_get_contents (path, &contents, NULL, error))
    return FALSE;
  m->vocab = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  lines = g_strsplit (contents, "\n", -1);
  g_free (contents);
  for (gchar **l = lines; *l != NULL; l++, id++)
    {
      gchar *t = g_strchomp (*l);
      if (*t != '\0' && !g_hash_table_contains (m->vocab, t))
        g_hash_table_insert (m->vocab, g_strdup (t), GINT_TO_POINTER (id + 1));
    }
  g_strfreev (lines);
  m->unk_id = vocab_id (m, "[UNK]");
  m->cls_id = vocab_id (m, "[CLS]");
  m->sep_id = vocab_id (m, "[SEP]");
  if (m->unk_id < 0 || m->cls_id < 0 || m->sep_id < 0)
    {
      g_set_error (error, VOSK_ERROR, 1, "%s: no [UNK], [CLS] or [SEP]", path);
      return FALSE;
    }
  return TRUE;
}



gboolean
vosk_model_load (VoskModel *m, int threads, gboolean use_bert, const gchar *cache_dir, GError **error)
{
  gchar *path;
  gboolean ok;

  if (m->acoustic != NULL)
    return TRUE;
  if (!ort_init (error))
    return FALSE;
  if (!dict_open (m, cache_dir, error))
    return FALSE;

  path = g_build_filename (m->dir, "model.onnx", NULL);
  ok = session_new (path, threads, TRUE, &m->acoustic, error);
  g_free (path);

  path = g_build_filename (m->dir, "bert", "model.onnx", NULL);
  if (ok && use_bert && g_file_test (path, G_FILE_TEST_EXISTS))
    {
      gchar *vocab = g_build_filename (m->dir, "bert", "vocab.txt", NULL);
      ok = vocab_load (m, vocab, error) && session_new (path, threads, FALSE, &m->bert, error);
      g_free (vocab);
    }
  g_free (path);
  if (!ok)
    vosk_model_unload (m);
  return ok;
}



void
vosk_model_unload (VoskModel *m)
{
  if (m->acoustic != NULL)
    ort->ReleaseSession (m->acoustic);
  if (m->bert != NULL)
    ort->ReleaseSession (m->bert);
  m->acoustic = m->bert = NULL;
  if (m->vocab != NULL)
    g_hash_table_unref (m->vocab);
  m->vocab = NULL;
  if (m->dict != NULL)
    munmap ((void *) m->dict, m->dict_size);
  m->dict = NULL;
  m->dict_size = 0;
}



/* ------------------------------------------------------------------ synthesis */

/* Runs BERT; returns the rows of the words (vosk_tts get_word_bert with
 * nopunc): [CLS], the first piece of each word except punctuation, [SEP]. */
static float *
bert_word_rows (VoskModel *m, const gchar *text, guint *n_rows, GString *trace, GError **error)
{
  GPtrArray *tokens = g_ptr_array_new_with_free_func (g_free);
  GArray *ids = bert_tokenize (m, text, tokens);
  gsize n = MIN (ids->len, BERT_MAX_TOKENS);
  gint64 *input = g_new (gint64, n), *mask = g_new (gint64, n), *types = g_new0 (gint64, n);
  int64_t shape[2] = { 1, (int64_t) n };
  OrtValue *in[3] = { NULL, NULL, NULL }, *out = NULL;
  const char *in_names[3] = { "input_ids", "attention_mask", "token_type_ids" };
  const char *out_names[1] = { "logits" };
  float *rows = NULL;

  if (n == BERT_MAX_TOKENS)   /* keep [SEP] last */
    g_array_index (ids, int, n - 1) = m->sep_id;
  for (gsize i = 0; i < n; i++)
    {
      input[i] = g_array_index (ids, int, i);
      mask[i] = 1;
    }
  if (trace != NULL)
    {
      g_string_append (trace, "tokens:");
      for (gsize i = 0; i < n; i++)
        g_string_append_printf (trace, " %s/%d", (gchar *) g_ptr_array_index (tokens, i),
                                g_array_index (ids, int, i));
      g_string_append_c (trace, '\n');
    }

  if (ort_ok (ort->CreateTensorWithDataAsOrtValue (ort_memory, input, n * sizeof (gint64), shape, 2,
                                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[0]), error)
      && ort_ok (ort->CreateTensorWithDataAsOrtValue (ort_memory, mask, n * sizeof (gint64), shape, 2,
                                                      ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[1]), error)
      && ort_ok (ort->CreateTensorWithDataAsOrtValue (ort_memory, types, n * sizeof (gint64), shape, 2,
                                                      ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[2]), error)
      && ort_ok (ort->Run (m->bert, NULL, in_names, (const OrtValue * const *) in, 3,
                           out_names, 1, &out), error))
    {
      float *data;
      OrtTensorTypeAndShapeInfo *info;
      size_t dims_n, total;
      int64_t dims[4] = { 0 };

      ort->GetTensorMutableData (out, (void **) &data);
      ort->GetTensorTypeAndShape (out, &info);
      ort->GetDimensionsCount (info, &dims_n);
      ort->GetDimensions (info, dims, MIN (dims_n, 4));
      ort->GetTensorShapeElementCount (info, &total);
      ort->ReleaseTensorTypeAndShapeInfo (info);
      if (total != n * BERT_DIM)
        g_set_error (error, VOSK_ERROR, 1, "BERT: unexpected output size %zu", total);
      else
        {
          GArray *sel = g_array_new (FALSE, FALSE, sizeof (gsize));
          for (gsize i = 0; i < n; i++)
            {
              const gchar *t = g_ptr_array_index (tokens, i);
              /* re.match("[-,.?!;:\"]", t) */
              if (t[0] != '#' && strchr ("-,.?!;:\"", t[0]) == NULL)
                g_array_append_val (sel, i);
              else if (t[0] == '\0')
                g_array_append_val (sel, i);
            }
          rows = g_new (float, sel->len * BERT_DIM);
          for (guint r = 0; r < sel->len; r++)
            memcpy (rows + r * BERT_DIM, data + g_array_index (sel, gsize, r) * BERT_DIM,
                    BERT_DIM * sizeof (float));
          *n_rows = sel->len;
          if (trace != NULL)
            {
              double sum = 0;
              for (gsize i = 0; i < (gsize) sel->len * BERT_DIM; i++)
                sum += fabsf (rows[i]);
              g_string_append_printf (trace, "bert_rows: %u sum %.3f\n", sel->len, sum);
            }
          g_array_free (sel, TRUE);
        }
    }
  for (int i = 0; i < 3; i++)
    if (in[i] != NULL)
      ort->ReleaseValue (in[i]);
  if (out != NULL)
    ort->ReleaseValue (out);
  g_free (input);
  g_free (mask);
  g_free (types);
  g_array_free (ids, TRUE);
  g_ptr_array_free (tokens, TRUE);
  return rows;
}



typedef struct
{
  gchar *phone;
  GPtrArray *punc;   /* gchar*, may be NULL */
  int in_quote;
  int bert_index;
} MsPhone;

static void
ms_phone_free (gpointer p)
{
  MsPhone *x = p;
  g_free (x->phone);
  if (x->punc != NULL)
    g_ptr_array_unref (x->punc);
  g_free (x);
}

static void
ms_add (GPtrArray *a, const gchar *phone, GPtrArray *punc, int in_quote, int bert_index)
{
  MsPhone *x = g_new0 (MsPhone, 1);
  x->phone = g_strdup (phone);
  x->punc = punc != NULL ? g_ptr_array_ref (punc) : NULL;
  x->in_quote = in_quote;
  x->bert_index = bert_index;
  g_ptr_array_add (a, x);
}

static gboolean
ms_has (MsPhone *x, const gchar *s)
{
  for (guint i = 0; x->punc != NULL && i < x->punc->len; i++)
    if (strcmp (g_ptr_array_index (x->punc, i), s) == 0)
      return TRUE;
  return FALSE;
}



/* vosk_tts g2p_multistream (word_pos False): the 5 streams of each phoneme
 * (phoneme, its punctuation, in quotes, the last punctuation, the last
 * sentence punctuation) and the BERT row of its word. */
static GArray *
g2p_multistream (VoskModel *m, const gchar *text, GArray *bert_index, GString *trace)
{
  GPtrArray *ph = g_ptr_array_new_with_free_func (ms_phone_free);
  GPtrArray *cur_punc = g_ptr_array_new_with_free_func (g_free);
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64) * 5);
  gchar *lower = g_utf8_strdown (text, -1);
  GString *t = g_string_new (NULL);
  int in_quote = 0, word_index = 1;
  const gchar *last_punc = " ", *last_sentence_punc = " ";

  /* text.replace(" -", "- ") */
  for (const gchar *p = lower; *p != '\0'; p++)
    {
      if (p[0] == ' ' && p[1] == '-')
        {
          g_string_append (t, "- ");
          p++;
        }
      else
        g_string_append_c (t, *p);
    }

  ms_add (ph, "^", NULL, 0, 0);
#define FLUSH_PUNC() G_STMT_START { g_ptr_array_unref (cur_punc); \
    cur_punc = g_ptr_array_new_with_free_func (g_free); } G_STMT_END

  /* re.split("(\.\.\.|- |[ ,.?!;:\"()])") */
  for (const gchar *p = t->str; *p != '\0';)
    {
      const gchar *sep = NULL;
      gsize sep_len = 0;
      const gchar *q;

      if (strncmp (p, "...", 3) == 0)
        sep = "...", sep_len = 3;
      else if (p[0] == '-' && p[1] == ' ')
        sep = "- ", sep_len = 2;
      else if (strchr (" ,.?!;:\"()", *p) != NULL)
        sep_len = 1;

      if (sep_len > 0)
        {
          gchar *s = sep != NULL ? g_strdup (sep) : g_strndup (p, 1);
          p += sep_len;
          if (strcmp (s, "\"") == 0)
            in_quote = !in_quote, g_free (s);
          else if (strcmp (s, "- ") == 0)
            g_ptr_array_add (cur_punc, g_strdup ("-")), g_free (s);
          else if (strcmp (s, " ") != 0)
            g_ptr_array_add (cur_punc, s);
          else
            {
              ms_add (ph, " ", cur_punc, in_quote, word_index);
              FLUSH_PUNC ();
              g_free (s);
            }
          continue;
        }

      /* a word: up to the next separator */
      for (q = p; *q != '\0'; q++)
        if (strncmp (q, "...", 3) == 0 || (q[0] == '-' && q[1] == ' ') || strchr (" ,.?!;:\"()", *q) != NULL)
          break;
      {
        gchar *word = g_strndup (p, q - p);
        p = q;
        if (strcmp (word, "-") == 0)
          g_ptr_array_add (cur_punc, g_strdup ("-"));
        else
          {
            gchar *phonemes = dict_lookup (m, word);
            GPtrArray *list = g_ptr_array_new_with_free_func (g_free);

            if (phonemes != NULL)
              {
                gchar **parts = g_strsplit_set (phonemes, " \t", -1);
                for (gchar **x = parts; *x != NULL; x++)
                  if (**x != '\0')
                    g_ptr_array_add (list, g_strdup (*x));
                g_strfreev (parts);
              }
            else
              g2p_convert (word, list);
            if (trace != NULL)
              {
                g_string_append_printf (trace, "word: %s%s:", word, phonemes != NULL ? "" : " (g2p)");
                for (guint i = 0; i < list->len; i++)
                  g_string_append_printf (trace, " %s", (gchar *) g_ptr_array_index (list, i));
                g_string_append_c (trace, '\n');
              }
            for (guint i = 0; i < list->len; i++)
              ms_add (ph, g_ptr_array_index (list, i), NULL, in_quote, word_index);
            g_ptr_array_free (list, TRUE);
            g_free (phonemes);
            FLUSH_PUNC ();
            word_index++;
          }
        g_free (word);
      }
    }
  ms_add (ph, " ", cur_punc, in_quote, word_index);
  ms_add (ph, "$", NULL, 0, word_index);
#undef FLUSH_PUNC

  /* backwards: the punctuation that follows each phoneme */
  g_array_set_size (ids, ph->len);
  g_array_set_size (bert_index, ph->len);
  for (guint k = ph->len; k-- > 0;)
    {
      MsPhone *x = g_ptr_array_index (ph, k);
      gint64 *row = &g_array_index (ids, gint64, k * 5);
      const gchar *cur = "_";

      if (ms_has (x, "..."))
        last_sentence_punc = "...";
      else if (ms_has (x, "."))
        last_sentence_punc = ".";
      else if (ms_has (x, "!"))
        last_sentence_punc = "!";
      else if (ms_has (x, "?"))
        last_sentence_punc = "?";
      else if (ms_has (x, "-"))
        last_sentence_punc = "-";
      if (x->punc != NULL && x->punc->len > 0)
        last_punc = cur = g_ptr_array_index (x->punc, 0);

      row[0] = phoneme_id (m, x->phone);
      row[1] = phoneme_id (m, cur);
      row[2] = x->in_quote;
      row[3] = phoneme_id (m, last_punc);
      row[4] = phoneme_id (m, last_sentence_punc);
      g_array_index (bert_index, int, k) = x->bert_index;
    }

  /* phonemes the model does not know (vosk_tts would fail): drop them */
  for (guint k = ids->len; k-- > 0;)
    {
      gint64 *row = &g_array_index (ids, gint64, k * 5);
      if (row[0] < 0 || row[1] < 0 || row[3] < 0 || row[4] < 0)
        {
          if (trace != NULL)
            g_string_append_printf (trace, "dropped: %s\n", ((MsPhone *) g_ptr_array_index (ph, k))->phone);
          g_array_remove_index (ids, k);
          g_array_remove_index (bert_index, k);
        }
    }
  if (trace != NULL)
    {
      g_string_append (trace, "phoneme_ids:");
      for (guint k = 0; k < ids->len; k++)
        {
          gint64 *row = &g_array_index (ids, gint64, k * 5);
          g_string_append_printf (trace, " [%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT
                                  ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "]",
                                  row[0], row[1], row[2], row[3], row[4]);
        }
      g_string_append_c (trace, '\n');
    }

  g_ptr_array_unref (cur_punc);
  g_ptr_array_free (ph, TRUE);
  g_string_free (t, TRUE);
  g_free (lower);
  return ids;
}



/* the text as vosk_tts expects it: quotes and dashes it knows, one space */
static gchar *
normalize_text (const gchar *text)
{
  GString *s = g_string_new (NULL);
  gboolean space = FALSE;

  for (const gchar *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (g_unichar_isspace (c) || c == 0x00A0)
        {
          space = s->len > 0;
          continue;
        }
      if (space)
        g_string_append_c (s, ' ');
      space = FALSE;
      switch (c)
        {
        case 0x00AB: case 0x00BB: case 0x201C: case 0x201D: case 0x201E: case 0x201F:
          g_string_append_c (s, '"');
          break;
        case 0x2013: case 0x2014: case 0x2012: case 0x2212:
          g_string_append_c (s, '-');
          break;
        case 0x2026:
          g_string_append (s, "...");
          break;
        case 0x2018: case 0x2019:
          g_string_append_c (s, '\'');
          break;
        default:
          g_string_append_unichar (s, c);
        }
    }
  return g_string_free (s, FALSE);
}



float *
vosk_synth (VoskModel *m, const gchar *text, int sid, float speech_rate,
            gsize *n_samples, GString *trace, GError **error)
{
  gchar *norm = normalize_text (text);
  GArray *bert_index = g_array_new (FALSE, FALSE, sizeof (int));
  GArray *ids = NULL;
  float *rows = NULL, *bert = NULL, *result = NULL;
  guint n_rows = 0;
  gint64 *input = NULL, lengths[1], sids[1] = { sid };
  float scales[3];
  gsize T;
  OrtValue *in[5] = { NULL }, *out = NULL;
  const char *in_names[5] = { "input", "input_lengths", "scales", "sid", "bert" };
  const char *out_names[1] = { "wav" };

  *n_samples = 0;
  if (m->acoustic == NULL)
    {
      g_set_error (error, VOSK_ERROR, 1, "the model is not loaded");
      goto out;
    }
  if (*norm == '\0')
    goto out;

  if (m->bert != NULL)
    {
      /* text.replace("+", "").replace("_", "") */
      GString *b = g_string_new (NULL);
      for (const gchar *p = norm; *p != '\0'; p++)
        if (*p != '+' && *p != '_')
          g_string_append_c (b, *p);
      rows = bert_word_rows (m, b->str, &n_rows, trace, error);
      g_string_free (b, TRUE);
      if (rows == NULL)
        goto out;
    }

  ids = g2p_multistream (m, norm, bert_index, trace);
  T = ids->len;
  if (T == 0)
    goto out;

  /* input [1, 5, T]: the transposed rows */
  input = g_new (gint64, 5 * T);
  for (gsize k = 0; k < T; k++)
    for (int s = 0; s < 5; s++)
      input[s * T + k] = g_array_index (ids, gint64, k * 5 + s);
  /* bert [1, 768, T] */
  bert = g_new0 (float, BERT_DIM * T);
  if (rows != NULL)
    for (gsize k = 0; k < T; k++)
      {
        guint r = MIN ((guint) g_array_index (bert_index, int, k), n_rows - 1);
        for (int d = 0; d < BERT_DIM; d++)
          bert[d * T + k] = rows[r * BERT_DIM + d];
      }
  lengths[0] = T;
  scales[0] = m->noise_level;
  scales[1] = 1.0f / (speech_rate > 0 ? speech_rate : 1.0f);
  scales[2] = m->duration_noise_level;

  {
    int64_t s_input[3] = { 1, 5, (int64_t) T }, s_one[1] = { 1 }, s_scales[1] = { 3 };
    int64_t s_bert[3] = { 1, BERT_DIM, (int64_t) T };

    if (!ort_ok (ort->CreateTensorWithDataAsOrtValue (ort_memory, input, 5 * T * sizeof (gint64), s_input, 3,
                                                      ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[0]), error)
        || !ort_ok (ort->CreateTensorWithDataAsOrtValue (ort_memory, lengths, sizeof lengths, s_one, 1,
                                                         ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[1]), error)
        || !ort_ok (ort->CreateTensorWithDataAsOrtValue (ort_memory, scales, sizeof scales, s_scales, 1,
                                                         ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[2]), error)
        || !ort_ok (ort->CreateTensorWithDataAsOrtValue (ort_memory, sids, sizeof sids, s_one, 1,
                                                         ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[3]), error)
        || !ort_ok (ort->CreateTensorWithDataAsOrtValue (ort_memory, bert, BERT_DIM * T * sizeof (float), s_bert, 3,
                                                         ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[4]), error)
        || !ort_ok (ort->Run (m->acoustic, NULL, in_names, (const OrtValue * const *) in, 5,
                              out_names, 1, &out), error))
      goto out;
  }
  {
    float *data;
    OrtTensorTypeAndShapeInfo *info;
    size_t total;

    ort->GetTensorMutableData (out, (void **) &data);
    ort->GetTensorTypeAndShape (out, &info);
    ort->GetTensorShapeElementCount (info, &total);
    ort->ReleaseTensorTypeAndShapeInfo (info);
    result = g_new (float, total);
    for (size_t i = 0; i < total; i++)
      result[i] = data[i] * m->scale;
    *n_samples = total;
  }

out:
  for (int i = 0; i < 5; i++)
    if (in[i] != NULL)
      ort->ReleaseValue (in[i]);
  if (out != NULL)
    ort->ReleaseValue (out);
  g_free (input);
  g_free (bert);
  g_free (rows);
  if (ids != NULL)
    g_array_free (ids, TRUE);
  g_array_free (bert_index, TRUE);
  g_free (norm);
  return result;
}

/*
 * ru-g2p - Russian text to IPA for the Russian Kokoro, see ru-g2p.h
 *
 * Reference: ru_g2p.py of zaakirio/kokoro-ru (Apache-2.0), misaki's
 * EspeakG2P and phonemizer 3.6 (punctuation preserved, ties), whose
 * output this reproduces character for character.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include "ru-g2p.h"

#include <espeak-ng/speak_lib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACUTE 0x0301

struct RuG2p
{
  const char *dict;     /* mmap of the sorted dictionary */
  gsize dict_size;
  GRegex *marks_re;     /* phonemizer's punctuation */
};

static gboolean espeak_ready;



/* ------------------------------------------------------------- helpers */

static gboolean
is_ru_letter (gunichar c)
{
  return (c >= 0x0430 && c <= 0x044F) || c == 0x0451
         || (c >= 0x0410 && c <= 0x042F) || c == 0x0401;
}

static gboolean
is_ru_vowel (gunichar c)
{
  return c != 0 && g_utf8_strchr ("аеёиоуыэюя", -1, c) != NULL;
}

/* replaces every occurrence of old in s (in place: frees s) */
static gchar *
replace_all (gchar *s, const gchar *old, const gchar *new)
{
  gchar **parts;
  gchar *r;

  if (strstr (s, old) == NULL)
    return s;
  parts = g_strsplit (s, old, -1);
  r = g_strjoinv (new, parts);
  g_strfreev (parts);
  g_free (s);
  return r;
}

static gboolean
in_list (const gchar *s, const char *const *list)
{
  for (; *list != NULL; list++)
    if (strcmp (s, *list) == 0)
      return TRUE;
  return FALSE;
}

/* UCS-4 string builder */
typedef GArray UStr;

static UStr *
ustr_new (void)
{
  return g_array_new (TRUE, TRUE, sizeof (gunichar));
}

static void
ustr_add (UStr *u, gunichar c)
{
  g_array_append_val (u, c);
}

static gchar *
ustr_free_utf8 (UStr *u)
{
  gchar *s = g_ucs4_to_utf8 ((gunichar *) u->data, u->len, NULL, NULL, NULL);
  g_array_free (u, TRUE);
  return s;
}

static gchar *
ucs4_to_utf8 (const gunichar *w, glong n)
{
  return g_ucs4_to_utf8 (w, n, NULL, NULL, NULL);
}

/* the word without the stress marks */
static gchar *
strip_acute (const gunichar *w, glong n)
{
  UStr *u = ustr_new ();
  for (glong i = 0; i < n; i++)
    if (w[i] != ACUTE)
      ustr_add (u, w[i]);
  return ustr_free_utf8 (u);
}

/* bare (utf-8) with ACUTE after its vowel_n-th vowel */
static UStr *
seat_acute (const gchar *bare, int vowel_n)
{
  UStr *u = ustr_new ();
  int n = 0;
  for (const gchar *p = bare; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      ustr_add (u, c);
      if (is_ru_vowel (c) && ++n == vowel_n)
        ustr_add (u, ACUTE);
    }
  return u;
}



/* ---------------------------------------------------------- dictionary */

static gchar *
dict_lookup (RuG2p *g, const gchar *word)
{
  gsize lo = 0, hi = g->dict_size, wl = strlen (word);

  if (g->dict == NULL || wl == 0)
    return NULL;
  while (lo < hi)
    {
      gsize mid = lo + (hi - lo) / 2, start = mid, tab;
      int c;

      while (start > lo && g->dict[start - 1] != '\n')
        start--;
      tab = start;
      while (tab < g->dict_size && g->dict[tab] != '\t' && g->dict[tab] != '\n')
        tab++;
      c = memcmp (g->dict + start, word, MIN (tab - start, wl));
      if (c == 0 && tab - start != wl)
        c = tab - start < wl ? -1 : 1;
      if (c == 0)
        {
          const char *p = g->dict + tab + 1;
          const char *nl = memchr (p, '\n', g->dict_size - (p - g->dict));
          return g_strndup (p, (nl != NULL ? nl : g->dict + g->dict_size) - p);
        }
      if (c < 0)
        {
          const char *nl = memchr (g->dict + tab, '\n', g->dict_size - tab);
          lo = nl != NULL ? (gsize) (nl - g->dict) + 1 : g->dict_size;
        }
      else
        hi = start;
    }
  return NULL;
}



/* ------------------------------------------------------------ espeak */

RuG2p *
ru_g2p_new (const gchar *dict, const gchar *espeak_path, GError **error)
{
  RuG2p *g;

  if (!espeak_ready)
    {
      if (espeak_Initialize (AUDIO_OUTPUT_SYNCHRONOUS, 0, espeak_path,
                             espeakINITIALIZE_DONT_EXIT) < 0
          || espeak_SetVoiceByName ("ru") != EE_OK)
        {
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                       "espeak-ng: cannot load the Russian voice from %s", espeak_path);
          return NULL;
        }
      espeak_ready = TRUE;
    }
  g = g_new0 (RuG2p, 1);
  /* phonemizer: (\s*(?:[others]|(?<![0-9])[,.]|[,.](?![0-9]))+\s*)+ */
  g->marks_re = g_regex_new ("(\\s*(?:[;:!?¡¿—…\"«»“”(){}\\[\\]]"
                             "|(?<![0-9])[,.]|[,.](?![0-9]))+\\s*)+",
                             G_REGEX_OPTIMIZE, 0, NULL);
  if (dict != NULL)
    {
      int fd = open (dict, O_RDONLY | O_CLOEXEC);
      struct stat st;
      if (fd < 0 || fstat (fd, &st) != 0)
        {
          g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                       "%s: %s", dict, g_strerror (errno));
          if (fd >= 0)
            close (fd);
          ru_g2p_free (g);
          return NULL;
        }
      if (st.st_size > 0)
        {
          void *p = mmap (NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
          if (p != MAP_FAILED)
            {
              g->dict = p;
              g->dict_size = st.st_size;
            }
        }
      close (fd);
    }
  return g;
}

void
ru_g2p_free (RuG2p *g)
{
  if (g == NULL)
    return;
  if (g->dict != NULL)
    munmap ((void *) g->dict, g->dict_size);
  if (g->marks_re != NULL)
    g_regex_unref (g->marks_re);
  g_free (g);
}

/* phonemizer EspeakBackend._phonemize_aux + _postprocess_line for one chunk,
 * tie '^', with stress, remove-flags, strip=False (a trailing space) */
static gchar *
espeak_chunk (const gchar *text)
{
  const void *ptr = text;
  GString *res = g_string_new (NULL);
  gchar *line, **words;
  GString *out;
  GRegex *re;

  while (ptr != NULL)
    {
      const char *ph = espeak_TextToPhonemes (&ptr, espeakCHARS_UTF8,
                                              espeakPHONEMES_IPA | espeakPHONEMES_TIE
                                              | (0x0361 << 8));
      if (ph != NULL && *ph != '\0')
        {
          if (res->len > 0)
            g_string_append_c (res, ' ');
          g_string_append (res, ph);
        }
    }
  line = g_strstrip (g_string_free (res, FALSE));
  line = replace_all (line, "\n", " ");
  /* Python str.replace('  ', ' '): one pass, non-overlapping */
  line = replace_all (line, "  ", " ");
  if (strstr (line, "__") != NULL)
    {
      re = g_regex_new ("_+", 0, 0, NULL);
      gchar *t = g_regex_replace_literal (re, line, -1, 0, "_", 0, NULL);
      g_regex_unref (re);
      g_free (line);
      line = t;
    }
  line = replace_all (line, "_ ", " ");
  /* RemoveFlags: (en) ... */
  re = g_regex_new ("\\(.+?\\)", 0, 0, NULL);
  {
    gchar *t = g_regex_replace_literal (re, line, -1, 0, "", 0, NULL);
    g_free (line);
    line = t;
  }
  g_regex_unref (re);
  if (*line == '\0')
    return line;
  words = g_strsplit (line, " ", -1);
  out = g_string_new (NULL);
  for (int i = 0; words[i] != NULL; i++)
    {
      gchar *w = replace_all (g_strdup (g_strstrip (words[i])), "͡", "^");
      if (i > 0)
        g_string_append_c (out, ' ');
      g_string_append (out, w);
      g_free (w);
    }
  g_string_append_c (out, ' ');
  g_strfreev (words);
  g_free (line);
  return g_string_free (out, FALSE);
}

static gboolean
is_inline_mark (const gchar *mark)
{
  gboolean any = FALSE;
  for (const gchar *p = mark; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (g_unichar_isspace (c))
        continue;
      if (g_utf8_strchr ("\"«»“”‘’„‹›()[]{}", -1, c) == NULL)
        return FALSE;
      any = TRUE;
    }
  return any;
}

static int
count_words (const gchar *s)
{
  int n = 0;
  gchar **w = g_strsplit (s, " ", -1);
  for (int i = 0; w[i] != NULL; i++)
    if (*w[i] != '\0')
      n++;
  g_strfreev (w);
  return n;
}

/* Part of a line: a chunk (index into chunks) or a mark */
typedef struct
{
  int chunk;            /* -1 for a mark */
  gchar *mark;
} Part;

/* phonemizer phonemize([text], preserve_punctuation=True, strip=False) */
static gchar *
phonemize_punct (RuG2p *g, const gchar *text)
{
  GMatchInfo *mi;
  GPtrArray *chunks = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *ph;
  GArray *parts = g_array_new (FALSE, FALSE, sizeof (Part));
  gchar **in_context;
  gint cursor = 0;
  GString *out = g_string_new (NULL);
  const gchar *mark = NULL;

  /* preserve() */
  g_regex_match (g->marks_re, text, 0, &mi);
  while (g_match_info_matches (mi))
    {
      gint s, e;
      Part p;
      g_match_info_fetch_pos (mi, 0, &s, &e);
      if (s > cursor)
        {
          p.chunk = chunks->len;
          p.mark = NULL;
          g_array_append_val (parts, p);
          g_ptr_array_add (chunks, g_strndup (text + cursor, s - cursor));
        }
      p.chunk = -1;
      p.mark = g_strndup (text + s, e - s);
      g_array_append_val (parts, p);
      cursor = e;
      g_match_info_next (mi, NULL);
    }
  g_match_info_free (mi);
  if (text[cursor] != '\0')
    {
      Part p = { chunks->len, NULL };
      g_array_append_val (parts, p);
      g_ptr_array_add (chunks, g_strdup (text + cursor));
    }

  ph = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < chunks->len; i++)
    g_ptr_array_add (ph, espeak_chunk (chunks->pdata[i]));
  in_context = g_new0 (gchar *, chunks->len + 1);

  /* inline_groups(): chunks separated by inline marks only, phonemized as a
   * whole too; restore() uses them when the word counts agree */
  for (guint i = 0; i < parts->len;)
    {
      GArray *group = g_array_new (FALSE, FALSE, sizeof (int));
      guint j = i;
      while (j < parts->len && g_array_index (parts, Part, j).chunk < 0)
        j++;
      if (j >= parts->len)
        {
          g_array_free (group, TRUE);
          break;
        }
      g_array_append_val (group, g_array_index (parts, Part, j).chunk);
      while (j + 2 < parts->len
             && g_array_index (parts, Part, j + 2).chunk >= 0
             && is_inline_mark (g_array_index (parts, Part, j + 1).mark))
        {
          j += 2;
          g_array_append_val (group, g_array_index (parts, Part, j).chunk);
        }
      i = j + 1;
      if (group->len > 1)
        {
          GString *joined = g_string_new (NULL);
          gchar *whole, **pieces;
          int total = 0, *counts = g_new (int, group->len);
          gboolean ok = TRUE;

          for (guint k = 0; k < group->len; k++)
            {
              int c = g_array_index (group, int, k);
              if (k > 0)
                g_string_append_c (joined, ' ');
              g_string_append (joined, chunks->pdata[c]);
              counts[k] = count_words (ph->pdata[c]);
              ok = ok && counts[k] > 0;
              total += counts[k];
            }
          whole = espeak_chunk (joined->str);
          if (ok && count_words (whole) == total)
            {
              /* cut before the first word of each chunk but the first */
              GString *piece = g_string_new (NULL);
              int nwords = 0, k = 0, start = counts[0];
              gboolean first = TRUE;
              pieces = g_strsplit (whole, " ", -1);
              for (int n = 0; pieces[n] != NULL; n++)
                {
                  if (*pieces[n] != '\0')
                    {
                      if (nwords == start && k + 1 < (int) group->len)
                        {
                          in_context[g_array_index (group, int, k)] = g_string_free (piece, FALSE);
                          piece = g_string_new (NULL);
                          first = TRUE;
                          k++;
                          start += counts[k];
                        }
                      nwords++;
                    }
                  if (!first)
                    g_string_append_c (piece, ' ');
                  g_string_append (piece, pieces[n]);
                  first = FALSE;
                }
              in_context[g_array_index (group, int, k)] = g_string_free (piece, FALSE);
              g_strfreev (pieces);
            }
          g_free (whole);
          g_free (counts);
          g_string_free (joined, TRUE);
        }
      g_array_free (group, TRUE);
    }

  /* restore() */
  for (guint i = 0; i < parts->len; i++)
    {
      Part *p = &g_array_index (parts, Part, i);
      if (p->chunk < 0)
        {
          if (out->len > 0 && out->str[out->len - 1] == ' ')
            g_string_truncate (out, out->len - 1);
          g_string_append (out, p->mark);
          mark = p->mark;
        }
      else
        {
          const gchar *c = in_context[p->chunk] != NULL ? in_context[p->chunk]
                                                         : ph->pdata[p->chunk];
          if (*c != '\0')
            {
              g_string_append (out, c);
              mark = NULL;
            }
        }
    }
  if (mark != NULL && !g_str_has_suffix (mark, " "))
    g_string_append_c (out, ' ');

  for (guint i = 0; i < parts->len; i++)
    g_free (g_array_index (parts, Part, i).mark);
  g_array_free (parts, TRUE);
  g_strfreev (in_context);
  g_ptr_array_free (ph, TRUE);
  g_ptr_array_free (chunks, TRUE);
  return g_string_free (out, FALSE);
}



/* ---------------------------------------------------------- respelling */

static const char *const ogo_blacklist[] = {
  "много", "немного", "намного", "ненамного", "строго", "нестрого",
  "настрого", "дорого", "недорого", "полого", "убого", "лего", "диего",
  "ого", "огого", NULL
};

static const char *const respell_substr[][2] = {
  { "солнц", "сонц" },
  { "чувств", "чуств" },
  { "здравств", "здраств" },
  { "счастлив", "счаслив" },
  { "завистлив", "завислив" },
  { "участлив", "учаслив" },
  { "совестлив", "совеслив" },
};

static const char *const respell_word_tab[][2] = {
  { "дорого", "дорога" },
  { "недорого", "недорога" },
  { "настрого", "настрога" },
  { "конечно", "конешно" },
  { "скучно", "скушно" },
  { "скучный", "скушный" },
  { "нарочно", "нарошно" },
  { "яичница", "яишница" },
  { "скворечник", "скворешник" },
  { "девичник", "девишник" },
  { "двоечник", "двоешник" },
  { "троечник", "троешник" },
  { "прачечная", "прашечная" },
  { "горчичник", "горчишник" },
  { "пустячный", "пустяшный" },
};

static glong
find_acute (const gunichar *w, glong n)
{
  for (glong i = 0; i < n; i++)
    if (w[i] == ACUTE)
      return i;
  return -1;
}

static int
vowels_before (const gunichar *w, glong idx)
{
  int n = 0;
  for (glong i = 0; i < idx; i++)
    if (is_ru_vowel (w[i]))
      n++;
  return n;
}

/* _respell_word(); appends the result to out */
static void
respell_word (const gunichar *w0, glong n, UStr *out)
{
  gunichar *w = g_memdup2 (w0, n * sizeof (gunichar));
  gchar *bare = strip_acute (w, n);
  gboolean any = FALSE;

  for (guint i = 0; i < G_N_ELEMENTS (respell_word_tab); i++)
    if (strcmp (bare, respell_word_tab[i][0]) == 0)
      {
        glong idx = find_acute (w, n);
        UStr *u;
        if (idx > 0)
          u = seat_acute (respell_word_tab[i][1], vowels_before (w, idx));
        else
          u = seat_acute (respell_word_tab[i][1], 0);
        g_array_append_vals (out, u->data, u->len);
        g_array_free (u, TRUE);
        goto done;
      }
  if (!in_list (bare, ogo_blacklist))
    {
      /* ([а-яё́]*?[ое]́?)го(́?)$ -> \1во\2 */
      glong e = n;
      if (e > 0 && w[e - 1] == ACUTE)
        e--;
      if (e >= 3 && w[e - 1] == 0x043E && w[e - 2] == 0x0433)
        {
          glong b = e - 3;
          if (b >= 0 && w[b] == ACUTE)
            b--;
          if (b >= 0 && (w[b] == 0x043E || w[b] == 0x0435))
            {
              w[e - 2] = 0x0432;
              g_free (bare);
              bare = strip_acute (w, n);
            }
        }
    }
  for (guint i = 0; i < G_N_ELEMENTS (respell_substr); i++)
    any = any || strstr (bare, respell_substr[i][0]) != NULL;
  if (any)
    {
      glong idx = find_acute (w, n);
      int vowel_n = idx > 0 ? vowels_before (w, idx) : 0;
      UStr *u;
      for (guint i = 0; i < G_N_ELEMENTS (respell_substr); i++)
        bare = replace_all (bare, respell_substr[i][0], respell_substr[i][1]);
      u = seat_acute (bare, vowel_n);
      g_array_append_vals (out, u->data, u->len);
      g_array_free (u, TRUE);
      goto done;
    }
  g_array_append_vals (out, w, n);
done:
  g_free (bare);
  g_free (w);
}

/* respell(): over the runs of [а-яё́] (the text is lower case) */
static gchar *
respell (const gchar *text)
{
  glong n;
  gunichar *t = g_utf8_to_ucs4_fast (text, -1, &n);
  UStr *out = ustr_new ();

  for (glong i = 0; i < n;)
    {
      if (is_ru_letter (t[i]) || t[i] == ACUTE)
        {
          glong j = i;
          while (j < n && (is_ru_letter (t[j]) || t[j] == ACUTE))
            j++;
          respell_word (t + i, j - i, out);
          i = j;
        }
      else
        ustr_add (out, t[i++]);
    }
  g_free (t);
  return ustr_free_utf8 (out);
}



/* ------------------------------------------------------ vowel reduction */

static gboolean
uin (gunichar c, const gchar *set)
{
  return g_utf8_strchr (set, -1, c) != NULL;
}

static void
reduce_token (const gunichar *t, glong n, UStr *out)
{
  gunichar *c = g_memdup2 (t, n * sizeof (gunichar));
  GArray *vowels = g_array_new (FALSE, FALSE, sizeof (glong));
  gboolean *stressed = g_new0 (gboolean, n + 1);
  GArray *sord = g_array_new (FALSE, FALSE, sizeof (int));
  glong primary = -1;

  for (glong i = 0; i < n; i++)
    if (uin (c[i], "aɑoeiuyʌəɪɐɛɨ"))
      g_array_append_val (vowels, i);
  if (vowels->len == 0)
    {
      g_array_append_vals (out, c, n);
      goto done;
    }
  for (glong i = 0; i < n; i++)
    if (c[i] == 0x02C8)
      {
        primary = i;
        break;
      }
  for (glong i = 0; i < n; i++)
    {
      if (c[i] != 0x02C8 && c[i] != 0x02CC)
        continue;
      if (c[i] == 0x02CC && primary != -1 && i > primary)
        continue;
      for (guint k = 0; k < vowels->len; k++)
        if (g_array_index (vowels, glong, k) > i)
          {
            stressed[g_array_index (vowels, glong, k)] = TRUE;
            break;
          }
    }
  for (guint k = 0; k < vowels->len; k++)
    if (stressed[g_array_index (vowels, glong, k)])
      {
        int o = k;
        g_array_append_val (sord, o);
      }
  for (guint ord = 0; ord < vowels->len; ord++)
    {
      glong i = g_array_index (vowels, glong, ord);
      int next = -1;
      if (stressed[i] || !uin (c[i], "aɑoʌə"))
        continue;
      if (i > 0 && (c[i - 1] == 0x02B2 || c[i - 1] == 'j'))
        {
          c[i] = 0x026A;  /* ɪ */
          continue;
        }
      for (guint s = 0; s < sord->len; s++)
        if (g_array_index (sord, int, s) > (int) ord)
          {
            next = g_array_index (sord, int, s);
            break;
          }
      c[i] = next == (int) ord + 1 || i == 0 ? 0x0250 /* ɐ */ : 0x0259 /* ə */;
    }
  for (glong k = 0; k < n; k++)
    if (!(primary != -1 && c[k] == 0x02CC && k > primary))
      ustr_add (out, c[k]);
done:
  g_array_free (sord, TRUE);
  g_free (stressed);
  g_array_free (vowels, TRUE);
  g_free (c);
}

/* reduce_vowels (split on ' ') then lengthen_sch (ɕ not before ː) */
static gchar *
reduce_vowels (const gchar *ps)
{
  glong n;
  gunichar *t = g_utf8_to_ucs4_fast (ps, -1, &n);
  UStr *r = ustr_new (), *out = ustr_new ();
  glong start = 0;

  for (glong i = 0; i <= n; i++)
    if (i == n || t[i] == ' ')
      {
        reduce_token (t + start, i - start, r);
        if (i < n)
          ustr_add (r, ' ');
        start = i + 1;
      }
  for (guint i = 0; i < r->len; i++)
    {
      gunichar c = g_array_index (r, gunichar, i);
      ustr_add (out, c);
      if (c == 0x0255 && (i + 1 >= r->len || g_array_index (r, gunichar, i + 1) != 0x02D0))
        ustr_add (out, 0x02D0);
    }
  g_array_free (r, TRUE);
  g_free (t);
  return ustr_free_utf8 (out);
}



/* ----------------------------------------------------------- pipeline */

gchar *
ru_g2p_accentuate (RuG2p *g, const gchar *text)
{
  gchar *lower = g_utf8_strdown (text, -1);
  glong n;
  gunichar *t = g_utf8_to_ucs4_fast (lower, -1, &n);
  UStr *out = ustr_new ();

  for (glong i = 0; i < n;)
    {
      glong j = i;
      gboolean marked = FALSE;
      if (!is_ru_letter (t[i]))
        {
          ustr_add (out, t[i++]);
          continue;
        }
      while (j < n && (is_ru_letter (t[j]) || t[j] == ACUTE))
        {
          marked = marked || t[j] == ACUTE;
          j++;
        }
      {
        gchar *word = ucs4_to_utf8 (t + i, j - i);
        gchar *ph = marked ? NULL : dict_lookup (g, word);
        int nv = 0;
        for (glong k = i; k < j; k++)
          nv += is_ru_vowel (t[k]);
        if (ph == NULL && !marked && strstr (word, "ё") != NULL)
          {
            gchar *e = replace_all (g_strdup (word), "ё", "е");
            ph = dict_lookup (g, e);
            g_free (e);
          }
        if (ph != NULL && nv >= 2)
          {
            gchar **tok = g_strsplit (ph, " ", -1);
            GPtrArray *vt = g_ptr_array_new ();
            for (int k = 0; tok[k] != NULL; k++)
              {
                gsize l = strlen (tok[k]);
                if (l >= 2 && (tok[k][l - 1] == '0' || tok[k][l - 1] == '1'))
                  g_ptr_array_add (vt, tok[k]);
              }
            if ((int) vt->len == nv)
              {
                int v = 0;
                for (glong k = i; k < j; k++)
                  {
                    gunichar c = t[k];
                    if (!is_ru_vowel (c))
                      {
                        ustr_add (out, c);
                        continue;
                      }
                    {
                      const gchar *p = vt->pdata[v++];
                      gboolean st = p[strlen (p) - 1] == '1';
                      if (st && c == 0x0435 && p[0] == 'o')
                        ustr_add (out, 0x0451);   /* е -> ё */
                      else
                        {
                          ustr_add (out, c);
                          if (st && c != 0x0451)
                            ustr_add (out, ACUTE);
                        }
                    }
                  }
                g_ptr_array_free (vt, TRUE);
                g_strfreev (tok);
                g_free (ph);
                g_free (word);
                i = j;
                continue;
              }
            g_ptr_array_free (vt, TRUE);
            g_strfreev (tok);
          }
        g_free (ph);
        g_free (word);
        g_array_append_vals (out, t + i, j - i);
        i = j;
      }
    }
  g_free (t);
  g_free (lower);
  return ustr_free_utf8 (out);
}

static gchar *
brackets (const gchar *text)
{
  gchar *s = g_strdup (text);
  for (gchar *p = s; *p != '\0'; p++)
    if (*p == '[' || *p == '{')
      *p = '(';
    else if (*p == ']' || *p == '}')
      *p = ')';
  return s;
}

gchar *
ru_g2p_phonemize_marked (RuG2p *g, const gchar *marked)
{
  static const char *const e2m[][2] = {
    /* misaki EspeakG2P, sorted() by key */
    { "a^ɪ", "I" }, { "a^ʊ", "W" }, { "d^z", "ʣ" }, { "d^ʒ", "ʤ" },
    { "e^ɪ", "A" }, { "o^ʊ", "O" }, { "s^s", "S" }, { "t^s", "ʦ" },
    { "t^ʃ", "ʧ" }, { "ɔ^ɪ", "Y" }, { "ə^ʊ", "Q" },
  };
  static const char *const normalize[][2] = {
    { "u\"", "u" }, { "ɭ", "l" }, { "ɵ", "o" }, { "ʑ", "ʒ" }, { "ʐ", "ʒ" },
    { "ʧʲ", "ʧ" },
  };
  gchar *b = brackets (marked);
  gchar *low = g_utf8_strdown (b, -1);
  gchar *s = respell (low), *ps, *r;

  g_free (b);
  g_free (low);
  /* misaki: angles to curly quotes, parentheses to angles */
  s = replace_all (s, "«", "“");
  s = replace_all (s, "»", "”");
  s = replace_all (s, "(", "«");
  s = replace_all (s, ")", "»");
  ps = phonemize_punct (g, s);
  g_free (s);
  g_strstrip (ps);
  for (guint i = 0; i < G_N_ELEMENTS (e2m); i++)
    ps = replace_all (ps, e2m[i][0], e2m[i][1]);
  ps = replace_all (ps, "^", "");
  ps = replace_all (ps, "-", "");
  ps = replace_all (ps, "«", "(");
  ps = replace_all (ps, "»", ")");
  for (guint i = 0; i < G_N_ELEMENTS (normalize); i++)
    ps = replace_all (ps, normalize[i][0], normalize[i][1]);
  r = reduce_vowels (ps);
  g_free (ps);
  return r;
}

gchar *
ru_g2p_phonemize (RuG2p *g, const gchar *text)
{
  gchar *b = brackets (text);
  gchar *m = ru_g2p_accentuate (g, b);
  gchar *r = ru_g2p_phonemize_marked (g, m);
  g_free (b);
  g_free (m);
  return r;
}

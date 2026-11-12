/*
 * Copyright (C) 2026 Grigory Tuboltsev <gtubolcev@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "speechd-config.h"

/* Speech Dispatcher reads ~/.config/speech-dispatcher/speechd.conf when it
 * exists and the system file otherwise. Includes and module configurations
 * are still looked up in the system directory, so a copy of the system file
 * is a complete user configuration (this is also what spd-conf does). */

struct _SpeechdConfig
{
  gchar     *path;
  GPtrArray *lines;
  gboolean   dirty;
};



/* Returns the key of an option line: "Key value" or, when @commented is
 * set, "# Key value". The key is copied to @key, the rest of the line is
 * returned. NULL when the line is no such option. */
static const gchar *
parse_option_line (const gchar *line,
                   gboolean     commented,
                   gchar      **key)
{
  const gchar *p = line;
  const gchar *start;

  while (g_ascii_isspace (*p))
    p++;

  if (commented)
    {
      if (*p != '#')
        return NULL;
      while (*p == '#' || g_ascii_isspace (*p))
        p++;
    }

  if (!g_ascii_isalpha (*p))
    return NULL;

  start = p;
  while (g_ascii_isalnum (*p))
    p++;

  if (*p != '\0' && !g_ascii_isspace (*p))
    return NULL;

  *key = g_strndup (start, p - start);

  while (g_ascii_isspace (*p))
    p++;

  return p;
}



/* Parses one value: a quoted string or a bare word. Returns NULL when there
 * is none. With @end, the position after the value is stored there. */
static gchar *
parse_value (const gchar  *p,
             const gchar **end)
{
  const gchar *start;
  gchar       *value;

  while (g_ascii_isspace (*p))
    p++;

  if (*p == '"')
    {
      start = ++p;
      while (*p != '\0' && *p != '"')
        p++;
      if (*p != '"')
        return NULL;
      value = g_strndup (start, p - start);
      p++;
    }
  else
    {
      start = p;
      while (*p != '\0' && *p != '#' && !g_ascii_isspace (*p))
        p++;
      if (p == start)
        return NULL;
      value = g_strndup (start, p - start);
    }

  if (end != NULL)
    *end = p;

  return value;
}



/* Client sections (BeginClient … EndClient) set per-client values, they
 * must not be mistaken for the global defaults. */
static gboolean
update_client_section (const gchar *line,
                       gboolean     in_client)
{
  const gchar *rest;
  gchar       *key = NULL;

  rest = parse_option_line (line, FALSE, &key);
  if (rest != NULL)
    {
      if (g_ascii_strcasecmp (key, "BeginClient") == 0)
        in_client = TRUE;
      else if (g_ascii_strcasecmp (key, "EndClient") == 0)
        in_client = FALSE;
    }
  g_free (key);

  return in_client;
}



/* Index of the last active line setting @key (the one that wins), or -1. */
static gint
find_active (SpeechdConfig *config,
             const gchar   *key)
{
  gboolean in_client = FALSE;
  gint     found = -1;
  guint    i;

  for (i = 0; i < config->lines->len; i++)
    {
      const gchar *line = g_ptr_array_index (config->lines, i);
      gchar       *k = NULL;

      in_client = update_client_section (line, in_client);
      if (in_client)
        continue;

      if (parse_option_line (line, FALSE, &k) != NULL
          && g_ascii_strcasecmp (k, key) == 0)
        found = i;
      g_free (k);
    }

  return found;
}



/* Index of the last commented-out example of @key ("# DefaultRate  0"),
 * where the new value goes, or -1. Prose comments are not matched. */
static gint
find_commented (SpeechdConfig *config,
                const gchar   *key)
{
  gint  found = -1;
  guint i;

  for (i = 0; i < config->lines->len; i++)
    {
      const gchar *line = g_ptr_array_index (config->lines, i);
      const gchar *rest, *end;
      gchar       *k = NULL, *value;

      rest = parse_option_line (line, TRUE, &k);
      if (rest != NULL && g_ascii_strcasecmp (k, key) == 0)
        {
          value = parse_value (rest, &end);
          if (value != NULL)
            {
              while (g_ascii_isspace (*end))
                end++;
              if (*end == '\0')
                found = i;
            }
          g_free (value);
        }
      g_free (k);
    }

  return found;
}



static gchar *
format_line (const gchar *key,
             const gchar *value,
             gboolean     quote)
{
  if (quote)
    return g_strdup_printf ("%s \"%s\"", key, value);

  return g_strdup_printf ("%s %s", key, value);
}



static void
set_line (SpeechdConfig *config,
          const gchar   *key,
          const gchar   *value,
          gboolean       quote)
{
  gchar *line;
  gchar *current;
  gint   i;

  current = speechd_config_get (config, key);
  if (g_strcmp0 (current, value) == 0)
    {
      g_free (current);
      return;
    }
  g_free (current);

  line = format_line (key, value, quote);

  i = find_active (config, key);
  if (i >= 0)
    {
      g_free (g_ptr_array_index (config->lines, i));
      g_ptr_array_index (config->lines, i) = line;
    }
  else
    {
      i = find_commented (config, key);
      if (i >= 0)
        g_ptr_array_insert (config->lines, i + 1, line);
      else
        g_ptr_array_add (config->lines, line);
    }

  config->dirty = TRUE;
}



SpeechdConfig *
speechd_config_load (GError **error)
{
  SpeechdConfig *config;
  gchar         *contents = NULL;
  gchar        **lines;
  guint          i, n;

  config = g_new0 (SpeechdConfig, 1);
  config->path = g_build_filename (g_get_user_config_dir (),
                                   "speech-dispatcher", "speechd.conf", NULL);
  config->lines = g_ptr_array_new_with_free_func (g_free);

  if (g_file_test (config->path, G_FILE_TEST_EXISTS))
    {
      if (!g_file_get_contents (config->path, &contents, NULL, error))
        {
          speechd_config_free (config);
          return NULL;
        }
    }
  else
    {
      gchar *system_path;

      /* the user file is only written once something is changed */
      system_path = g_build_filename (SPEECHD_SYSCONFDIR, "speechd.conf", NULL);
      if (!g_file_get_contents (system_path, &contents, NULL, NULL))
        contents = g_strdup ("");
      g_free (system_path);
    }

  lines = g_strsplit (contents, "\n", -1);
  n = g_strv_length (lines);

  /* a final newline gives an empty last element, which is not a line */
  if (n > 0 && *lines[n - 1] == '\0')
    n--;

  for (i = 0; i < n; i++)
    g_ptr_array_add (config->lines, g_strdup (lines[i]));

  g_strfreev (lines);
  g_free (contents);

  return config;
}



void
speechd_config_free (SpeechdConfig *config)
{
  if (config == NULL)
    return;

  g_ptr_array_unref (config->lines);
  g_free (config->path);
  g_free (config);
}



const gchar *
speechd_config_get_path (SpeechdConfig *config)
{
  return config->path;
}



gboolean
speechd_config_is_dirty (SpeechdConfig *config)
{
  return config->dirty;
}



/* The value of @key without quotes, or NULL when it is not set. */
gchar *
speechd_config_get (SpeechdConfig *config,
                    const gchar   *key)
{
  const gchar *rest;
  gchar       *k = NULL;
  gchar       *value;
  gint         i;

  i = find_active (config, key);
  if (i < 0)
    return NULL;

  rest = parse_option_line (g_ptr_array_index (config->lines, i), FALSE, &k);
  g_free (k);
  value = parse_value (rest, NULL);

  return value;
}



gint
speechd_config_get_int (SpeechdConfig *config,
                        const gchar   *key,
                        gint           fallback)
{
  gchar  *value;
  gchar  *end;
  gint64  number;

  value = speechd_config_get (config, key);
  if (value == NULL)
    return fallback;

  number = g_ascii_strtoll (value, &end, 10);
  if (*end != '\0' || end == value)
    number = fallback;
  g_free (value);

  return CLAMP (number, G_MININT, G_MAXINT);
}



void
speechd_config_set (SpeechdConfig *config,
                    const gchar   *key,
                    const gchar   *value)
{
  g_return_if_fail (value != NULL);
  g_return_if_fail (strchr (value, '"') == NULL);

  set_line (config, key, value, TRUE);
}



void
speechd_config_set_int (SpeechdConfig *config,
                        const gchar   *key,
                        gint           value)
{
  gchar text[16];

  g_snprintf (text, sizeof (text), "%d", value);
  set_line (config, key, text, FALSE);
}



/* Comments out every active line of @key, so the built-in default of
 * Speech Dispatcher applies again. */
void
speechd_config_unset (SpeechdConfig *config,
                      const gchar   *key)
{
  gint i;

  while ((i = find_active (config, key)) >= 0)
    {
      gchar *line = g_ptr_array_index (config->lines, i);

      g_ptr_array_index (config->lines, i) = g_strconcat ("# ", line, NULL);
      g_free (line);
      config->dirty = TRUE;
    }
}



gboolean
speechd_config_save (SpeechdConfig *config,
                     GError       **error)
{
  GString *contents;
  gchar   *dirname;
  guint    i;
  gboolean ok;

  if (!config->dirty)
    return TRUE;

  dirname = g_path_get_dirname (config->path);
  if (g_mkdir_with_parents (dirname, 0700) != 0)
    {
      gint errsv = errno;

      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errsv),
                   "%s: %s", dirname, g_strerror (errsv));
      g_free (dirname);
      return FALSE;
    }
  g_free (dirname);

  contents = g_string_new (NULL);
  for (i = 0; i < config->lines->len; i++)
    {
      g_string_append (contents, g_ptr_array_index (config->lines, i));
      g_string_append_c (contents, '\n');
    }

  ok = g_file_set_contents (config->path, contents->str, contents->len, error);
  g_string_free (contents, TRUE);

  if (ok)
    config->dirty = FALSE;

  return ok;
}



/* All values of an option line ("AddModule "a" "b" "c"") from @rest on. */
static gchar **
parse_values (const gchar *rest)
{
  GPtrArray *values = g_ptr_array_new ();
  gchar     *value;

  while ((value = parse_value (rest, &rest)) != NULL)
    g_ptr_array_add (values, value);
  g_ptr_array_add (values, NULL);

  return (gchar **) g_ptr_array_free (values, FALSE);
}



/* Calls @func for every active option line outside the client sections
 * with its index, key and values. */
typedef void (*LineFunc) (SpeechdConfig *config,
                          guint          index,
                          const gchar   *key,
                          gchar        **values,
                          gpointer       data);

static void
foreach_option (SpeechdConfig *config,
                gboolean       with_clients,
                LineFunc       func,
                gpointer       data)
{
  gboolean in_client = FALSE;
  guint    i;

  for (i = 0; i < config->lines->len; i++)
    {
      const gchar *line = g_ptr_array_index (config->lines, i);
      const gchar *rest;
      gchar       *key = NULL;
      gchar      **values;

      in_client = update_client_section (line, in_client);
      rest = parse_option_line (line, FALSE, &key);
      if (rest != NULL && (with_clients || !in_client))
        {
          values = parse_values (rest);
          func (config, i, key, values, data);
          g_strfreev (values);
        }
      g_free (key);
    }
}



static void
find_add_module (SpeechdConfig *config,
                 guint          index,
                 const gchar   *key,
                 gchar        **values,
                 gpointer       data)
{
  if (g_ascii_strcasecmp (key, "AddModule") == 0)
    *(gboolean *) data = TRUE;
}



/* Whether the configuration lists its output modules with AddModule. Then
 * Speech Dispatcher loads only those and does not look for the installed
 * ones. */
gboolean
speechd_config_has_module_list (SpeechdConfig *config)
{
  gboolean found = FALSE;

  foreach_option (config, FALSE, find_add_module, &found);

  return found;
}



typedef struct
{
  gchar      *user_dir;
  GPtrArray  *copies;  /* pairs of source and destination */
  GHashTable *renames; /* old module name → detected name */
  GError     *error;
} DetectData;

static void
plan_detection (SpeechdConfig *config,
                guint          index,
                const gchar   *key,
                gchar        **values,
                gpointer       user_data)
{
  DetectData *data = user_data;
  gchar      *conf, *binary, *name, *source, *dest;

  if (data->error != NULL || g_ascii_strcasecmp (key, "AddModule") != 0)
    return;

  if (g_strv_length (values) < 3)
    {
      g_set_error (&data->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "%s", (gchar *) g_ptr_array_index (config->lines, index));
      return;
    }

  conf = g_path_get_basename (values[2]);

  if (strcmp (values[1], "sd_generic") == 0)
    {
      /* generic modules are found as modules/NAME-generic.conf, and
       * then they are called NAME-generic */
      if (g_str_has_suffix (conf, "-generic.conf"))
        name = g_strndup (conf, strlen (conf) - strlen (".conf"));
      else
        name = g_strconcat (values[0], "-generic", NULL);

      dest = g_strconcat (data->user_dir, G_DIR_SEPARATOR_S, name, ".conf",
                          NULL);
      source = g_build_filename (data->user_dir, conf, NULL);
      if (!g_file_test (source, G_FILE_TEST_IS_REGULAR))
        {
          g_free (source);
          source = g_build_filename (SPEECHD_SYSCONFDIR, "modules", conf, NULL);
        }

      if (strcmp (source, dest) != 0 && !g_file_test (dest, G_FILE_TEST_EXISTS))
        {
          if (!g_file_test (source, G_FILE_TEST_IS_REGULAR))
            g_set_error (&data->error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                         "%s: %s", source, g_strerror (ENOENT));
          g_ptr_array_add (data->copies, source);
          g_ptr_array_add (data->copies, dest);
        }
      else
        {
          g_free (source);
          g_free (dest);
        }

      if (strcmp (values[0], name) != 0)
        g_hash_table_insert (data->renames, g_strdup (values[0]), name);
      else
        g_free (name);
    }
  else
    {
      /* a module binary is found as sd_NAME with NAME.conf */
      binary = g_strconcat ("sd_", values[0], NULL);
      name = g_strconcat (values[0], ".conf", NULL);
      if (strcmp (values[1], binary) != 0 || strcmp (conf, name) != 0)
        g_set_error (&data->error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "“%s” (%s, %s)", values[0], values[1], values[2]);
      g_free (binary);
      g_free (name);
    }

  g_free (conf);
}



static void
rename_modules (SpeechdConfig *config,
                guint          index,
                const gchar   *key,
                gchar        **values,
                gpointer       user_data)
{
  DetectData  *data = user_data;
  const gchar *line = g_ptr_array_index (config->lines, index);
  const gchar *new_name;
  gchar       *indent;
  guint        n;

  if (g_ascii_strcasecmp (key, "DefaultModule") == 0)
    n = 0;
  else if (g_ascii_strcasecmp (key, "LanguageDefaultModule") == 0)
    n = 1;
  else
    return;

  if (g_strv_length (values) <= n)
    return;

  new_name = g_hash_table_lookup (data->renames, values[n]);
  if (new_name == NULL)
    return;

  indent = g_strndup (line, strspn (line, " \t"));
  g_free (g_ptr_array_index (config->lines, index));
  if (n == 0)
    g_ptr_array_index (config->lines, index) =
      g_strdup_printf ("%s%s \"%s\"", indent, key, new_name);
  else
    g_ptr_array_index (config->lines, index) =
      g_strdup_printf ("%s%s \"%s\" \"%s\"", indent, key, values[0], new_name);
  g_free (indent);

  config->dirty = TRUE;
}



/* Comments out the AddModule lines, so Speech Dispatcher loads every
 * installed module again. A generic module that would not be found under
 * its name is copied to the user's modules/NAME-generic.conf, and the
 * DefaultModule settings follow its new name. Nothing is changed when a
 * module cannot be detected this way. The configuration still has to be
 * saved. */
gboolean
speechd_config_use_detected_modules (SpeechdConfig *config,
                                     GError       **error)
{
  DetectData data = { NULL, };
  gchar     *contents;
  gsize      length;
  guint      i;
  gboolean   ok = FALSE;

  data.user_dir = g_build_filename (g_get_user_config_dir (),
                                    "speech-dispatcher", "modules", NULL);
  data.copies = g_ptr_array_new_with_free_func (g_free);
  data.renames = g_hash_table_new_full (g_str_hash, g_str_equal,
                                        g_free, g_free);

  foreach_option (config, FALSE, plan_detection, &data);
  if (data.error != NULL)
    {
      g_propagate_error (error, data.error);
      goto out;
    }

  if (data.copies->len > 0 && g_mkdir_with_parents (data.user_dir, 0700) != 0)
    {
      gint errsv = errno;

      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errsv),
                   "%s: %s", data.user_dir, g_strerror (errsv));
      goto out;
    }

  for (i = 0; i < data.copies->len; i += 2)
    {
      if (!g_file_get_contents (g_ptr_array_index (data.copies, i),
                                &contents, &length, error))
        goto out;
      if (!g_file_set_contents (g_ptr_array_index (data.copies, i + 1),
                                contents, length, error))
        {
          g_free (contents);
          goto out;
        }
      g_free (contents);
    }

  speechd_config_unset (config, "AddModule");
  foreach_option (config, TRUE, rename_modules, &data);
  ok = TRUE;

out:
  g_free (data.user_dir);
  g_ptr_array_unref (data.copies);
  g_hash_table_unref (data.renames);

  return ok;
}



/* Makes the running server read its configuration again. SIGHUP resets all
 * defaults to the built-in values before the file is read, and restarts the
 * output modules. Not an error when no server is running: the next one
 * reads the new file anyway. */
gboolean
speechd_server_reload (GError **error)
{
  gchar  *pid_file;
  gchar  *contents = NULL;
  gchar  *comm_file;
  gchar  *comm = NULL;
  gchar  *end;
  gint64  pid;

  pid_file = g_build_filename (g_get_user_runtime_dir (), "speech-dispatcher",
                               "pid", "speech-dispatcher.pid", NULL);
  if (!g_file_get_contents (pid_file, &contents, NULL, NULL))
    {
      g_free (pid_file);
      return TRUE;
    }
  g_free (pid_file);

  pid = g_ascii_strtoll (g_strstrip (contents), &end, 10);
  if (*end != '\0' || pid <= 1)
    {
      g_free (contents);
      return TRUE;
    }
  g_free (contents);

  /* the pid file can be stale; only signal Speech Dispatcher itself */
  comm_file = g_strdup_printf ("/proc/%" G_GINT64_FORMAT "/comm", pid);
  if (!g_file_get_contents (comm_file, &comm, NULL, NULL)
      || !g_str_has_prefix (comm, "speech-dispatch"))
    {
      g_free (comm_file);
      g_free (comm);
      return TRUE;
    }
  g_free (comm_file);
  g_free (comm);

  if (kill ((pid_t) pid, SIGHUP) != 0)
    {
      gint errsv = errno;

      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errsv),
                   "kill (%" G_GINT64_FORMAT ", SIGHUP): %s", pid,
                   g_strerror (errsv));
      return FALSE;
    }

  return TRUE;
}



/* ---- Per-language voices in the output module configurations ---- */

/* Speech Dispatcher has no per-language voice. Modules choose it in their
 * own configuration: the generic modules map the voice types of a language
 * to voices with AddVoice lines (a later line for the same language and type
 * replaces an earlier one), our modules read "<Module>Voice "lang" "voice"". */

/* the fallback order of module_getvoice() for a type without a voice */
static const gchar *const voice_type_order[] =
{
  "MALE1", "FEMALE1", "MALE2", "FEMALE2", "MALE3", "FEMALE3",
  "CHILD_MALE", "CHILD_FEMALE",
};

static gchar *
module_config_path (const gchar *module,
                    gboolean     user)
{
  gchar *name = g_strconcat (module, ".conf", NULL);
  gchar *path;

  if (user)
    path = g_build_filename (g_get_user_config_dir (), "speech-dispatcher",
                             "modules", name, NULL);
  else
    path = g_build_filename (SPEECHD_SYSCONFDIR, "modules", name, NULL);
  g_free (name);

  return path;
}

/* The lines of the module configuration that Speech Dispatcher reads: the
 * user's copy, else the system one. NULL when there is none. */
static gchar **
module_config_read (const gchar *module)
{
  gchar *contents = NULL;
  gchar *path;
  gchar **lines;
  guint  n;

  if (module == NULL || *module == '\0' || strchr (module, '/') != NULL)
    return NULL;

  path = module_config_path (module, TRUE);
  if (!g_file_get_contents (path, &contents, NULL, NULL))
    {
      g_free (path);
      path = module_config_path (module, FALSE);
      if (!g_file_get_contents (path, &contents, NULL, NULL))
        contents = NULL;
    }
  g_free (path);

  if (contents == NULL)
    return NULL;

  lines = g_strsplit (contents, "\n", -1);
  g_free (contents);
  n = g_strv_length (lines);
  if (n > 0 && *lines[n - 1] == '\0')
    {
      g_free (lines[n - 1]);
      lines[n - 1] = NULL;
    }

  return lines;
}

/* The values of line @line when its key is @key, else NULL */
static gchar **
module_line_values (const gchar *line,
                    const gchar *key)
{
  const gchar *rest;
  gchar       *k = NULL;
  gchar      **values = NULL;

  rest = parse_option_line (line, FALSE, &k);
  if (rest != NULL && g_ascii_strcasecmp (k, key) == 0)
    values = parse_values (rest);
  g_free (k);

  return values;
}

/* "Zonos3Voice" for "zonos3": the setting of our own modules */
static gchar *
module_voice_key (const gchar *module)
{
  gchar *key = g_strconcat (module, "Voice", NULL);

  key[0] = g_ascii_toupper (key[0]);

  return key;
}

/* our modules, which read "<Module>Voice", also from older configurations */
static const gchar *const own_modules[] = { "zonos3", "sherpa", "vosk", "kokoro", NULL };

SpeechdVoiceSetting
speechd_module_voice_setting (const gchar *module)
{
  gchar             **lines;
  SpeechdVoiceSetting setting = SPEECHD_VOICE_SETTING_NONE;
  guint               i;

  if (module != NULL && g_strv_contains (own_modules, module))
    return SPEECHD_VOICE_SETTING_MODULE;

  lines = module_config_read (module);
  if (lines == NULL)
    return setting;

  for (i = 0; lines[i] != NULL; i++)
    {
      const gchar *rest;
      gchar       *key = NULL;

      /* commented-out examples tell as much as active lines */
      rest = parse_option_line (lines[i], FALSE, &key);
      if (rest == NULL)
        rest = parse_option_line (lines[i], TRUE, &key);
      if (rest == NULL)
        continue;
      if (g_ascii_strcasecmp (key, "GenericExecuteSynth") == 0)
        setting = SPEECHD_VOICE_SETTING_ADD_VOICE;
      else if (setting == SPEECHD_VOICE_SETTING_NONE)
        {
          gchar *voice_key = module_voice_key (module);

          if (g_ascii_strcasecmp (key, voice_key) == 0)
            setting = SPEECHD_VOICE_SETTING_MODULE;
          g_free (voice_key);
        }
      g_free (key);
    }
  g_strfreev (lines);

  return setting;
}

/* The voice the module speaks @language with for voice type @type (as in
 * DefaultVoiceType), or NULL when its configuration does not say. */
gchar *
speechd_module_get_voice (const gchar *module,
                          const gchar *language,
                          const gchar *type)
{
  SpeechdVoiceSetting setting = speechd_module_voice_setting (module);
  gchar             **lines;
  gchar              *voice_key;
  gchar              *found = NULL;
  gchar              *by_type[G_N_ELEMENTS (voice_type_order)] = { NULL, };
  guint               i, t;

  if (setting == SPEECHD_VOICE_SETTING_NONE || language == NULL)
    return NULL;

  lines = module_config_read (module);
  voice_key = module_voice_key (module);
  for (i = 0; lines != NULL && lines[i] != NULL; i++)
    {
      gchar **values;

      if (setting == SPEECHD_VOICE_SETTING_MODULE)
        {
          values = module_line_values (lines[i], voice_key);
          if (values != NULL && g_strv_length (values) >= 2
              && g_ascii_strcasecmp (values[0], language) == 0)
            {
              g_free (found);
              found = g_strdup (values[1]);
            }
        }
      else
        {
          values = module_line_values (lines[i], "AddVoice");
          if (values != NULL && g_strv_length (values) >= 3
              && g_ascii_strcasecmp (values[0], language) == 0)
            for (t = 0; t < G_N_ELEMENTS (voice_type_order); t++)
              if (g_ascii_strcasecmp (values[1], voice_type_order[t]) == 0)
                {
                  g_free (by_type[t]);
                  by_type[t] = g_strdup (values[2]);
                }
        }
      g_strfreev (values);
    }
  g_free (voice_key);
  g_strfreev (lines);

  if (setting == SPEECHD_VOICE_SETTING_ADD_VOICE)
    {
      for (t = 0; type != NULL && t < G_N_ELEMENTS (voice_type_order); t++)
        if (g_ascii_strcasecmp (type, voice_type_order[t]) == 0 && by_type[t])
          found = g_strdup (by_type[t]);
      for (t = 0; found == NULL && t < G_N_ELEMENTS (voice_type_order); t++)
        if (by_type[t] != NULL)
          found = g_strdup (by_type[t]);
      for (t = 0; t < G_N_ELEMENTS (voice_type_order); t++)
        g_free (by_type[t]);
    }

  return found;
}

/* Makes @voice the voice of @language in the module configuration, writing
 * the user's copy (made from the system file when there is none yet).
 * Generic modules: the AddVoice line of the voice gets type @type and moves
 * behind the other lines of the language, so it wins. Our modules: one
 * "<Module>Voice "lang" "voice"" line per language. The server has to be
 * reloaded afterwards. */
gboolean
speechd_module_set_voice (const gchar *module,
                          const gchar *language,
                          const gchar *type,
                          const gchar *voice,
                          GError     **error)
{
  SpeechdVoiceSetting setting = speechd_module_voice_setting (module);
  gchar             **lines;
  GPtrArray          *out;
  GString            *contents;
  gchar              *voice_key;
  gchar              *new_line;
  gchar              *path, *dir;
  gint                last = -1;
  guint               i;
  gboolean            ok;

  if (setting == SPEECHD_VOICE_SETTING_NONE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "%s: no voice setting", module);
      return FALSE;
    }
  if (strchr (language, '"') != NULL || strchr (voice, '"') != NULL
      || (type != NULL && strchr (type, '"') != NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "%s: invalid voice", voice);
      return FALSE;
    }

  voice_key = module_voice_key (module);
  if (setting == SPEECHD_VOICE_SETTING_MODULE)
    new_line = g_strdup_printf ("%s \"%s\" \"%s\"", voice_key, language, voice);
  else
    new_line = g_strdup_printf ("AddVoice \"%s\" \"%s\" \"%s\"", language,
                                type != NULL ? type : "MALE1", voice);

  /* drop the old line, remember where the language's lines end */
  lines = module_config_read (module);
  out = g_ptr_array_new ();
  for (i = 0; lines != NULL && lines[i] != NULL; i++)
    {
      gchar  **values;
      gboolean drop = FALSE;

      if (setting == SPEECHD_VOICE_SETTING_MODULE)
        {
          values = module_line_values (lines[i], voice_key);
          if (values != NULL && g_strv_length (values) >= 2
              && g_ascii_strcasecmp (values[0], language) == 0)
            drop = TRUE;
        }
      else
        {
          values = module_line_values (lines[i], "AddVoice");
          if (values != NULL && g_strv_length (values) >= 3
              && g_ascii_strcasecmp (values[0], language) == 0)
            {
              drop = strcmp (values[2], voice) == 0;
              if (!drop)
                last = out->len;
            }
        }
      g_strfreev (values);

      if (!drop)
        g_ptr_array_add (out, lines[i]);
      else if (last < 0 && setting == SPEECHD_VOICE_SETTING_MODULE)
        last = (gint) out->len - 1;
    }

  if (last < 0)
    g_ptr_array_add (out, new_line);
  else
    g_ptr_array_insert (out, last + 1, new_line);

  contents = g_string_new (NULL);
  for (i = 0; i < out->len; i++)
    {
      g_string_append (contents, g_ptr_array_index (out, i));
      g_string_append_c (contents, '\n');
    }

  path = module_config_path (module, TRUE);
  dir = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      gint errsv = errno;

      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errsv),
                   "%s: %s", dir, g_strerror (errsv));
      ok = FALSE;
    }
  else
    ok = g_file_set_contents (path, contents->str, contents->len, error);

  g_free (dir);
  g_free (path);
  g_string_free (contents, TRUE);
  g_ptr_array_free (out, TRUE);
  g_strfreev (lines);
  g_free (new_line);
  g_free (voice_key);

  return ok;
}

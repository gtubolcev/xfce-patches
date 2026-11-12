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

#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>

#include <libxfce4ui/libxfce4ui.h>
#include <libxfce4util/libxfce4util.h>
#include <speech-dispatcher/libspeechd.h>

#include "speech-page.h"
#include "speechd-config.h"

#define SAVE_DELAY_MS 400
/* time the server needs to restart its modules after a reload */
#define REFILL_DELAY_MS 1500

/* Built-in defaults of Speech Dispatcher, used when a key is not set */
#define DEFAULT_LANGUAGE "en-US"

typedef struct
{
  const gchar *id;
  const gchar *label;
} Choice;

/* the order matches SPDVoiceType, starting at SPD_MALE1 */
static const Choice voice_types[] =
{
  { "MALE1", N_("Male 1") },
  { "MALE2", N_("Male 2") },
  { "MALE3", N_("Male 3") },
  { "FEMALE1", N_("Female 1") },
  { "FEMALE2", N_("Female 2") },
  { "FEMALE3", N_("Female 3") },
  { "CHILD_MALE", N_("Child, male") },
  { "CHILD_FEMALE", N_("Child, female") },
};

static const Choice punctuation_modes[] =
{
  { "none", N_("None") },
  { "some", N_("Some") },
  { "most", N_("Most") },
  { "all", N_("All") },
};

static const SPDPunctuation punctuation_values[] =
{
  SPD_PUNCT_NONE, SPD_PUNCT_SOME, SPD_PUNCT_MOST, SPD_PUNCT_ALL,
};

static const Choice capital_modes[] =
{
  { "none", N_("Do nothing") },
  { "spell", N_("Say “capital”") },
  { "icon", N_("Play a sound") },
};

static const SPDCapitalLetters capital_values[] =
{
  SPD_CAP_NONE, SPD_CAP_SPELL, SPD_CAP_ICON,
};

/* an empty id leaves the choice to Speech Dispatcher */
static const Choice audio_methods[] =
{
  { "", N_("Automatic") },
  { "pipewire", "PipeWire" },
  { "pulse", "PulseAudio" },
  { "alsa", "ALSA" },
  { "oss", "OSS" },
  { "nas", "NAS" },
  { "libao", "libao" },
};

/* modules Speech Dispatcher always adds itself, not a real choice, and the
 * legacy engines, whose voices are no choice next to the others: eSpeak
 * (user, 2026-11-11), Festival, Flite and Pico (user, 2026-11-12) */
static const gchar *hidden_modules[] = { "dummy", "espeak-ng-fallback",
                                         "espeak", "espeak-ng", "festival",
                                         "flite", "pico", NULL };

/* The test sentence in the language that is spoken, not in the language of
 * the dialog, so these are not translated. */
static const struct
{
  const gchar *language;
  const gchar *text;
}
test_texts[] =
{
  { "cs", "Takhle mluví váš počítač." },
  { "de", "So spricht Ihr Computer." },
  { "en", "This is how your computer speaks." },
  { "es", "Así habla tu ordenador." },
  { "fi", "Näin tietokoneesi puhuu." },
  { "fr", "Voici comment parle votre ordinateur." },
  { "it", "Così parla il tuo computer." },
  { "ja", "これがあなたのコンピューターの声です。" },
  { "nl", "Zo spreekt je computer." },
  { "pl", "Tak mówi twój komputer." },
  { "pt", "É assim que o seu computador fala." },
  { "ru", "Так говорит ваш компьютер." },
  { "sv", "Så här talar din dator." },
  { "tr", "Bilgisayarınız böyle konuşur." },
  { "uk", "Так говорить ваш комп’ютер." },
  { "zh", "这就是你的电脑说话的声音。" },
};

typedef struct
{
  SpeechdConfig *config;
  SPDConnection *connection;

  GtkWidget     *info_bar;
  GtkWidget     *info_label;
  GtkWidget     *modules_bar;

  GtkWidget     *module_combo;
  GtkWidget     *language_combo;
  GtkWidget     *voice_combo;
  GtkWidget     *voice_type_label;
  GtkWidget     *voice_type_combo;
  /* the voices of the selected module, NULL-terminated, or NULL */
  SPDVoice     **voices;
  /* the modules of the server, and the languages each one has voices for */
  gchar        **modules;
  GHashTable    *module_languages;
  /* the languages of all modules, sorted */
  GPtrArray     *languages;
  GtkWidget     *rate_scale;
  GtkWidget     *pitch_scale;
  GtkWidget     *volume_scale;
  GtkWidget     *punctuation_combo;
  GtkWidget     *capital_combo;
  GtkWidget     *audio_combo;
  GtkWidget     *test_entry;
  /* the sentence put into test_entry for a language, while the user keeps it */
  const gchar   *test_text;

  guint          save_id;
  guint          refill_id;
  /* the server the module list came from (its socket), see
   * speech_page_server_changed */
  dev_t          server_dev;
  ino_t          server_ino;
  /* a module configuration was written, the server has to read it */
  gboolean       reload_pending;
  gboolean       updating;
} SpeechPage;



static void
speech_page_show_message (SpeechPage     *page,
                          GtkMessageType  type,
                          const gchar    *message)
{
  gtk_info_bar_set_message_type (GTK_INFO_BAR (page->info_bar), type);
  gtk_label_set_text (GTK_LABEL (page->info_label), message);
  gtk_widget_show (page->info_bar);
}



static gboolean
speech_page_save (gpointer data)
{
  SpeechPage *page = data;
  GError     *error = NULL;
  gchar      *message;

  page->save_id = 0;

  if (!speechd_config_is_dirty (page->config) && !page->reload_pending)
    return G_SOURCE_REMOVE;
  page->reload_pending = FALSE;

  if (!speechd_config_save (page->config, &error)
      || !speechd_server_reload (&error))
    {
      message = g_strdup_printf (_("Failed to apply the settings: %s"),
                                 error->message);
      speech_page_show_message (page, GTK_MESSAGE_ERROR, message);
      g_free (message);
      g_error_free (error);
    }

  return G_SOURCE_REMOVE;
}



static void
speech_page_queue_save (SpeechPage *page)
{
  if (page->save_id != 0)
    g_source_remove (page->save_id);
  page->save_id = g_timeout_add (SAVE_DELAY_MS, speech_page_save, page);
}



static GtkWidget *
speech_page_combo_new (const Choice *choices,
                       guint         n_choices)
{
  GtkWidget *combo;
  guint      i;

  combo = gtk_combo_box_text_new ();
  for (i = 0; i < n_choices; i++)
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), choices[i].id,
                               _(choices[i].label));

  return combo;
}



/* Selects the entry with @value (case-insensitive like Speech Dispatcher),
 * or @fallback when there is none. */
static void
speech_page_combo_select (GtkWidget    *combo,
                          const Choice *choices,
                          guint         n_choices,
                          const gchar  *value,
                          const gchar  *fallback)
{
  guint i;

  for (i = 0; value != NULL && i < n_choices; i++)
    if (g_ascii_strcasecmp (choices[i].id, value) == 0)
      {
        gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), choices[i].id);
        return;
      }

  gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), fallback);
}



static GtkWidget *
speech_page_scale_new (gint         value,
                       const gchar *low,
                       const gchar *high)
{
  GtkWidget *scale;

  scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, -100, 100, 1);
  gtk_scale_set_digits (GTK_SCALE (scale), 0);
  gtk_scale_set_draw_value (GTK_SCALE (scale), FALSE);
  /* 0 is what the synthesizer does by default, not silence */
  gtk_scale_add_mark (GTK_SCALE (scale), -100, GTK_POS_BOTTOM, low);
  gtk_scale_add_mark (GTK_SCALE (scale), 0, GTK_POS_BOTTOM, _("Normal"));
  gtk_scale_add_mark (GTK_SCALE (scale), 100, GTK_POS_BOTTOM, high);
  gtk_range_set_value (GTK_RANGE (scale), CLAMP (value, -100, 100));
  gtk_widget_set_hexpand (scale, TRUE);

  return scale;
}



/* labels of all frames share one width, so the controls line up */
static GtkSizeGroup *label_size_group = NULL;

static void
speech_page_grid_add (GtkWidget   *grid,
                      gint         row,
                      const gchar *text,
                      GtkWidget   *widget)
{
  GtkWidget *label;

  label = gtk_label_new_with_mnemonic (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_size_group_add_widget (label_size_group, label);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), widget);
  gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);

  gtk_widget_set_hexpand (widget, TRUE);
  gtk_grid_attach (GTK_GRID (grid), widget, 1, row, 1, 1);
}



static GtkWidget *
speech_page_frame_new (GtkWidget   *box,
                       const gchar *title)
{
  GtkWidget *frame;
  GtkWidget *bin;
  GtkWidget *grid;

  frame = xfce_gtk_frame_box_new (title, &bin);
  gtk_box_pack_start (GTK_BOX (box), frame, FALSE, TRUE, 0);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
  gtk_container_add (GTK_CONTAINER (bin), grid);

  return grid;
}



static gboolean
speech_page_connect (SpeechPage *page)
{
  gchar *error = NULL;
  gchar *message;

  if (page->connection != NULL)
    return TRUE;

  page->connection = spd_open2 ("xfce4-speech-settings", "main", NULL,
                                SPD_MODE_SINGLE, NULL, TRUE, &error);
  if (page->connection == NULL)
    {
      message = g_strdup_printf (_("Cannot connect to Speech Dispatcher: %s"),
                                 error != NULL ? error : _("unknown error"));
      speech_page_show_message (page, GTK_MESSAGE_WARNING, message);
      g_free (message);
      g_free (error);
      return FALSE;
    }

  return TRUE;
}



static gboolean
speech_page_same_language (const gchar *a,
                           const gchar *b);
static gchar *
speech_page_get_language (SpeechPage *page);
static gint
speech_page_compare_strings (gconstpointer a,
                             gconstpointer b);



/* Whether @module has a voice for @language; a module that does not list
 * its voices may have one */
static gboolean
speech_page_module_speaks (SpeechPage  *page,
                           const gchar *module,
                           const gchar *language)
{
  GPtrArray *languages;
  guint      i;

  languages = g_hash_table_lookup (page->module_languages, module);
  if (languages == NULL || languages->len == 0)
    return TRUE;

  for (i = 0; i < languages->len; i++)
    if (speech_page_same_language (g_ptr_array_index (languages, i), language))
      return TRUE;

  return FALSE;
}



static gboolean
speech_page_array_has (GPtrArray   *array,
                       const gchar *text)
{
  guint i;

  for (i = 0; i < array->len; i++)
    if (strcmp (g_ptr_array_index (array, i), text) == 0)
      return TRUE;

  return FALSE;
}



/* The socket of the server; a new one means the server was restarted (for
 * example to pick up a newly installed module). Only the default address. */
static gboolean
speech_page_server_stat (struct stat *st)
{
  gchar   *path;
  gboolean ok;

  if (g_getenv ("SPEECHD_ADDRESS") != NULL)
    return FALSE;
  path = g_build_filename (g_get_user_runtime_dir (), "speech-dispatcher",
                           "speechd.sock", NULL);
  ok = stat (path, st) == 0;
  g_free (path);
  return ok;
}



/* Asks every module for its voices, to know the languages it speaks. The
 * connection is opened anew afterwards, so it uses the default module again. */
static void
speech_page_scan_modules (SpeechPage *page)
{
  guint i;
  guint j;

  g_clear_pointer (&page->modules, g_strfreev);
  g_hash_table_remove_all (page->module_languages);
  g_ptr_array_set_size (page->languages, 0);

  if (!speech_page_connect (page))
    return;

  {
    struct stat st;

    if (speech_page_server_stat (&st))
      {
        page->server_dev = st.st_dev;
        page->server_ino = st.st_ino;
      }
  }

  page->modules = spd_list_modules (page->connection);

  for (i = 0; page->modules != NULL && page->modules[i] != NULL; i++)
    {
      const gchar *module = page->modules[i];
      GPtrArray   *languages;
      SPDVoice   **voices;
      gchar       *base;

      if (g_strv_contains (hidden_modules, module)
          || spd_set_output_module (page->connection, module) != 0)
        continue;

      voices = spd_list_synthesis_voices (page->connection);
      languages = g_ptr_array_new_with_free_func (g_free);
      for (j = 0; voices != NULL && voices[j] != NULL; j++)
        {
          const gchar *language = voices[j]->language;

          if (language == NULL || *language == '\0'
              || speech_page_array_has (languages, language))
            continue;
          g_ptr_array_add (languages, g_strdup (language));

          /* one entry per language: "ru" for "ru", "ru-RU" and "ru_RU";
           * the voices of a language are matched the same way */
          base = g_ascii_strdown (language, strcspn (language, "-_"));
          if (!speech_page_array_has (page->languages, base))
            g_ptr_array_add (page->languages, base);
          else
            g_free (base);
        }
      if (voices != NULL)
        free_spd_voices (voices);

      g_hash_table_insert (page->module_languages, g_strdup (module),
                           languages);
    }

  g_ptr_array_sort (page->languages, speech_page_compare_strings);

  spd_close (page->connection);
  page->connection = NULL;
  speech_page_connect (page);
}



/* Offers the modules that speak the selected language, and the selected one */
static void
speech_page_fill_modules (SpeechPage *page)
{
  GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT (page->module_combo);
  gchar           *current;
  gchar           *language;
  gchar           *label;
  gboolean         found = FALSE;
  guint            i;

  current = g_strdup (gtk_combo_box_get_active_id (GTK_COMBO_BOX (combo)));
  if (current == NULL)
    current = speechd_config_get (page->config, "DefaultModule");
  language = speech_page_get_language (page);

  gtk_combo_box_text_remove_all (combo);
  gtk_combo_box_text_append (combo, "", _("Automatic"));

  for (i = 0; page->modules != NULL && page->modules[i] != NULL; i++)
    {
      const gchar *module = page->modules[i];

      if (g_strv_contains (hidden_modules, module))
        continue;
      if (g_strcmp0 (module, current) == 0)
        found = TRUE;
      else if (!speech_page_module_speaks (page, module, language))
        continue;

      if (speech_page_module_speaks (page, module, language))
        gtk_combo_box_text_append (combo, module, module);
      else
        {
          /* TRANSLATORS: a synthesizer without a voice for the language */
          label = g_strdup_printf (_("%s (no voice for this language)"), module);
          gtk_combo_box_text_append (combo, module, label);
          g_free (label);
        }
    }

  /* keep a configured module that is not loaded at the moment */
  if (current != NULL && *current != '\0' && !found)
    gtk_combo_box_text_append (combo, current, current);

  gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo),
                               current != NULL ? current : "");

  g_free (language);
  g_free (current);
}



static gint
speech_page_compare_strings (gconstpointer a,
                             gconstpointer b)
{
  return g_strcmp0 (*(const gchar **) a, *(const gchar **) b);
}



/* Offers the languages of all modules */
static void
speech_page_fill_languages (SpeechPage *page)
{
  GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT (page->language_combo);
  guint            i;

  gtk_combo_box_text_remove_all (combo);
  for (i = 0; i < page->languages->len; i++)
    gtk_combo_box_text_append_text (combo,
                                    g_ptr_array_index (page->languages, i));
}



/* Asks the selected module for its voices */
static void
speech_page_load_voices (SpeechPage *page)
{
  const gchar *module;

  if (page->voices != NULL)
    free_spd_voices (page->voices);
  page->voices = NULL;

  if (page->connection == NULL)
    return;

  module = gtk_combo_box_get_active_id (GTK_COMBO_BOX (page->module_combo));
  if (module != NULL && *module != '\0')
    spd_set_output_module (page->connection, module);

  page->voices = spd_list_synthesis_voices (page->connection);
}



static gboolean
speech_page_same_language (const gchar *a,
                           const gchar *b)
{
  gsize len = strcspn (a, "-_");

  return len == strcspn (b, "-_") && g_ascii_strncasecmp (a, b, len) == 0;
}



/* The voice of @voices that a voice type like "FEMALE2" selects for
 * @language, or NULL when the module does not tell.  Modules report either
 * the gender ("male"/"female", sd_zonos3: the n-th voice of that gender,
 * else the n-th voice) or the voice type a voice serves (the generic
 * modules' AddVoice: that type, else MALE1, else the first voice). */
static const gchar *
speech_page_resolve_voice (SPDVoice   **voices,
                           const gchar *language,
                           const gchar *type)
{
  GPtrArray   *candidates;
  const gchar *gender = NULL;
  const gchar *rest = type;
  const gchar *found = NULL;
  gboolean     by_gender = FALSE;
  gboolean     by_type = FALSE;
  guint        index = 0;
  guint        i, t;

  if (voices == NULL)
    return NULL;

  candidates = g_ptr_array_new ();
  for (i = 0; voices[i] != NULL; i++)
    {
      const gchar *variant = voices[i]->variant;

      if (voices[i]->language == NULL
          || !speech_page_same_language (voices[i]->language, language))
        continue;
      g_ptr_array_add (candidates, voices[i]);
      if (variant == NULL)
        continue;
      if (g_ascii_strcasecmp (variant, "male") == 0
          || g_ascii_strcasecmp (variant, "female") == 0)
        by_gender = TRUE;
      for (t = 0; t < G_N_ELEMENTS (voice_types); t++)
        if (g_ascii_strcasecmp (variant, voice_types[t].id) == 0)
          by_type = TRUE;
    }

  if (g_ascii_strncasecmp (rest, "CHILD_", 6) == 0)
    rest += 6;
  if (g_ascii_strncasecmp (rest, "FEMALE", 6) == 0)
    gender = "female", rest += 6;
  else if (g_ascii_strncasecmp (rest, "MALE", 4) == 0)
    gender = "male", rest += 4;
  if (g_ascii_isdigit (*rest) && atoi (rest) > 0)
    index = atoi (rest) - 1;

  /* sd_zonos3 without genders: the n-th voice of the language */
  if (candidates->len > 0 && !by_gender && !by_type)
    found = ((SPDVoice *) g_ptr_array_index (candidates,
                                             index % candidates->len))->name;
  else if (candidates->len > 0 && by_gender)
    {
      GPtrArray *same = g_ptr_array_new ();

      for (i = 0; i < candidates->len; i++)
        {
          SPDVoice *voice = g_ptr_array_index (candidates, i);

          if (voice->variant != NULL && gender != NULL
              && g_ascii_strcasecmp (voice->variant, gender) == 0)
            g_ptr_array_add (same, voice);
        }
      if (same->len == 0)
        g_ptr_array_extend (same, candidates, NULL, NULL);
      found = ((SPDVoice *) g_ptr_array_index (same, index % same->len))->name;
      g_ptr_array_free (same, TRUE);
    }
  else if (candidates->len > 0 && by_type)
    {
      const gchar *tries[] = { type, "MALE1" };

      for (t = 0; t < G_N_ELEMENTS (tries) && found == NULL; t++)
        for (i = 0; i < candidates->len && found == NULL; i++)
          {
            SPDVoice *voice = g_ptr_array_index (candidates, i);

            if (voice->variant != NULL
                && g_ascii_strcasecmp (voice->variant, tries[t]) == 0)
              found = voice->name;
          }
      if (found == NULL)
        found = ((SPDVoice *) g_ptr_array_index (candidates, 0))->name;
    }

  g_ptr_array_free (candidates, TRUE);

  return found;
}



/* The module the dialog configures: the chosen one, else the default */
static gchar *
speech_page_get_module (SpeechPage *page)
{
  const gchar *module;

  module = gtk_combo_box_get_active_id (GTK_COMBO_BOX (page->module_combo));
  if (module != NULL && *module != '\0')
    return g_strdup (module);

  return speechd_config_get (page->config, "DefaultModule");
}



static gchar *
speech_page_get_language (SpeechPage *page)
{
  GtkWidget   *entry = gtk_bin_get_child (GTK_BIN (page->language_combo));
  const gchar *language = gtk_entry_get_text (GTK_ENTRY (entry));

  if (language == NULL || *language == '\0')
    language = DEFAULT_LANGUAGE;

  return g_strstrip (g_strdup (language));
}



/* The voice type the server asks the modules for */
static gchar *
speech_page_get_voice_type (SpeechPage *page)
{
  gchar *type = speechd_config_get (page->config, "DefaultVoiceType");

  if (type == NULL || *type == '\0')
    {
      g_free (type);
      type = g_strdup ("MALE1");
    }

  return type;
}



/* The voice of @name for @language in the module's voice list, or NULL */
static SPDVoice *
speech_page_find_voice (SpeechPage  *page,
                        const gchar *name,
                        const gchar *language)
{
  guint i;

  for (i = 0; page->voices != NULL && page->voices[i] != NULL; i++)
    if (g_strcmp0 (page->voices[i]->name, name) == 0
        && page->voices[i]->language != NULL
        && speech_page_same_language (page->voices[i]->language, language))
      return page->voices[i];

  return NULL;
}



/* Lists the voices the selected synthesizer has for the selected language
 * and selects the one it speaks with: the voice set in its configuration,
 * else the one the voice type selects. Synthesizers without a voice setting
 * keep the voice type list. */
static void
speech_page_update_voices (SpeechPage *page)
{
  GtkComboBoxText    *combo;
  SpeechdVoiceSetting setting;
  GHashTable         *seen;
  gchar              *module;
  gchar              *language;
  gchar              *type;
  gchar              *active = NULL;
  const gchar        *voice_language = NULL;
  gboolean            updating = page->updating;
  guint               n = 0;
  guint               i;

  if (page->voice_combo == NULL)
    return;
  combo = GTK_COMBO_BOX_TEXT (page->voice_combo);

  module = speech_page_get_module (page);
  language = speech_page_get_language (page);
  type = speech_page_get_voice_type (page);
  setting = speechd_module_voice_setting (module);

  page->updating = TRUE;
  gtk_combo_box_text_remove_all (combo);

  seen = g_hash_table_new (g_str_hash, g_str_equal);
  for (i = 0; page->voices != NULL && page->voices[i] != NULL; i++)
    {
      SPDVoice *voice = page->voices[i];

      if (voice->name == NULL || voice->language == NULL
          || !speech_page_same_language (voice->language, language)
          || g_hash_table_contains (seen, voice->name))
        continue;
      g_hash_table_add (seen, voice->name);
      gtk_combo_box_text_append (combo, voice->name, voice->name);
      /* an exact match of the language wins, "en" for "en-US" otherwise */
      if (voice_language == NULL
          || g_ascii_strcasecmp (voice->language, language) == 0)
        voice_language = voice->language;
      n++;
    }
  g_hash_table_destroy (seen);

  if (voice_language != NULL)
    active = speechd_module_get_voice (module, voice_language, type);
  /* a configured voice that is gone: the module falls back to the type */
  if (active != NULL && speech_page_find_voice (page, active, language) == NULL)
    g_clear_pointer (&active, g_free);
  if (active == NULL && setting != SPEECHD_VOICE_SETTING_NONE)
    active = g_strdup (speech_page_resolve_voice (page->voices, language,
                                                  type));
  if (active != NULL)
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), active);

  gtk_widget_set_sensitive (page->voice_combo,
                            n > 0 && setting != SPEECHD_VOICE_SETTING_NONE);
  if (n == 0)
    gtk_widget_set_tooltip_text (page->voice_combo,
                                 _("The synthesizer has no voice for this "
                                   "language"));
  else if (setting == SPEECHD_VOICE_SETTING_NONE)
    gtk_widget_set_tooltip_text (page->voice_combo,
                                 _("This synthesizer chooses its voice by "
                                   "the voice type"));
  else
    gtk_widget_set_tooltip_text (page->voice_combo,
                                 _("The voice for this language, kept in the "
                                   "synthesizer's settings"));

  /* only the synthesizers without a voice setting need the voice type */
  gtk_widget_set_visible (page->voice_type_label,
                          setting == SPEECHD_VOICE_SETTING_NONE);
  gtk_widget_set_visible (page->voice_type_combo,
                          setting == SPEECHD_VOICE_SETTING_NONE);

  page->updating = updating;
  g_free (active);
  g_free (type);
  g_free (language);
  g_free (module);
}



static void
speech_page_voice_changed (GtkComboBox *combo,
                           SpeechPage  *page)
{
  const gchar *name = gtk_combo_box_get_active_id (combo);
  SPDVoice    *voice;
  GError      *error = NULL;
  gchar       *module;
  gchar       *language;
  gchar       *type;
  gchar       *message;

  if (page->updating || name == NULL)
    return;

  module = speech_page_get_module (page);
  language = speech_page_get_language (page);
  type = speech_page_get_voice_type (page);
  voice = speech_page_find_voice (page, name, language);

  /* the module knows the language as its voice says, e.g. "ru", not "ru-RU" */
  if (voice != NULL && module != NULL
      && !speechd_module_set_voice (module, voice->language, type, name,
                                    &error))
    {
      message = g_strdup_printf (_("Failed to save the voice: %s"),
                                 error->message);
      speech_page_show_message (page, GTK_MESSAGE_ERROR, message);
      g_free (message);
      g_error_free (error);
    }
  else if (voice != NULL && module != NULL)
    {
      page->reload_pending = TRUE;
      speech_page_queue_save (page);
    }

  g_free (type);
  g_free (language);
  g_free (module);
}



static void
speech_page_module_changed (GtkComboBox *combo,
                            SpeechPage  *page)
{
  const gchar *module = gtk_combo_box_get_active_id (combo);

  if (page->updating || module == NULL)
    return;

  if (*module == '\0')
    speechd_config_unset (page->config, "DefaultModule");
  else
    speechd_config_set (page->config, "DefaultModule", module);
  speech_page_queue_save (page);

  speech_page_load_voices (page);
  speech_page_update_voices (page);
}



/* The test sentence for a language code like "ru" or "en-US", or NULL */
static const gchar *
speech_page_get_test_text (const gchar *language)
{
  gsize len;
  guint i;

  if (language == NULL)
    return NULL;

  len = strcspn (language, "-_");
  for (i = 0; i < G_N_ELEMENTS (test_texts); i++)
    if (strlen (test_texts[i].language) == len
        && g_ascii_strncasecmp (test_texts[i].language, language, len) == 0)
      return test_texts[i].text;

  return NULL;
}



/* Shows the sentence of the language, unless the user has typed another text */
static void
speech_page_update_test_text (SpeechPage  *page,
                              const gchar *language)
{
  const gchar *text = speech_page_get_test_text (language);
  const gchar *current;

  if (text == NULL || page->test_entry == NULL)
    return;

  current = gtk_entry_get_text (GTK_ENTRY (page->test_entry));
  if (page->test_text != NULL && g_strcmp0 (current, page->test_text) != 0)
    return;

  gtk_entry_set_text (GTK_ENTRY (page->test_entry), text);
  page->test_text = text;
}



static void
speech_page_language_changed (GtkComboBox *combo,
                              SpeechPage  *page)
{
  GtkWidget *entry = gtk_bin_get_child (GTK_BIN (combo));
  gchar     *language;

  if (page->updating)
    return;

  language = g_strstrip (g_strdup (gtk_entry_get_text (GTK_ENTRY (entry))));

  /* a quote would end the value in speechd.conf */
  if (*language == '\0' || strchr (language, '"') != NULL)
    speechd_config_unset (page->config, "DefaultLanguage");
  else
    speechd_config_set (page->config, "DefaultLanguage", language);
  speech_page_queue_save (page);

  speech_page_update_test_text (page, *language != '\0' ? language : DEFAULT_LANGUAGE);

  page->updating = TRUE;
  speech_page_fill_modules (page);
  page->updating = FALSE;
  speech_page_update_voices (page);

  g_free (language);
}



static void
speech_page_choice_changed (GtkComboBox *combo,
                            SpeechPage  *page)
{
  const gchar *key = g_object_get_data (G_OBJECT (combo), "speechd-key");
  const gchar *id = gtk_combo_box_get_active_id (combo);

  if (page->updating || id == NULL)
    return;

  if (*id == '\0')
    speechd_config_unset (page->config, key);
  else
    speechd_config_set (page->config, key, id);
  speech_page_queue_save (page);
}



static void
speech_page_scale_changed (GtkRange   *range,
                           SpeechPage *page)
{
  const gchar *key = g_object_get_data (G_OBJECT (range), "speechd-key");

  if (page->updating)
    return;

  speechd_config_set_int (page->config, key,
                          (gint) gtk_range_get_value (range));
  speech_page_queue_save (page);
}



static gint
speech_page_choice_index (GtkWidget    *combo,
                          const Choice *choices,
                          guint         n_choices)
{
  const gchar *id = gtk_combo_box_get_active_id (GTK_COMBO_BOX (combo));
  guint        i;

  for (i = 0; id != NULL && i < n_choices; i++)
    if (strcmp (choices[i].id, id) == 0)
      return i;

  return 0;
}



/* The modules restart after the reload; ask for the list once they are up. */
static gboolean
speech_page_refill_modules (gpointer data)
{
  SpeechPage *page = data;

  page->refill_id = 0;

  if (page->connection != NULL)
    {
      spd_close (page->connection);
      page->connection = NULL;
    }

  page->updating = TRUE;
  speech_page_scan_modules (page);
  speech_page_fill_modules (page);
  speech_page_load_voices (page);
  {
    /* the voices of the new connection; refilling must not clear the language */
    GtkWidget *entry = gtk_bin_get_child (GTK_BIN (page->language_combo));
    gchar     *language = g_strdup (gtk_entry_get_text (GTK_ENTRY (entry)));

    speech_page_fill_languages (page);
    gtk_entry_set_text (GTK_ENTRY (entry), language);
    g_free (language);
  }
  page->updating = FALSE;
  speech_page_update_voices (page);

  return G_SOURCE_REMOVE;
}



/* The list of modules is read when the window opens; when the server was
 * restarted meanwhile (a module installed or removed), read it again once the
 * window is used */
static void
speech_page_window_active (GObject    *window,
                           GParamSpec *pspec,
                           gpointer    box)
{
  SpeechPage *page = g_object_get_data (G_OBJECT (box), "speech-page");
  struct stat st;

  if (page == NULL || page->config == NULL || page->refill_id != 0
      || !gtk_window_is_active (GTK_WINDOW (window))
      || !speech_page_server_stat (&st)
      || (st.st_dev == page->server_dev && st.st_ino == page->server_ino))
    return;

  page->server_dev = st.st_dev;
  page->server_ino = st.st_ino;
  page->refill_id = g_idle_add (speech_page_refill_modules, page);
}



static void
speech_page_hierarchy_changed (GtkWidget *box,
                               GtkWidget *previous,
                               gpointer   data)
{
  GtkWidget *toplevel = gtk_widget_get_toplevel (box);

  if (!GTK_IS_WINDOW (toplevel))
    return;
  g_signal_connect_object (toplevel, "notify::is-active",
                           G_CALLBACK (speech_page_window_active), box, 0);
  g_signal_handlers_disconnect_by_func (box, speech_page_hierarchy_changed, data);
}



/* A window does not get smaller by itself when a child is hidden */
static void
speech_page_hide_modules_bar (SpeechPage *page)
{
  GtkWidget *toplevel;
  gint       width;

  gtk_widget_hide (page->modules_bar);

  toplevel = gtk_widget_get_toplevel (page->modules_bar);
  if (GTK_IS_WINDOW (toplevel))
    {
      gtk_window_get_size (GTK_WINDOW (toplevel), &width, NULL);
      gtk_window_resize (GTK_WINDOW (toplevel), width, 1);
    }
}



static void
speech_page_modules_response (GtkInfoBar *bar,
                              gint        response,
                              SpeechPage *page)
{
  GError *error = NULL;
  gchar  *message;

  if (response != GTK_RESPONSE_ACCEPT)
    {
      speech_page_hide_modules_bar (page);
      return;
    }

  if (page->save_id != 0)
    {
      g_source_remove (page->save_id);
      page->save_id = 0;
    }

  if (!speechd_config_use_detected_modules (page->config, &error)
      || !speechd_config_save (page->config, &error)
      || !speechd_server_reload (&error))
    {
      message = g_strdup_printf (_("Failed to apply the settings: %s"),
                                 error->message);
      speech_page_show_message (page, GTK_MESSAGE_ERROR, message);
      g_free (message);
      g_error_free (error);
      return;
    }

  speech_page_hide_modules_bar (page);

  if (page->refill_id != 0)
    g_source_remove (page->refill_id);
  page->refill_id = g_timeout_add (REFILL_DELAY_MS,
                                   speech_page_refill_modules, page);
}



/* Speaks with the values shown in the dialog, set explicitly on our own
 * connection, so the test does not wait for the server reload. */
static void
speech_page_test_clicked (GtkButton  *button,
                          SpeechPage *page)
{
  const gchar *module;
  const gchar *language;
  const gchar *text;
  GtkWidget   *entry;
  gint         i;

  if (!speech_page_connect (page))
    return;

  module = gtk_combo_box_get_active_id (GTK_COMBO_BOX (page->module_combo));
  if (module != NULL && *module != '\0')
    spd_set_output_module (page->connection, module);

  entry = gtk_bin_get_child (GTK_BIN (page->language_combo));
  language = gtk_entry_get_text (GTK_ENTRY (entry));
  spd_set_language (page->connection,
                    *language != '\0' ? language : DEFAULT_LANGUAGE);

  i = speech_page_choice_index (page->voice_type_combo, voice_types,
                                G_N_ELEMENTS (voice_types));
  spd_set_voice_type (page->connection, SPD_MALE1 + i);
  if (gtk_widget_get_sensitive (page->voice_combo)
      && gtk_combo_box_get_active_id (GTK_COMBO_BOX (page->voice_combo)) != NULL)
    spd_set_synthesis_voice (page->connection,
                             gtk_combo_box_get_active_id (
                               GTK_COMBO_BOX (page->voice_combo)));

  spd_set_voice_rate (page->connection,
                      gtk_range_get_value (GTK_RANGE (page->rate_scale)));
  spd_set_voice_pitch (page->connection,
                       gtk_range_get_value (GTK_RANGE (page->pitch_scale)));
  spd_set_volume (page->connection,
                  gtk_range_get_value (GTK_RANGE (page->volume_scale)));

  i = speech_page_choice_index (page->punctuation_combo, punctuation_modes,
                                G_N_ELEMENTS (punctuation_modes));
  spd_set_punctuation (page->connection, punctuation_values[i]);

  i = speech_page_choice_index (page->capital_combo, capital_modes,
                                G_N_ELEMENTS (capital_modes));
  spd_set_capital_letters (page->connection, capital_values[i]);

  text = gtk_entry_get_text (GTK_ENTRY (page->test_entry));
  if (*text == '\0')
    return;

  spd_cancel (page->connection);
  if (spd_say (page->connection, SPD_TEXT, text) < 0)
    speech_page_show_message (page, GTK_MESSAGE_WARNING,
                              _("Speech Dispatcher did not accept the text."));
}



static void
speech_page_stop_clicked (GtkButton  *button,
                          SpeechPage *page)
{
  if (page->connection != NULL)
    spd_cancel (page->connection);
}



static void
speech_page_free (gpointer data)
{
  SpeechPage *page = data;

  if (page->save_id != 0)
    {
      g_source_remove (page->save_id);
      speech_page_save (page);
    }

  if (page->refill_id != 0)
    g_source_remove (page->refill_id);

  if (page->connection != NULL)
    spd_close (page->connection);

  if (page->voices != NULL)
    free_spd_voices (page->voices);
  g_strfreev (page->modules);
  g_hash_table_destroy (page->module_languages);
  g_ptr_array_free (page->languages, TRUE);

  speechd_config_free (page->config);
  g_free (page);
}



void
speech_page_flush (GtkWidget *widget)
{
  SpeechPage *page = g_object_get_data (G_OBJECT (widget), "speech-page");

  if (page != NULL && page->save_id != 0)
    {
      g_source_remove (page->save_id);
      speech_page_save (page);
    }
}



static void
speech_page_bind_choice (SpeechPage   *page,
                         GtkWidget    *combo,
                         const gchar  *key,
                         const Choice *choices,
                         guint         n_choices,
                         const gchar  *fallback)
{
  gchar *value;

  value = speechd_config_get (page->config, key);
  speech_page_combo_select (combo, choices, n_choices, value, fallback);
  g_free (value);

  g_object_set_data (G_OBJECT (combo), "speechd-key", (gpointer) key);
  g_signal_connect (combo, "changed",
                    G_CALLBACK (speech_page_choice_changed), page);
}



static GtkWidget *
speech_page_bind_scale (SpeechPage  *page,
                        const gchar *key,
                        gint         fallback,
                        const gchar *low,
                        const gchar *high)
{
  GtkWidget *scale;

  scale = speech_page_scale_new (speechd_config_get_int (page->config, key,
                                                         fallback),
                                 low, high);
  g_object_set_data (G_OBJECT (scale), "speechd-key", (gpointer) key);
  g_signal_connect (scale, "value-changed",
                    G_CALLBACK (speech_page_scale_changed), page);

  return scale;
}



GtkWidget *
speech_page_new (void)
{
  SpeechPage *page;
  GtkWidget  *box;
  GtkWidget  *grid;
  GtkWidget  *entry;
  GtkWidget  *button;
  GtkWidget  *hbox;
  GtkWidget  *label;
  GError     *error = NULL;
  gchar      *value;
  gchar      *text;

  page = g_new0 (SpeechPage, 1);
  page->module_languages =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                           (GDestroyNotify) g_ptr_array_unref);
  page->languages = g_ptr_array_new_with_free_func (g_free);

  if (label_size_group == NULL)
    label_size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width (GTK_CONTAINER (box), 12);
  g_object_set_data_full (G_OBJECT (box), "speech-page", page,
                          speech_page_free);
  g_signal_connect (box, "hierarchy-changed",
                    G_CALLBACK (speech_page_hierarchy_changed), NULL);

  page->info_bar = gtk_info_bar_new ();
  page->info_label = gtk_label_new (NULL);
  gtk_label_set_line_wrap (GTK_LABEL (page->info_label), TRUE);
  gtk_label_set_width_chars (GTK_LABEL (page->info_label), 40);
  gtk_label_set_xalign (GTK_LABEL (page->info_label), 0.0);
  gtk_container_add (GTK_CONTAINER (gtk_info_bar_get_content_area (
                       GTK_INFO_BAR (page->info_bar))), page->info_label);
  gtk_widget_show (page->info_label);
  gtk_widget_set_no_show_all (page->info_bar, TRUE);
  gtk_box_pack_start (GTK_BOX (box), page->info_bar, FALSE, TRUE, 0);

  page->config = speechd_config_load (&error);
  if (page->config == NULL)
    {
      text = g_strdup_printf (_("Failed to read the settings: %s"),
                              error->message);
      speech_page_show_message (page, GTK_MESSAGE_ERROR, text);
      g_free (text);
      g_error_free (error);
      return box;
    }

  /* AddModule lines hide every module that is not listed */
  page->modules_bar = gtk_info_bar_new_with_buttons (
    _("_Use All"), GTK_RESPONSE_ACCEPT, NULL);
  gtk_info_bar_set_message_type (GTK_INFO_BAR (page->modules_bar),
                                 GTK_MESSAGE_QUESTION);
  gtk_info_bar_set_show_close_button (GTK_INFO_BAR (page->modules_bar), TRUE);
  label = gtk_label_new (_("Your settings list the synthesizers to load, so "
                           "synthesizers installed later are not offered. "
                           "Use all installed synthesizers?"));
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  /* the window height is computed for the narrowest wrapping */
  gtk_label_set_width_chars (GTK_LABEL (label), 40);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_container_add (GTK_CONTAINER (gtk_info_bar_get_content_area (
                       GTK_INFO_BAR (page->modules_bar))), label);
  gtk_widget_show (label);
  gtk_widget_set_no_show_all (page->modules_bar, TRUE);
  gtk_box_pack_start (GTK_BOX (box), page->modules_bar, FALSE, TRUE, 0);
  g_signal_connect (page->modules_bar, "response",
                    G_CALLBACK (speech_page_modules_response), page);
  if (speechd_config_has_module_list (page->config))
    gtk_widget_show (page->modules_bar);

  page->updating = TRUE;

  /* Voice */
  grid = speech_page_frame_new (box, _("Voice"));

  /* the language first: it decides which synthesizers can speak */
  speech_page_scan_modules (page);

  page->language_combo = gtk_combo_box_text_new_with_entry ();
  entry = gtk_bin_get_child (GTK_BIN (page->language_combo));
  gtk_entry_set_placeholder_text (GTK_ENTRY (entry), DEFAULT_LANGUAGE);
  gtk_widget_set_tooltip_text (page->language_combo,
                               _("Language code like “en-US” or “ru”. Most "
                                 "applications set the language themselves."));
  speech_page_grid_add (grid, 0, _("_Language:"), page->language_combo);
  speech_page_fill_languages (page);
  value = speechd_config_get (page->config, "DefaultLanguage");
  gtk_entry_set_text (GTK_ENTRY (entry), value != NULL ? value : "");
  g_free (value);

  page->module_combo = gtk_combo_box_text_new ();
  gtk_widget_set_tooltip_text (page->module_combo,
                               _("The speech synthesizer used when an "
                                 "application does not ask for another one. "
                                 "Only the ones with a voice for the "
                                 "language are offered."));
  speech_page_grid_add (grid, 1, _("_Synthesizer:"), page->module_combo);
  speech_page_fill_modules (page);
  speech_page_load_voices (page);
  g_signal_connect (page->module_combo, "changed",
                    G_CALLBACK (speech_page_module_changed), page);
  g_signal_connect (page->language_combo, "changed",
                    G_CALLBACK (speech_page_language_changed), page);

  page->voice_combo = gtk_combo_box_text_new ();
  speech_page_grid_add (grid, 2, _("_Voice:"), page->voice_combo);
  g_signal_connect (page->voice_combo, "changed",
                    G_CALLBACK (speech_page_voice_changed), page);

  page->voice_type_combo = speech_page_combo_new (voice_types,
                                                  G_N_ELEMENTS (voice_types));
  gtk_widget_set_tooltip_text (page->voice_type_combo,
                               _("The synthesizer maps these types to its "
                                 "own voices"));
  speech_page_grid_add (grid, 3, _("Voice _type:"), page->voice_type_combo);
  page->voice_type_label =
    gtk_grid_get_child_at (GTK_GRID (grid), 0, 3);
  gtk_widget_set_no_show_all (page->voice_type_label, TRUE);
  gtk_widget_set_no_show_all (page->voice_type_combo, TRUE);
  speech_page_bind_choice (page, page->voice_type_combo, "DefaultVoiceType",
                           voice_types, G_N_ELEMENTS (voice_types), "MALE1");
  speech_page_update_voices (page);

  /* Speech */
  grid = speech_page_frame_new (box, _("Speech"));

  page->rate_scale = speech_page_bind_scale (page, "DefaultRate", 0,
                                             _("Slower"), _("Faster"));
  speech_page_grid_add (grid, 0, _("_Rate:"), page->rate_scale);

  page->pitch_scale = speech_page_bind_scale (page, "DefaultPitch", 0,
                                              _("Lower"), _("Higher"));
  speech_page_grid_add (grid, 1, _("_Pitch:"), page->pitch_scale);

  page->volume_scale = speech_page_bind_scale (page, "DefaultVolume", 0,
                                               _("Quieter"), _("Louder"));
  speech_page_grid_add (grid, 2, _("_Volume:"), page->volume_scale);

  /* Reading */
  grid = speech_page_frame_new (box, _("Reading"));

  page->punctuation_combo =
    speech_page_combo_new (punctuation_modes, G_N_ELEMENTS (punctuation_modes));
  speech_page_grid_add (grid, 0, _("P_unctuation:"), page->punctuation_combo);
  speech_page_bind_choice (page, page->punctuation_combo,
                           "DefaultPunctuationMode", punctuation_modes,
                           G_N_ELEMENTS (punctuation_modes), "none");

  page->capital_combo =
    speech_page_combo_new (capital_modes, G_N_ELEMENTS (capital_modes));
  speech_page_grid_add (grid, 1, _("_Capital letters:"), page->capital_combo);
  speech_page_bind_choice (page, page->capital_combo,
                           "DefaultCapLetRecognition", capital_modes,
                           G_N_ELEMENTS (capital_modes), "none");

  /* Audio */
  grid = speech_page_frame_new (box, _("Audio"));

  page->audio_combo =
    speech_page_combo_new (audio_methods, G_N_ELEMENTS (audio_methods));
  speech_page_grid_add (grid, 0, _("_Output:"), page->audio_combo);
  speech_page_bind_choice (page, page->audio_combo, "AudioOutputMethod",
                           audio_methods, G_N_ELEMENTS (audio_methods), "");

  /* Test */
  grid = speech_page_frame_new (box, _("Test"));

  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_grid_attach (GTK_GRID (grid), hbox, 0, 0, 1, 1);

  page->test_entry = gtk_entry_new ();
  gtk_entry_set_text (GTK_ENTRY (page->test_entry),
                      _("This is how your computer speaks."));
  value = speechd_config_get (page->config, "DefaultLanguage");
  page->test_text = _("This is how your computer speaks.");
  if (value != NULL && *value != '\0')
    speech_page_update_test_text (page, value);
  g_free (value);
  gtk_widget_set_hexpand (page->test_entry, TRUE);
  gtk_box_pack_start (GTK_BOX (hbox), page->test_entry, TRUE, TRUE, 0);

  button = gtk_button_new_with_mnemonic (_("_Speak"));
  g_signal_connect (button, "clicked",
                    G_CALLBACK (speech_page_test_clicked), page);
  g_signal_connect_swapped (page->test_entry, "activate",
                            G_CALLBACK (gtk_button_clicked), button);
  gtk_box_pack_start (GTK_BOX (hbox), button, FALSE, FALSE, 0);

  button = gtk_button_new_with_mnemonic (_("S_top"));
  g_signal_connect (button, "clicked",
                    G_CALLBACK (speech_page_stop_clicked), page);
  gtk_box_pack_start (GTK_BOX (hbox), button, FALSE, FALSE, 0);

  page->updating = FALSE;

  return box;
}

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

#ifndef __SPEECHD_CONFIG_H__
#define __SPEECHD_CONFIG_H__

#include <glib.h>

G_BEGIN_DECLS

/* Editor for the user's speechd.conf that keeps comments and layout.
 * Toolkit-independent, so it can be shared with other front ends. */
typedef struct _SpeechdConfig SpeechdConfig;

SpeechdConfig *speechd_config_load       (GError       **error);
void           speechd_config_free       (SpeechdConfig *config);

const gchar   *speechd_config_get_path   (SpeechdConfig *config);
gboolean       speechd_config_is_dirty   (SpeechdConfig *config);

gchar         *speechd_config_get        (SpeechdConfig *config,
                                          const gchar   *key);
gint           speechd_config_get_int    (SpeechdConfig *config,
                                          const gchar   *key,
                                          gint           fallback);
void           speechd_config_set        (SpeechdConfig *config,
                                          const gchar   *key,
                                          const gchar   *value);
void           speechd_config_set_int    (SpeechdConfig *config,
                                          const gchar   *key,
                                          gint           value);
void           speechd_config_unset      (SpeechdConfig *config,
                                          const gchar   *key);

/* AddModule lines turn off the detection of the installed modules */
gboolean       speechd_config_has_module_list      (SpeechdConfig *config);
gboolean       speechd_config_use_detected_modules (SpeechdConfig *config,
                                                    GError       **error);

gboolean       speechd_config_save       (SpeechdConfig *config,
                                          GError       **error);

gboolean       speechd_server_reload     (GError       **error);

/* How a module's configuration chooses the voice of a language */
typedef enum
{
  SPEECHD_VOICE_SETTING_NONE,      /* not at all: voice types only */
  SPEECHD_VOICE_SETTING_ADD_VOICE, /* generic module: AddVoice lines */
  SPEECHD_VOICE_SETTING_MODULE,    /* "<Module>Voice "lang" "voice"" */
} SpeechdVoiceSetting;

SpeechdVoiceSetting speechd_module_voice_setting (const gchar *module);
gchar         *speechd_module_get_voice  (const gchar   *module,
                                          const gchar   *language,
                                          const gchar   *type);
gboolean       speechd_module_set_voice  (const gchar   *module,
                                          const gchar   *language,
                                          const gchar   *type,
                                          const gchar   *voice,
                                          GError       **error);

G_END_DECLS

#endif /* !__SPEECHD_CONFIG_H__ */

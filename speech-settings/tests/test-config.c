/*
 * Copyright (C) 2026 Grigory Tuboltsev <gtubolcev@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/* Tests for the speechd.conf editor. They run in temporary XDG
 * directories, never on the real configuration. */

#include <gio/gio.h>

#include "src/speechd-config.h"

static const gchar *sample =
  "# The DefaultRate controls how fast the synthesizer is going to speak.\n"
  "# DefaultRate  0\n"
  "\n"
  "DefaultVolume 100\n"
  "# DefaultLanguage \"en-US\"\n"
  "BeginClient \"emacs:*\"\n"
  "    DefaultPunctuationMode \"some\"\n"
  "EndClient\n"
  "AddModule \"piper\" \"sd_generic\" \"piper.conf\"\n"
  "DefaultModule \"piper\"\n"
  "Include \"clients/*.conf\"\n";

static const gchar *expected =
  "# The DefaultRate controls how fast the synthesizer is going to speak.\n"
  "# DefaultRate  0\n"
  "DefaultRate -20\n"
  "\n"
  "DefaultVolume 80\n"
  "# DefaultLanguage \"en-US\"\n"
  "DefaultLanguage \"ru\"\n"
  "BeginClient \"emacs:*\"\n"
  "    DefaultPunctuationMode \"some\"\n"
  "EndClient\n"
  "AddModule \"piper\" \"sd_generic\" \"piper.conf\"\n"
  "# DefaultModule \"piper\"\n"
  "Include \"clients/*.conf\"\n"
  "DefaultPunctuationMode \"all\"\n";



/* G_TEST_OPTION_ISOLATE_DIRS gives every test its own empty, temporary
 * XDG directories, which GLib removes afterwards */
static gchar *
setup_config_home (void)
{
  return g_strdup (g_get_user_config_dir ());
}



static void
cleanup_config_home (gchar *dir)
{
  g_free (dir);
}



static void
test_edit (void)
{
  gchar         *dir = setup_config_home ();
  gchar         *conf_dir, *path, *contents, *value;
  SpeechdConfig *config;

  conf_dir = g_build_filename (dir, "speech-dispatcher", NULL);
  path = g_build_filename (conf_dir, "speechd.conf", NULL);
  g_mkdir_with_parents (conf_dir, 0700);
  g_assert_true (g_file_set_contents (path, sample, -1, NULL));

  config = speechd_config_load (NULL);
  g_assert_nonnull (config);
  g_assert_cmpstr (speechd_config_get_path (config), ==, path);

  /* keys are case-insensitive, like in Speech Dispatcher */
  value = speechd_config_get (config, "defaultmodule");
  g_assert_cmpstr (value, ==, "piper");
  g_free (value);
  g_assert_cmpint (speechd_config_get_int (config, "DefaultVolume", 0), ==, 100);
  g_assert_cmpint (speechd_config_get_int (config, "DefaultRate", 7), ==, 7);

  /* a client section is not a global default */
  g_assert_null (speechd_config_get (config, "DefaultPunctuationMode"));

  /* the same value changes nothing */
  speechd_config_set_int (config, "DefaultVolume", 100);
  g_assert_false (speechd_config_is_dirty (config));

  speechd_config_set_int (config, "DefaultRate", -20);
  speechd_config_set_int (config, "DefaultVolume", 80);
  speechd_config_set (config, "DefaultLanguage", "ru");
  speechd_config_set (config, "DefaultPunctuationMode", "all");
  speechd_config_unset (config, "DefaultModule");
  g_assert_true (speechd_config_is_dirty (config));
  g_assert_true (speechd_config_save (config, NULL));
  g_assert_false (speechd_config_is_dirty (config));
  speechd_config_free (config);

  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, expected);
  g_free (contents);

  config = speechd_config_load (NULL);
  value = speechd_config_get (config, "DefaultLanguage");
  g_assert_cmpstr (value, ==, "ru");
  g_free (value);
  g_assert_null (speechd_config_get (config, "DefaultModule"));
  speechd_config_free (config);

  g_free (path);
  g_free (conf_dir);
  cleanup_config_home (dir);
}



static void
test_no_user_file (void)
{
  gchar         *dir = setup_config_home ();
  gchar         *path;
  SpeechdConfig *config;

  config = speechd_config_load (NULL);
  g_assert_nonnull (config);

  /* nothing is written until something changes */
  path = g_build_filename (dir, "speech-dispatcher", "speechd.conf", NULL);
  g_assert_true (speechd_config_save (config, NULL));
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  speechd_config_set_int (config, "DefaultRate", 10);
  g_assert_true (speechd_config_save (config, NULL));
  g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));
  speechd_config_free (config);

  g_free (path);
  cleanup_config_home (dir);
}



static void
test_detect_modules (void)
{
  gchar         *dir = setup_config_home ();
  gchar         *conf_dir, *path, *contents;
  GError        *error = NULL;
  SpeechdConfig *config;

  conf_dir = g_build_filename (dir, "speech-dispatcher", "modules", NULL);
  g_mkdir_with_parents (conf_dir, 0700);
  path = g_build_filename (conf_dir, "piper.conf", NULL);
  g_assert_true (g_file_set_contents (path, "GenericExecuteSynth \"true\"\n",
                                      -1, NULL));
  g_free (path);
  path = g_build_filename (dir, "speech-dispatcher", "speechd.conf", NULL);
  g_assert_true (g_file_set_contents (path,
                                      "AddModule \"piper\" \"sd_generic\" \"piper.conf\"\n"
                                      "AddModule \"rhvoice\" \"sd_rhvoice\" \"rhvoice.conf\"\n"
                                      "DefaultModule \"piper\"\n"
                                      "LanguageDefaultModule \"ru\" \"piper\"\n",
                                      -1, NULL));

  config = speechd_config_load (NULL);
  g_assert_true (speechd_config_has_module_list (config));
  g_assert_true (speechd_config_use_detected_modules (config, &error));
  g_assert_no_error (error);
  g_assert_false (speechd_config_has_module_list (config));
  g_assert_true (speechd_config_save (config, NULL));
  speechd_config_free (config);

  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==,
                   "# AddModule \"piper\" \"sd_generic\" \"piper.conf\"\n"
                   "# AddModule \"rhvoice\" \"sd_rhvoice\" \"rhvoice.conf\"\n"
                   "DefaultModule \"piper-generic\"\n"
                   "LanguageDefaultModule \"ru\" \"piper-generic\"\n");
  g_free (contents);
  g_free (path);

  /* the old file stays, the copy is what Speech Dispatcher finds */
  path = g_build_filename (conf_dir, "piper-generic.conf", NULL);
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, "GenericExecuteSynth \"true\"\n");
  g_free (contents);
  g_free (path);

  /* a module under another name cannot be detected: nothing changes */
  path = g_build_filename (dir, "speech-dispatcher", "speechd.conf", NULL);
  g_assert_true (g_file_set_contents (path,
                                      "AddModule \"mbrola\" \"sd_espeak-ng\" \"mbrola.conf\"\n",
                                      -1, NULL));
  config = speechd_config_load (NULL);
  g_assert_false (speechd_config_use_detected_modules (config, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);
  g_assert_false (speechd_config_is_dirty (config));
  speechd_config_free (config);

  g_free (path);
  g_free (conf_dir);
  cleanup_config_home (dir);
}



int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

  g_test_add_func ("/config/edit", test_edit);
  g_test_add_func ("/config/no-user-file", test_no_user_file);
  g_test_add_func ("/config/detect-modules", test_detect_modules);

  return g_test_run ();
}

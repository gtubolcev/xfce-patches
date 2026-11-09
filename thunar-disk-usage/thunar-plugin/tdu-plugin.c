/*
 * Copyright (C) 2026 Grigory Tuboltsev
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <https://www.gnu.org/licenses/>.
 */


#include "tdu-page.h"
#include "tdu-provider.h"
#include "tdu-window.h"
#include "xfce-revision.h"

#include <glib/gi18n-lib.h>



G_MODULE_EXPORT void
thunar_extension_initialize (ThunarxProviderPlugin *plugin);
G_MODULE_EXPORT void
thunar_extension_shutdown (void);
G_MODULE_EXPORT void
thunar_extension_list_types (const GType **types,
                             gint         *n_types);



static GType type_list[1];



G_MODULE_EXPORT void
thunar_extension_initialize (ThunarxProviderPlugin *plugin)
{
  const gchar *mismatch;

  /* verify that the thunarx versions are compatible */
  mismatch = thunarx_check_version (THUNARX_MAJOR_VERSION, THUNARX_MINOR_VERSION, THUNARX_MICRO_VERSION);
  if (G_UNLIKELY (mismatch != NULL))
    {
      g_warning ("Version mismatch: %s", mismatch);
      return;
    }

  bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  g_debug ("Initializing thunar-disk-usage-plugin %s", VERSION_FULL);

  /* the chart and view types are registered statically, and a scan may
   * still run in a worker thread, so the module must never be unloaded */
  thunarx_provider_plugin_set_resident (plugin, TRUE);

  /* register the types provided by this plugin */
  tdu_page_register_type (plugin);
  tdu_window_register_type (plugin);
  tdu_provider_register_type (plugin);

  /* the default shortcut for the menu item, unless the user has set one */
  tdu_provider_add_default_accel ();

  /* setup the plugin provider type list */
  type_list[0] = TDU_TYPE_PROVIDER;
}



G_MODULE_EXPORT void
thunar_extension_shutdown (void)
{
  g_debug ("Shutting down thunar-disk-usage-plugin");
}



G_MODULE_EXPORT void
thunar_extension_list_types (const GType **types,
                             gint         *n_types)
{
  *types = type_list;
  *n_types = G_N_ELEMENTS (type_list);
}

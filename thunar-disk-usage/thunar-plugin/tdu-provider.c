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

#include <glib/gi18n-lib.h>



static void
tdu_provider_menu_provider_init (ThunarxMenuProviderIface *iface);
static void
tdu_provider_property_page_provider_init (ThunarxPropertyPageProviderIface *iface);
static GList *
tdu_provider_get_file_menu_items (ThunarxMenuProvider *menu_provider,
                                  GtkWidget           *window,
                                  GList               *files);
static GList *
tdu_provider_get_folder_menu_items (ThunarxMenuProvider *menu_provider,
                                    GtkWidget           *window,
                                    ThunarxFileInfo     *folder);
static GList *
tdu_provider_get_pages (ThunarxPropertyPageProvider *property_page_provider,
                        GList                       *files);



struct _TduProviderClass
{
  GObjectClass __parent__;
};

struct _TduProvider
{
  GObject __parent__;
};



THUNARX_DEFINE_TYPE_WITH_CODE (TduProvider,
                               tdu_provider,
                               G_TYPE_OBJECT,
                               THUNARX_IMPLEMENT_INTERFACE (THUNARX_TYPE_MENU_PROVIDER,
                                                            tdu_provider_menu_provider_init)
                               THUNARX_IMPLEMENT_INTERFACE (THUNARX_TYPE_PROPERTY_PAGE_PROVIDER,
                                                            tdu_provider_property_page_provider_init));



static void
tdu_provider_class_init (TduProviderClass *klass)
{
}



static void
tdu_provider_menu_provider_init (ThunarxMenuProviderIface *iface)
{
  iface->get_file_menu_items = tdu_provider_get_file_menu_items;
  iface->get_folder_menu_items = tdu_provider_get_folder_menu_items;
}



static void
tdu_provider_property_page_provider_init (ThunarxPropertyPageProviderIface *iface)
{
  iface->get_pages = tdu_provider_get_pages;
}



static void
tdu_provider_init (TduProvider *provider)
{
}



/**
 * tdu_provider_add_default_accel:
 *
 * Thunar's shortcut editor does not list the menu items of extensions,
 * but Thunar activates them by the accelerator of
 * "<Actions>/ThunarActions/<name>". Add Ctrl+Alt+U there (not Ctrl+Shift,
 * which often switches the keyboard layout; Ctrl+Alt+D is "Show desktop" in
 * xfwm4), unless the accel map already has an entry (also an empty one: the
 * user removed it).
 **/
void
tdu_provider_add_default_accel (void)
{
  const gchar *accel_path = "<Actions>/ThunarActions/" TDU_MENU_ITEM_NAME;

  if (!gtk_accel_map_lookup_entry (accel_path, NULL))
    gtk_accel_map_add_entry (accel_path, GDK_KEY_u, GDK_CONTROL_MASK | GDK_MOD1_MASK);
}



/**
 * tdu_provider_is_local_folder:
 * @file_info : a #ThunarxFileInfo.
 *
 * Returns: %TRUE if @file_info is a directory on a local file system,
 *          the only kind of folder the plugin scans.
 **/
gboolean
tdu_provider_is_local_folder (ThunarxFileInfo *file_info)
{
  gchar   *scheme;
  gboolean result;

  if (file_info == NULL || !thunarx_file_info_is_directory (file_info))
    return FALSE;

  scheme = thunarx_file_info_get_uri_scheme (file_info);
  result = g_strcmp0 (scheme, "file") == 0;
  g_free (scheme);

  return result;
}



static void
tdu_provider_activated (ThunarxMenuItem *item,
                        GtkWidget       *window)
{
  ThunarxFileInfo *folder;
  GFile           *location;

  folder = g_object_get_data (G_OBJECT (item), "tdu-folder");
  if (G_UNLIKELY (folder == NULL))
    return;

  location = thunarx_file_info_get_location (folder);
  tdu_window_show (location, window);
  g_object_unref (location);
}



static ThunarxMenuItem *
tdu_provider_menu_item_new (ThunarxFileInfo *folder,
                            GtkWidget       *window)
{
  ThunarxMenuItem *item;

  /* the same name in the file and the folder menu, so that one shortcut
   * works with and without a selected folder; a toolbar button activates
   * the item of the folder menu, i.e. shows the current folder */
  item = thunarx_menu_item_new (TDU_MENU_ITEM_NAME,
                                _("Show _Disk Usage"),
                                  _("Show what takes up the space in this folder"),
                                "thunar-disk-usage-plugin");
  g_object_set_data_full (G_OBJECT (item), "tdu-folder", g_object_ref (folder), g_object_unref);
  g_signal_connect (item, "activate", G_CALLBACK (tdu_provider_activated), window);

  return item;
}



static GList *
tdu_provider_get_file_menu_items (ThunarxMenuProvider *menu_provider,
                                  GtkWidget           *window,
                                  GList               *files)
{
  /* exactly one selected local folder */
  if (files == NULL || files->next != NULL || !tdu_provider_is_local_folder (files->data))
    return NULL;

  return g_list_append (NULL, tdu_provider_menu_item_new (files->data, window));
}



static GList *
tdu_provider_get_folder_menu_items (ThunarxMenuProvider *menu_provider,
                                    GtkWidget           *window,
                                    ThunarxFileInfo     *folder)
{
  if (!tdu_provider_is_local_folder (folder))
    return NULL;

  return g_list_append (NULL, tdu_provider_menu_item_new (folder, window));
}



static GList *
tdu_provider_get_pages (ThunarxPropertyPageProvider *property_page_provider,
                        GList                       *files)
{
  /* a page only for the properties of exactly one local folder */
  if (files == NULL || files->next != NULL || !tdu_provider_is_local_folder (files->data))
    return NULL;

  return g_list_append (NULL, tdu_page_new (files->data));
}

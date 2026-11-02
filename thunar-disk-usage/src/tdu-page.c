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


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "du-view.h"
#include "tdu-page.h"

#include <glib/gi18n-lib.h>



enum
{
  PROP_0,
  PROP_FILE,
};



static void
tdu_page_finalize (GObject *object);
static void
tdu_page_get_property (GObject    *object,
                       guint       prop_id,
                       GValue     *value,
                       GParamSpec *pspec);
static void
tdu_page_set_property (GObject      *object,
                       guint         prop_id,
                       const GValue *value,
                       GParamSpec   *pspec);
static void
tdu_page_file_changed (ThunarxFileInfo *file,
                       TduPage         *page);



struct _TduPageClass
{
  ThunarxPropertyPageClass __parent__;
};

struct _TduPage
{
  ThunarxPropertyPage __parent__;

  ThunarxFileInfo *file;
  GtkWidget       *view;
};



THUNARX_DEFINE_TYPE (TduPage, tdu_page, THUNARX_TYPE_PROPERTY_PAGE);



static void
tdu_page_class_init (TduPageClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = tdu_page_finalize;
  gobject_class->get_property = tdu_page_get_property;
  gobject_class->set_property = tdu_page_set_property;

  /**
   * TduPage:file:
   *
   * The folder shown in this page.
   **/
  g_object_class_install_property (gobject_class,
                                   PROP_FILE,
                                   g_param_spec_object ("file", NULL, NULL,
                                                        THUNARX_TYPE_FILE_INFO,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}



static void
tdu_page_init (TduPage *page)
{
  gtk_container_set_border_width (GTK_CONTAINER (page), 12);

  page->view = du_view_new ();
  gtk_container_add (GTK_CONTAINER (page), page->view);
  gtk_widget_show (page->view);
}



static void
tdu_page_finalize (GObject *object)
{
  TduPage *page = TDU_PAGE (object);

  if (page->file != NULL)
    {
      g_signal_handlers_disconnect_by_func (page->file, tdu_page_file_changed, page);
      g_object_unref (page->file);
    }

  G_OBJECT_CLASS (tdu_page_parent_class)->finalize (object);
}



static void
tdu_page_get_property (GObject    *object,
                       guint       prop_id,
                       GValue     *value,
                       GParamSpec *pspec)
{
  TduPage *page = TDU_PAGE (object);

  switch (prop_id)
    {
    case PROP_FILE:
      g_value_set_object (value, page->file);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}



static void
tdu_page_set_property (GObject      *object,
                       guint         prop_id,
                       const GValue *value,
                       GParamSpec   *pspec)
{
  TduPage         *page = TDU_PAGE (object);
  ThunarxFileInfo *file;

  switch (prop_id)
    {
    case PROP_FILE:
      file = g_value_get_object (value);
      if (page->file == file)
        break;

      if (page->file != NULL)
        {
          g_signal_handlers_disconnect_by_func (page->file, tdu_page_file_changed, page);
          g_object_unref (page->file);
        }

      page->file = file != NULL ? g_object_ref (file) : NULL;

      if (page->file != NULL)
        {
          g_signal_connect (page->file, "changed", G_CALLBACK (tdu_page_file_changed), page);
          tdu_page_file_changed (page->file, page);
        }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}



static void
tdu_page_file_changed (ThunarxFileInfo *file,
                       TduPage         *page)
{
  GFile *location;
  gchar *path;

  /* "changed" fires for any change of the folder; du_view_set_path()
   * ignores the same path, so this does not rescan each time */
  location = thunarx_file_info_get_location (file);
  path = g_file_get_path (location);
  if (path != NULL)
    du_view_set_path (DU_VIEW (page->view), path);

  g_free (path);
  g_object_unref (location);
}



/**
 * tdu_page_new:
 * @file : the #ThunarxFileInfo of a folder.
 *
 * Returns: a new "Disk Usage" property page for @file.
 **/
ThunarxPropertyPage *
tdu_page_new (ThunarxFileInfo *file)
{
  return g_object_new (TDU_TYPE_PAGE,
                       "label", _("Disk Usage"),
                       "file", file,
                       NULL);
}

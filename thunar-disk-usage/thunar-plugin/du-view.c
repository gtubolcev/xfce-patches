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

#include "du-rings-chart.h"
#include "du-scanner.h"
#include "du-view.h"

#include <glib/gi18n-lib.h>



/* how often the chart follows a running scan */
#define UPDATE_INTERVAL 200



static void
du_view_dispose (GObject *object);
static void
du_view_finalize (GObject *object);



struct _DuView
{
  GtkBox __parent__;

  gchar  *path;
  DuScan *scan;
  guint   generation;
  guint   update_id;

  GtkWidget *chart;
  GtkWidget *status;
};



G_DEFINE_FINAL_TYPE (DuView, du_view, GTK_TYPE_BOX)



static void
du_view_class_init (DuViewClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = du_view_dispose;
  gobject_class->finalize = du_view_finalize;
}



static void
du_view_item_activated (DuChart *chart,
                        DuNode  *node,
                        DuView  *view)
{
  du_chart_set_root (chart, node);
}



static void
du_view_trash_item (DuChart *chart,
                    DuNode  *node,
                    DuView  *view)
{
  GError *error = NULL;
  GFile  *file;
  gchar  *path;

  path = du_node_get_path (view->scan, node);
  file = g_file_new_for_path (path);

  if (g_file_trash (file, NULL, &error))
    du_view_rescan (view);
  else
    {
      GtkWidget *dialog;
      GtkWidget *toplevel = gtk_widget_get_toplevel (GTK_WIDGET (view));
      gchar     *display = g_filename_display_name (path);

      dialog = gtk_message_dialog_new (GTK_IS_WINDOW (toplevel) ? GTK_WINDOW (toplevel) : NULL,
                                       GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                       GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                       _("Failed to move \"%s\" to the trash"), display);
      gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s", error->message);
      gtk_dialog_run (GTK_DIALOG (dialog));
      gtk_widget_destroy (dialog);

      g_free (display);
      g_error_free (error);
    }

  g_object_unref (file);
  g_free (path);
}



static void
du_view_init (DuView *view)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (view), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (view), 6);

  view->chart = du_rings_chart_new ();
  gtk_widget_set_size_request (view->chart, 240, 240);
  g_signal_connect (view->chart, "item-activated", G_CALLBACK (du_view_item_activated), view);
  g_signal_connect (view->chart, "trash-item", G_CALLBACK (du_view_trash_item), view);
  gtk_box_pack_start (GTK_BOX (view), view->chart, TRUE, TRUE, 0);
  gtk_widget_show (view->chart);

  view->status = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (view->status), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (view->status), PANGO_ELLIPSIZE_END);
  gtk_box_pack_start (GTK_BOX (view), view->status, FALSE, FALSE, 0);
  gtk_widget_show (view->status);
}



static void
du_view_stop (DuView *view)
{
  if (view->update_id != 0)
    {
      g_source_remove (view->update_id);
      view->update_id = 0;
    }

  if (view->scan != NULL)
    {
      du_scan_cancel (view->scan);
      du_scan_unref (view->scan);
      view->scan = NULL;
    }
}



static void
du_view_dispose (GObject *object)
{
  DuView *view = DU_VIEW (object);

  /* stop the worker thread when the page or the window goes away */
  du_view_stop (view);

  G_OBJECT_CLASS (du_view_parent_class)->dispose (object);
}



static void
du_view_finalize (GObject *object)
{
  g_free (DU_VIEW (object)->path);

  G_OBJECT_CLASS (du_view_parent_class)->finalize (object);
}



static void
du_view_update_status (DuView *view)
{
  DuScanState state = du_scan_get_state (view->scan);
  guint64     bytes;
  guint64     items;
  guint       n_unreadable;
  gchar      *size;
  gchar      *count;
  gchar      *text;
  gchar      *unreadable = NULL;

  du_scan_get_progress (view->scan, &bytes, &items, &n_unreadable);
  size = g_format_size (bytes);
  /* with the thousands separator of the locale */
  count = g_strdup_printf ("%'" G_GUINT64_FORMAT, items);

  if (n_unreadable > 0)
    unreadable = g_strdup_printf (g_dngettext (GETTEXT_PACKAGE,
                                               "%u folder could not be read",
                                               "%u folders could not be read",
                                               n_unreadable),
                                  n_unreadable);

  switch (state)
    {
    case DU_SCAN_FAILED:
      text = g_strdup (du_scan_get_error (view->scan)->message);
      break;

    case DU_SCAN_RUNNING:
    case DU_SCAN_IDLE:
      /* TRANSLATORS: while scanning, e.g. "Scanning… 1.2 GB in 3,456 items" */
      text = g_strdup_printf (g_dngettext (GETTEXT_PACKAGE,
                                           "Scanning… %s in %s item",
                                           "Scanning… %s in %s items",
                                           items),
                              size, count);
      break;

    default:
      text = g_strdup_printf (g_dngettext (GETTEXT_PACKAGE,
                                           "%s in %s item",
                                           "%s in %s items",
                                           items),
                              size, count);
      break;
    }

  if (unreadable != NULL)
    {
      gchar *tmp = g_strdup_printf ("%s · %s", text, unreadable);
      g_free (text);
      text = tmp;
    }

  gtk_label_set_text (GTK_LABEL (view->status), text);

  g_free (unreadable);
  g_free (text);
  g_free (count);
  g_free (size);
}



static gboolean
du_view_update (gpointer user_data)
{
  DuView  *view = DU_VIEW (user_data);
  guint    generation = du_scan_get_generation (view->scan);
  gboolean running = du_scan_get_state (view->scan) == DU_SCAN_RUNNING;

  if (generation != view->generation)
    {
      view->generation = generation;
      du_chart_update (DU_CHART (view->chart));
    }

  du_view_update_status (view);

  if (running)
    return G_SOURCE_CONTINUE;

  view->update_id = 0;
  return G_SOURCE_REMOVE;
}



/**
 * du_view_set_path:
 * @view : a #DuView.
 * @path : the folder to scan, in the file system encoding.
 *
 * Starts scanning @path and shows the result as it grows.
 **/
void
du_view_set_path (DuView      *view,
                  const gchar *path)
{
  g_return_if_fail (DU_IS_VIEW (view));
  g_return_if_fail (path != NULL);

  if (g_strcmp0 (view->path, path) == 0 && view->scan != NULL)
    return;

  g_free (view->path);
  view->path = g_strdup (path);

  du_view_rescan (view);
}



/**
 * du_view_rescan:
 * @view : a #DuView.
 *
 * Scans the folder again, e.g. after something was moved to the trash.
 **/
void
du_view_rescan (DuView *view)
{
  g_return_if_fail (DU_IS_VIEW (view));

  if (view->path == NULL)
    return;

  du_view_stop (view);

  view->scan = du_scan_new (view->path);
  view->generation = 0;
  du_chart_set_scan (DU_CHART (view->chart), view->scan);
  du_scan_start (view->scan, NULL);

  view->update_id = g_timeout_add (UPDATE_INTERVAL, du_view_update, view);
  du_view_update_status (view);
}



GtkWidget *
du_view_new (void)
{
  return g_object_new (DU_TYPE_VIEW, NULL);
}

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

/* Shows the disk usage view of a folder in a window, without Thunar. */

#include "thunar-plugin/du-view.h"

int
main (int    argc,
      char **argv)
{
  GtkWidget *window;
  GtkWidget *view;

  gtk_init (&argc, &argv);

  if (argc != 2)
    {
      g_printerr ("Usage: %s PATH\n", argv[0]);
      return 2;
    }

  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title (GTK_WINDOW (window), argv[1]);
  gtk_window_set_default_size (GTK_WINDOW (window), 640, 520);
  gtk_container_set_border_width (GTK_CONTAINER (window), 12);
  g_signal_connect (window, "destroy", G_CALLBACK (gtk_main_quit), NULL);

  view = du_view_new ();
  gtk_container_add (GTK_CONTAINER (window), view);
  du_view_set_path (DU_VIEW (view), argv[1]);

  gtk_widget_show_all (window);
  gtk_main ();

  return 0;
}

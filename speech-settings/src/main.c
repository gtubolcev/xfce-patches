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

#include <gtk/gtk.h>
#include <libxfce4ui/libxfce4ui.h>
#include <libxfce4util/libxfce4util.h>

#ifdef HAVE_GTK_X11
#include <gdk/gdkx.h>
#include <gtk/gtkx.h>
#endif

#include "speech-page.h"

#ifdef HAVE_GTK_X11
static gint32 opt_socket_id = 0;
#endif
static gboolean opt_version = FALSE;

static GOptionEntry entries[] =
{
#ifdef HAVE_GTK_X11
  { "socket-id", 's', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_INT, &opt_socket_id,
    N_("Settings manager socket"), N_("SOCKET ID") },
#endif
  { "version", 'V', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_NONE, &opt_version,
    N_("Version information"), NULL },
  { NULL }
};



static void
dialog_response (GtkDialog *dialog,
                 gint       response,
                 GtkWidget *page)
{
  speech_page_flush (page);
  gtk_main_quit ();
}



gint
main (gint    argc,
      gchar **argv)
{
  GtkWidget *page;
  GtkWidget *dialog;
  GError    *error = NULL;

  xfce_textdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR, "UTF-8");

  if (!gtk_init_with_args (&argc, &argv, NULL, entries, PACKAGE, &error))
    {
      if (error != NULL)
        {
          g_print ("%s: %s.\n", PACKAGE, error->message);
          g_print (_("Type '%s --help' for usage."), PACKAGE);
          g_print ("\n");
          g_error_free (error);
        }
      else
        g_error ("Unable to open display.");

      return EXIT_FAILURE;
    }

  if (opt_version)
    {
      g_print ("%s %s (Xfce %s)\n", PACKAGE, PACKAGE_VERSION,
               xfce_version_string ());
      return EXIT_SUCCESS;
    }

  page = speech_page_new ();

#ifdef HAVE_GTK_X11
  if (opt_socket_id != 0 && GDK_IS_X11_DISPLAY (gdk_display_get_default ()))
    {
      GtkWidget *plug;

      plug = gtk_plug_new (opt_socket_id);
      g_signal_connect (plug, "delete-event", G_CALLBACK (gtk_main_quit), NULL);
      gtk_container_add (GTK_CONTAINER (plug), page);
      gtk_widget_show_all (plug);

      gdk_notify_startup_complete ();

      /* keep the dialog out of the session */
      gdk_x11_set_sm_client_id ("FAKE ID");

      gtk_main ();

      speech_page_flush (page);
      return EXIT_SUCCESS;
    }
#endif

  dialog = xfce_titled_dialog_new_with_mixed_buttons (_("Speech"), NULL, 0,
                                                      "window-close-symbolic",
                                                      _("_Close"),
                                                      GTK_RESPONSE_CLOSE,
                                                      NULL);
  gtk_window_set_icon_name (GTK_WINDOW (dialog), "audio-speakers");
  gtk_window_set_default_size (GTK_WINDOW (dialog), 560, -1);
  gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))),
                      page, TRUE, TRUE, 0);
  g_signal_connect (dialog, "response", G_CALLBACK (dialog_response), page);
  gtk_widget_show_all (dialog);

#ifdef HAVE_GTK_X11
  if (GDK_IS_X11_DISPLAY (gdk_display_get_default ()))
    gdk_x11_set_sm_client_id ("FAKE ID");
#endif

  gtk_main ();

  return EXIT_SUCCESS;
}

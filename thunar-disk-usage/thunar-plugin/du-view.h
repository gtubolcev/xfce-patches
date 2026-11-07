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

#ifndef __DU_VIEW_H__
#define __DU_VIEW_H__

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DU_TYPE_VIEW (du_view_get_type ())
G_DECLARE_FINAL_TYPE (DuView, du_view, DU, VIEW, GtkBox)

GtkWidget *
du_view_new (void);

void
du_view_set_path (DuView      *view,
                  const gchar *path);

void
du_view_rescan (DuView *view);

G_END_DECLS

#endif /* !__DU_VIEW_H__ */

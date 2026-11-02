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


#ifndef __TDU_WINDOW_H__
#define __TDU_WINDOW_H__

#include <thunarx/thunarx.h>

G_BEGIN_DECLS

typedef struct _TduWindowClass TduWindowClass;
typedef struct _TduWindow      TduWindow;

#define TDU_TYPE_WINDOW (tdu_window_get_type ())
#define TDU_WINDOW(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), TDU_TYPE_WINDOW, TduWindow))
#define TDU_IS_WINDOW(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), TDU_TYPE_WINDOW))

GType
tdu_window_get_type (void) G_GNUC_CONST;
void
tdu_window_register_type (ThunarxProviderPlugin *plugin);

void
tdu_window_show (GFile     *location,
                 GtkWidget *parent);

G_END_DECLS

#endif /* !__TDU_WINDOW_H__ */

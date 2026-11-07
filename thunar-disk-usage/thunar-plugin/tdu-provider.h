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


#ifndef __TDU_PROVIDER_H__
#define __TDU_PROVIDER_H__

#include <thunarx/thunarx.h>

G_BEGIN_DECLS

/* name of the menu item; Thunar binds "<Actions>/ThunarActions/<name>",
 * and offers the items whose name starts with "uca-action" (meant for its
 * custom actions) as buttons in its toolbar editor */
#define TDU_MENU_ITEM_NAME "uca-action-tdu-show-disk-usage"

typedef struct _TduProviderClass TduProviderClass;
typedef struct _TduProvider      TduProvider;

#define TDU_TYPE_PROVIDER (tdu_provider_get_type ())
#define TDU_PROVIDER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), TDU_TYPE_PROVIDER, TduProvider))
#define TDU_IS_PROVIDER(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), TDU_TYPE_PROVIDER))

GType
tdu_provider_get_type (void) G_GNUC_CONST;
void
tdu_provider_register_type (ThunarxProviderPlugin *plugin);

void
tdu_provider_add_default_accel (void);

gboolean
tdu_provider_is_local_folder (ThunarxFileInfo *file_info);

G_END_DECLS

#endif /* !__TDU_PROVIDER_H__ */

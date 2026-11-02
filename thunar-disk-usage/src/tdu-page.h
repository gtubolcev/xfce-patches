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


#ifndef __TDU_PAGE_H__
#define __TDU_PAGE_H__

#include <thunarx/thunarx.h>

G_BEGIN_DECLS

typedef struct _TduPageClass TduPageClass;
typedef struct _TduPage      TduPage;

#define TDU_TYPE_PAGE (tdu_page_get_type ())
#define TDU_PAGE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), TDU_TYPE_PAGE, TduPage))
#define TDU_IS_PAGE(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), TDU_TYPE_PAGE))

GType
tdu_page_get_type (void) G_GNUC_CONST;
void
tdu_page_register_type (ThunarxProviderPlugin *plugin);

ThunarxPropertyPage *
tdu_page_new (ThunarxFileInfo *file) G_GNUC_MALLOC;

G_END_DECLS

#endif /* !__TDU_PAGE_H__ */

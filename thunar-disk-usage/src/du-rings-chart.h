/*
 * Copyright (C) 2006, 2007, 2008  Igalia
 * Copyright (C) 2013  Stefano Facchini <stefano.facchini@gmail.com>
 * Copyright (C) 2026 Grigory Tuboltsev
 *
 * Ported to C and GTK 3 from baobab-ringschart.vala of Baobab 49.1.
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

#ifndef __DU_RINGS_CHART_H__
#define __DU_RINGS_CHART_H__

#include "du-chart.h"

G_BEGIN_DECLS

#define DU_TYPE_RINGS_CHART (du_rings_chart_get_type ())
G_DECLARE_FINAL_TYPE (DuRingsChart, du_rings_chart, DU, RINGS_CHART, DuChart)

GtkWidget *
du_rings_chart_new (void);

G_END_DECLS

#endif /* !__DU_RINGS_CHART_H__ */

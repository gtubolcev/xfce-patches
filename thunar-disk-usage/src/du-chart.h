/*
 * Copyright (C) 2006, 2007, 2008  Igalia
 * Copyright (C) 2013  Stefano Facchini <stefano.facchini@gmail.com>
 * Copyright (C) 2026 Grigory Tuboltsev
 *
 * Ported to C and GTK 3 from baobab-chart.vala of Baobab 49.1.
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

#ifndef __DU_CHART_H__
#define __DU_CHART_H__

#include "du-scanner.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DU_CHART_MAX_DEPTH 5
#define DU_CHART_MIN_DEPTH 1

typedef struct _DuChartItem DuChartItem;

/**
 * DuChartItem:
 * @node                 : the folder; valid as long as the chart's scan.
 * @name                 : the display name of the folder.
 * @size                 : the size of the folder when the items were built.
 * @has_children         : the folder has sub folders.
 * @depth                : 0 for the root of the chart.
 * @rel_start            : start within the parent, in percent.
 * @rel_size             : size relative to the parent, in percent.
 * @visible              : the item is large enough to be drawn.
 * @has_visible_children : at least one child is drawn.
 * @rect                 : the bounding box, for tooltips.
 * @parent               : the item of the parent folder, %NULL for the root.
 *
 * One segment of a chart. Subclasses extend it, see #DuChartClass.item_size.
 **/
struct _DuChartItem
{
  DuNode      *node;
  gchar       *name;
  guint64      size;
  gboolean     has_children;
  guint        depth;
  gdouble      rel_start;
  gdouble      rel_size;
  gboolean     visible;
  gboolean     has_visible_children;
  GdkRectangle rect;
  DuChartItem *parent;
};

#define DU_TYPE_CHART (du_chart_get_type ())
G_DECLARE_DERIVABLE_TYPE (DuChart, du_chart, DU, CHART, GtkDrawingArea)

struct _DuChartClass
{
  GtkDrawingAreaClass __parent__;

  /* size of the item struct of the subclass */
  gsize item_size;

  void (*post_draw) (DuChart *chart,
                     cairo_t *cr);
  void (*draw_item) (DuChart     *chart,
                     cairo_t     *cr,
                     DuChartItem *item,
                     gboolean     highlighted);
  void (*calculate_item_geometry) (DuChart     *chart,
                                   DuChartItem *item);
  gboolean (*is_point_over_item) (DuChart     *chart,
                                  DuChartItem *item,
                                  gdouble      x,
                                  gdouble      y);
  void (*get_item_rectangle) (DuChart     *chart,
                              DuChartItem *item);
  gboolean (*can_zoom_in) (DuChart *chart);
  gboolean (*can_zoom_out) (DuChart *chart);

  /* signals */
  void (*item_activated) (DuChart *chart,
                          gpointer node);
  void (*trash_item) (DuChart *chart,
                      gpointer node);
};

void
du_chart_set_scan (DuChart *chart,
                   DuScan  *scan);
DuScan *
du_chart_get_scan (DuChart *chart);

void
du_chart_set_root (DuChart *chart,
                   DuNode  *root);
DuNode *
du_chart_get_root (DuChart *chart);

void
du_chart_update (DuChart *chart);

void
du_chart_set_max_depth (DuChart *chart,
                        guint    max_depth);
guint
du_chart_get_max_depth (DuChart *chart);

DuChartItem *
du_chart_get_highlighted_item (DuChart *chart);

void
du_chart_get_item_color (DuChart *chart,
                         gdouble  rel_position,
                         guint    depth,
                         gboolean highlighted,
                         GdkRGBA *color);

void
du_chart_move_up_root (DuChart *chart);
void
du_chart_zoom_in (DuChart *chart);
void
du_chart_zoom_out (DuChart *chart);

G_END_DECLS

#endif /* !__DU_CHART_H__ */

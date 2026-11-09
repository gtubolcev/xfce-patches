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
 *
 * Authors of the original code:
 *   Felipe Erias <femorandeira@igalia.com>
 *   Pablo Santamaria <psantamaria@igalia.com>
 *   Jacobo Aragunde <jaragunde@igalia.com>
 *   Eduardo Lima <elima@igalia.com>
 *   Mario Sanchez <msanchez@igalia.com>
 *   Miguel Gomez <magomez@igalia.com>
 *   Henrique Ferreiro <hferreiro@igalia.com>
 *   Alejandro Pinheiro <apinheiro@igalia.com>
 *   Carlos Sanmartin <csanmartin@igalia.com>
 *   Alejandro Garcia <alex@igalia.com>
 */


#include "du-rings-chart.h"

#include <math.h>



#define ITEM_BORDER_WIDTH 1
#define ITEM_MIN_ANGLE 0.03
#define EDGE_ANGLE 0.004

/* space around the outer ring, for the "continued" marks */
#define CHART_PADDING 6

/* twice the GTK tooltip timeout */
#define SUBTIP_TIMEOUT 1000

/* the labels of the sub folders (Baobab's .subfolder-tip CSS) */
#define SUBTIP_PADDING_X 10
#define SUBTIP_PADDING_Y 6
#define SUBTIP_RADIUS 9



typedef struct
{
  DuChartItem __parent__;

  gdouble  min_radius;
  gdouble  max_radius;
  gdouble  start_angle;
  gdouble  angle;
  gboolean continued;
} DuRingsChartItem;

struct _DuRingsChart
{
  DuChart __parent__;

  guint      tips_timeout_id;
  gboolean   drawing_subtips;
  GPtrArray *subtip_items;
};



static void
du_rings_chart_finalize (GObject *object);
static void
du_rings_chart_post_draw (DuChart *chart,
                          cairo_t *cr);
static void
du_rings_chart_draw_item (DuChart     *chart,
                          cairo_t     *cr,
                          DuChartItem *item,
                          gboolean     highlighted);
static void
du_rings_chart_calculate_item_geometry (DuChart     *chart,
                                        DuChartItem *item);
static gboolean
du_rings_chart_is_point_over_item (DuChart     *chart,
                                   DuChartItem *item,
                                   gdouble      x,
                                   gdouble      y);
static void
du_rings_chart_get_item_rectangle (DuChart     *chart,
                                   DuChartItem *item);
static gboolean
du_rings_chart_can_zoom_in (DuChart *chart);
static gboolean
du_rings_chart_can_zoom_out (DuChart *chart);



G_DEFINE_FINAL_TYPE (DuRingsChart, du_rings_chart, DU_TYPE_CHART)



static void
du_rings_chart_class_init (DuRingsChartClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  DuChartClass *chart_class = DU_CHART_CLASS (klass);

  gobject_class->finalize = du_rings_chart_finalize;

  chart_class->item_size = sizeof (DuRingsChartItem);
  chart_class->post_draw = du_rings_chart_post_draw;
  chart_class->draw_item = du_rings_chart_draw_item;
  chart_class->calculate_item_geometry = du_rings_chart_calculate_item_geometry;
  chart_class->is_point_over_item = du_rings_chart_is_point_over_item;
  chart_class->get_item_rectangle = du_rings_chart_get_item_rectangle;
  chart_class->can_zoom_in = du_rings_chart_can_zoom_in;
  chart_class->can_zoom_out = du_rings_chart_can_zoom_out;

  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "ringschart");
}



static gboolean
du_rings_chart_subtips_timeout (gpointer user_data)
{
  DuRingsChart *self = DU_RINGS_CHART (user_data);

  self->drawing_subtips = TRUE;
  self->tips_timeout_id = 0;
  gtk_widget_queue_draw (GTK_WIDGET (self));

  return G_SOURCE_REMOVE;
}



static void
du_rings_chart_subtips_update (DuRingsChart *self)
{
  if (self->drawing_subtips)
    gtk_widget_queue_draw (GTK_WIDGET (self));
  self->drawing_subtips = FALSE;

  if (self->tips_timeout_id != 0)
    {
      g_source_remove (self->tips_timeout_id);
      self->tips_timeout_id = 0;
    }

  g_ptr_array_set_size (self->subtip_items, 0);

  if (du_chart_get_highlighted_item (DU_CHART (self)) != NULL)
    self->tips_timeout_id = g_timeout_add (SUBTIP_TIMEOUT, du_rings_chart_subtips_timeout, self);
}



static void
du_rings_chart_init (DuRingsChart *self)
{
  self->subtip_items = g_ptr_array_new ();

  g_signal_connect (self, "notify::max-depth", G_CALLBACK (du_rings_chart_subtips_update), NULL);
  g_signal_connect (self, "notify::highlighted-item", G_CALLBACK (du_rings_chart_subtips_update), NULL);
  g_signal_connect (self, "notify::root", G_CALLBACK (du_rings_chart_subtips_update), NULL);
}



static void
du_rings_chart_finalize (GObject *object)
{
  DuRingsChart *self = DU_RINGS_CHART (object);

  if (self->tips_timeout_id != 0)
    g_source_remove (self->tips_timeout_id);
  g_ptr_array_unref (self->subtip_items);

  G_OBJECT_CLASS (du_rings_chart_parent_class)->finalize (object);
}



static void
du_rings_chart_rounded_rectangle (cairo_t *cr,
                                  gdouble  x,
                                  gdouble  y,
                                  gdouble  width,
                                  gdouble  height,
                                  gdouble  radius)
{
  radius = MIN (radius, MIN (width, height) / 2);

  cairo_new_sub_path (cr);
  cairo_arc (cr, x + width - radius, y + radius, radius, -G_PI / 2, 0);
  cairo_arc (cr, x + width - radius, y + height - radius, radius, 0, G_PI / 2);
  cairo_arc (cr, x + radius, y + height - radius, radius, G_PI / 2, G_PI);
  cairo_arc (cr, x + radius, y + radius, radius, G_PI, 3 * G_PI / 2);
  cairo_close_path (cr);
}



static PangoLayout *
du_rings_chart_create_small_layout (GtkWidget   *widget,
                                    const gchar *text)
{
  PangoLayout *layout;
  gchar       *markup;

  markup = g_markup_printf_escaped ("<span size=\"small\">%s</span>", text);
  layout = gtk_widget_create_pango_layout (widget, NULL);
  pango_layout_set_markup (layout, markup, -1);
  pango_layout_set_indent (layout, 0);
  pango_layout_set_spacing (layout, 0);
  g_free (markup);

  return layout;
}



static void
du_rings_chart_post_draw (DuChart *chart,
                          cairo_t *cr)
{
  DuRingsChart *self = DU_RINGS_CHART (chart);
  GdkRectangle  last_rect = { 0, 0, 0, 0 };
  gint          width;
  gint          height;
  gdouble       q_width;
  gdouble       q_height;
  gdouble       q_angle;
  guint         n;

  if (!self->drawing_subtips)
    return;

  width = gtk_widget_get_allocated_width (GTK_WIDGET (chart));
  height = gtk_widget_get_allocated_height (GTK_WIDGET (chart));

  q_width = width / 2;
  q_height = height / 2;
  q_angle = atan2 (q_height, q_width);

  for (n = 0; n < self->subtip_items->len; n++)
    {
      DuRingsChartItem *ringsitem = g_ptr_array_index (self->subtip_items, n);
      DuChartItem      *item = (DuChartItem *) ringsitem;
      PangoLayout      *layout;
      PangoRectangle    layout_rect;
      GdkRectangle      rect;
      gdouble           middle_angle;
      gdouble           middle_angle_n;
      gdouble           middle_radius;
      gdouble           sector_center_x;
      gdouble           sector_center_y;
      gdouble           tip_x;
      gdouble           tip_y;
      gdouble           rx, ry, rw, rh;
      gdouble           a;
      gint              i;

      /* get the middle angle */
      middle_angle = ringsitem->start_angle + ringsitem->angle / 2;

      /* normalize the middle angle (take it to the first quadrant) */
      middle_angle_n = middle_angle;
      while (middle_angle_n > G_PI / 2)
        middle_angle_n -= G_PI;
      middle_angle_n = fabs (middle_angle_n);

      /* get the pango layout and its enclosing rectangle */
      layout = du_rings_chart_create_small_layout (GTK_WIDGET (chart), item->name);
      pango_layout_set_width (layout, PANGO_SCALE * q_width / 2);
      pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
      pango_layout_get_pixel_extents (layout, NULL, &layout_rect);

      /* get the center point of the tooltip rectangle */
      if (middle_angle_n < q_angle)
        {
          tip_x = q_width - layout_rect.width / 2 - 2 * SUBTIP_PADDING_X;
          tip_y = tan (middle_angle_n) * tip_x;
        }
      else
        {
          tip_y = q_height - layout_rect.height / 2 - 2 * SUBTIP_PADDING_Y;
          tip_x = tip_y / tan (middle_angle_n);
        }

      /* get the tooltip rectangle */
      rx = q_width + tip_x - layout_rect.width / 2 - SUBTIP_PADDING_X;
      ry = q_height + tip_y - layout_rect.height / 2 - SUBTIP_PADDING_Y;
      rw = layout_rect.width + 2 * SUBTIP_PADDING_X;
      rh = layout_rect.height + 2 * SUBTIP_PADDING_Y;

      /* translate tooltip rectangle and edge angles to the original quadrant */
      for (a = middle_angle, i = 0; a > G_PI / 2; a -= G_PI / 2, i++)
        {
          if (i % 2 == 0)
            rx = width - rx - rw;
          else
            ry = height - ry - rh;
        }

      /* get the GdkRectangle of the tooltip (with a little padding) */
      rect.x = rx - 1;
      rect.y = ry - 1;
      rect.width = rw + 2;
      rect.height = rh + 2;

      /* check if tooltip overlaps */
      if (!gdk_rectangle_intersect (&rect, &last_rect, NULL))
        {
          last_rect = rect;

          /* avoid blurred lines */
          rx = floor (rx);
          ry = floor (ry);

          middle_radius = ringsitem->min_radius + (ringsitem->max_radius - ringsitem->min_radius) / 2;
          sector_center_x = q_width + middle_radius * cos (middle_angle);
          sector_center_y = q_height + middle_radius * sin (middle_angle);

          /* draw line from sector center to tooltip center */
          cairo_save (cr);

          /* clip to avoid drawing inside tooltip area (tooltip background
           * is transparent) */
          cairo_rectangle (cr, 0, 0, width, height);
          cairo_rectangle (cr, rx + rw, ry, -rw, rh);
          cairo_clip (cr);

          cairo_set_line_width (cr, 1);
          cairo_move_to (cr, sector_center_x, sector_center_y);
          cairo_set_source_rgba (cr, 0, 0, 0, 0.8);
          cairo_line_to (cr, rx + rw / 2, ry + rh / 2);
          cairo_stroke (cr);

          cairo_restore (cr);

          /* draw a tiny circle in sector center */
          cairo_set_source_rgba (cr, 0, 0, 0, 0.8);
          cairo_arc (cr, sector_center_x, sector_center_y, 1.0, 0, 2 * G_PI);
          cairo_stroke (cr);

          /* draw tooltip box */
          du_rings_chart_rounded_rectangle (cr, rx, ry, rw, rh, SUBTIP_RADIUS);
          cairo_fill (cr);

          cairo_set_source_rgb (cr, 1, 1, 1);
          cairo_move_to (cr, rx + SUBTIP_PADDING_X, ry + SUBTIP_PADDING_Y);
          pango_cairo_show_layout (cr, layout);
        }

      g_object_unref (layout);
    }

  /* collected again by the next draw */
  g_ptr_array_set_size (self->subtip_items, 0);
}



static void
du_rings_chart_lookup_color (GtkStyleContext *context,
                             const gchar     *name,
                             const gchar     *fallback,
                             GdkRGBA         *color)
{
  if (!gtk_style_context_lookup_color (context, name, color))
    gdk_rgba_parse (color, fallback);
}



static void
du_rings_chart_draw_item (DuChart     *chart,
                          cairo_t     *cr,
                          DuChartItem *item,
                          gboolean     highlighted)
{
  DuRingsChart     *self = DU_RINGS_CHART (chart);
  DuRingsChartItem *ringsitem = (DuRingsChartItem *) item;
  DuChartItem      *highlighted_item;
  GtkStyleContext  *context;
  GdkRGBA           border_color;
  GdkRGBA           bg_color;
  GdkRGBA           fill_color;
  gdouble           center_x;
  gdouble           center_y;
  gdouble           final_angle;

  if (self->drawing_subtips)
    {
      highlighted_item = du_chart_get_highlighted_item (chart);
      if (highlighted_item != NULL && item->parent == highlighted_item)
        g_ptr_array_add (self->subtip_items, item);
    }

  cairo_set_line_width (cr, ITEM_BORDER_WIDTH);

  context = gtk_widget_get_style_context (GTK_WIDGET (chart));
  du_rings_chart_lookup_color (context, "theme_bg_color", "#ffffff", &bg_color);
  du_rings_chart_lookup_color (context, "borders", "#b6b6b3", &border_color);

  center_x = gtk_widget_get_allocated_width (GTK_WIDGET (chart)) / 2;
  center_y = gtk_widget_get_allocated_height (GTK_WIDGET (chart)) / 2;
  final_angle = ringsitem->start_angle + ringsitem->angle;

  if (item->depth == 0)
    {
      PangoLayout   *layout;
      PangoRectangle layout_rect;
      gchar         *size;

      /* draw a label with the size of the folder in the middle of the
       * central disk */
      size = g_format_size (item->size);
      layout = du_rings_chart_create_small_layout (GTK_WIDGET (chart), size);
      pango_layout_get_pixel_extents (layout, NULL, &layout_rect);

      if (layout_rect.width < 2 * ringsitem->max_radius)
        gtk_render_layout (context, cr,
                           center_x - layout_rect.width / 2,
                           center_y - layout_rect.height / 2,
                           layout);

      g_object_unref (layout);
      g_free (size);

      cairo_new_path (cr);
      cairo_arc (cr, center_x, center_y, ringsitem->max_radius + 1, 0, 2 * G_PI);
      gdk_cairo_set_source_rgba (cr, &border_color);
      cairo_stroke (cr);
    }
  else
    {
      du_chart_get_item_color (chart, ringsitem->start_angle / G_PI * 99, item->depth, highlighted, &fill_color);

      cairo_arc (cr, center_x, center_y, ringsitem->min_radius, ringsitem->start_angle, final_angle);
      cairo_arc_negative (cr, center_x, center_y, ringsitem->max_radius, final_angle, ringsitem->start_angle);
      cairo_close_path (cr);

      gdk_cairo_set_source_rgba (cr, &fill_color);
      cairo_fill_preserve (cr);
      gdk_cairo_set_source_rgba (cr, &bg_color);
      cairo_stroke (cr);

      if (ringsitem->continued)
        {
          gdk_cairo_set_source_rgba (cr, &border_color);
          cairo_set_line_width (cr, 3);
          cairo_arc (cr, center_x, center_y, ringsitem->max_radius + 4,
                     ringsitem->start_angle + EDGE_ANGLE, final_angle - EDGE_ANGLE);
          cairo_stroke (cr);
        }
    }
}



static void
du_rings_chart_calculate_item_geometry (DuChart     *chart,
                                        DuChartItem *item)
{
  DuRingsChartItem *ringsitem = (DuRingsChartItem *) item;
  DuRingsChartItem *parent;
  guint             max_depth = du_chart_get_max_depth (chart);
  gint              width;
  gint              height;
  gint              max_radius;
  gint              thickness;

  ringsitem->continued = FALSE;
  item->visible = FALSE;

  width = gtk_widget_get_allocated_width (GTK_WIDGET (chart));
  height = gtk_widget_get_allocated_height (GTK_WIDGET (chart));
  max_radius = MIN (width / 2, height / 2) - CHART_PADDING;
  thickness = MAX (max_radius, 0) / (max_depth + 1);

  if (item->parent == NULL)
    {
      ringsitem->min_radius = 0;
      ringsitem->max_radius = thickness;
      ringsitem->start_angle = 0;
      ringsitem->angle = 2 * G_PI;
    }
  else
    {
      parent = (DuRingsChartItem *) item->parent;
      ringsitem->min_radius = item->depth * thickness;
      if (item->depth > max_depth)
        return;
      ringsitem->max_radius = ringsitem->min_radius + thickness;

      ringsitem->angle = parent->angle * item->rel_size / 100;
      if (ringsitem->angle < ITEM_MIN_ANGLE)
        return;

      ringsitem->start_angle = parent->start_angle + parent->angle * item->rel_start / 100;
      ringsitem->continued = item->has_children && item->depth == max_depth;
      item->parent->has_visible_children = TRUE;
    }

  item->visible = TRUE;
  du_rings_chart_get_item_rectangle (chart, item);
}



static void
du_rings_chart_get_point_min_rect (gdouble       cx,
                                   gdouble       cy,
                                   gdouble       radius,
                                   gdouble       angle,
                                   GdkRectangle *r)
{
  gdouble x = cx + cos (angle) * radius;
  gdouble y = cy + sin (angle) * radius;

  r->x = MIN (r->x, (gint) x);
  r->y = MIN (r->y, (gint) y);
  r->width = MAX (r->width, (gint) x);
  r->height = MAX (r->height, (gint) y);
}



static void
du_rings_chart_get_item_rectangle (DuChart     *chart,
                                   DuChartItem *item)
{
  DuRingsChartItem *ringsitem = (DuRingsChartItem *) item;
  GdkRectangle      rect;
  gdouble           cx, cy, r1, r2, a1, a2;
  gint              width;
  gint              height;

  width = gtk_widget_get_allocated_width (GTK_WIDGET (chart));
  height = gtk_widget_get_allocated_height (GTK_WIDGET (chart));
  cx = width / 2;
  cy = height / 2;
  r1 = ringsitem->min_radius;
  r2 = ringsitem->max_radius;
  a1 = ringsitem->start_angle;
  a2 = ringsitem->start_angle + ringsitem->angle;

  rect.x = width;
  rect.y = height;
  rect.width = 0;
  rect.height = 0;

  du_rings_chart_get_point_min_rect (cx, cy, r1, a1, &rect);
  du_rings_chart_get_point_min_rect (cx, cy, r2, a1, &rect);
  du_rings_chart_get_point_min_rect (cx, cy, r1, a2, &rect);
  du_rings_chart_get_point_min_rect (cx, cy, r2, a2, &rect);

  if (a1 <= G_PI / 2 && a2 >= G_PI / 2)
    rect.height = MAX (rect.height, cy + r2);

  if (a1 <= G_PI && a2 >= G_PI)
    rect.x = MIN (rect.x, cx - r2);

  if (a1 <= G_PI * 1.5 && a2 >= G_PI * 1.5)
    rect.y = MIN (rect.y, cy - r2);

  if (a1 <= G_PI * 2 && a2 >= G_PI * 2)
    rect.width = MAX (rect.width, cx + r2);

  rect.width -= rect.x;
  rect.height -= rect.y;

  item->rect = rect;
}



static gboolean
du_rings_chart_is_point_over_item (DuChart     *chart,
                                   DuChartItem *item,
                                   gdouble      x,
                                   gdouble      y)
{
  DuRingsChartItem *ringsitem = (DuRingsChartItem *) item;
  gdouble           radius;
  gdouble           angle;

  x -= gtk_widget_get_allocated_width (GTK_WIDGET (chart)) / 2;
  y -= gtk_widget_get_allocated_height (GTK_WIDGET (chart)) / 2;

  radius = sqrt (x * x + y * y);
  angle = atan2 (y, x);
  angle = (angle > 0) ? angle : (angle + 2 * G_PI);

  return radius >= ringsitem->min_radius
         && radius <= ringsitem->max_radius
         && angle >= ringsitem->start_angle
         && angle <= ringsitem->start_angle + ringsitem->angle;
}



static gboolean
du_rings_chart_can_zoom_in (DuChart *chart)
{
  return du_chart_get_max_depth (chart) > DU_CHART_MIN_DEPTH;
}



static gboolean
du_rings_chart_can_zoom_out (DuChart *chart)
{
  return du_chart_get_max_depth (chart) < DU_CHART_MAX_DEPTH;
}



/**
 * du_rings_chart_new:
 *
 * Returns: a new rings chart; give it a scan with du_chart_set_scan().
 **/
GtkWidget *
du_rings_chart_new (void)
{
  return g_object_new (DU_TYPE_RINGS_CHART, NULL);
}

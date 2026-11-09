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


#include "du-chart.h"

#include <glib/gi18n-lib.h>



/* keep in sync with chart_colors[] */
#define NUM_COLORS 6



enum
{
  PROP_0,
  PROP_MAX_DEPTH,
  PROP_ROOT,
  PROP_HIGHLIGHTED_ITEM,
  N_PROPERTIES,
};

enum
{
  ITEM_ACTIVATED,
  TRASH_ITEM,
  LAST_SIGNAL,
};



typedef struct
{
  DuScan  *scan;
  DuNode  *root;
  guint    max_depth;
  gboolean model_changed;

  /* DuChartItem's, parents before their children */
  GPtrArray *items;

  DuChartItem *highlighted_item;

  GtkWidget *context_menu;
  GtkWidget *open_item;
  GtkWidget *copy_item;
  GtkWidget *trash_item;
  GtkWidget *up_item;
  GtkWidget *zoom_in_item;
  GtkWidget *zoom_out_item;
  DuNode    *popover_node;

  GdkRGBA chart_colors[NUM_COLORS];
} DuChartPrivate;



static void
du_chart_finalize (GObject *object);
static void
du_chart_get_property (GObject    *object,
                       guint       prop_id,
                       GValue     *value,
                       GParamSpec *pspec);
static void
du_chart_set_property (GObject      *object,
                       guint         prop_id,
                       const GValue *value,
                       GParamSpec   *pspec);
static gboolean
du_chart_draw (GtkWidget *widget,
               cairo_t   *cr);
static void
du_chart_size_allocate (GtkWidget     *widget,
                        GtkAllocation *allocation);
static gboolean
du_chart_button_press_event (GtkWidget      *widget,
                             GdkEventButton *event);
static gboolean
du_chart_motion_notify_event (GtkWidget      *widget,
                              GdkEventMotion *event);
static gboolean
du_chart_leave_notify_event (GtkWidget        *widget,
                             GdkEventCrossing *event);
static gboolean
du_chart_scroll_event (GtkWidget      *widget,
                       GdkEventScroll *event);
static gboolean
du_chart_query_tooltip (GtkWidget  *widget,
                        gint        x,
                        gint        y,
                        gboolean    keyboard_tooltip,
                        GtkTooltip *tooltip);



static GParamSpec *properties[N_PROPERTIES];
static guint       chart_signals[LAST_SIGNAL];



G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (DuChart, du_chart, GTK_TYPE_DRAWING_AREA)



static void
du_chart_class_init (DuChartClass *klass)
{
  GObjectClass   *gobject_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gobject_class->finalize = du_chart_finalize;
  gobject_class->get_property = du_chart_get_property;
  gobject_class->set_property = du_chart_set_property;

  widget_class->draw = du_chart_draw;
  widget_class->size_allocate = du_chart_size_allocate;
  widget_class->button_press_event = du_chart_button_press_event;
  widget_class->motion_notify_event = du_chart_motion_notify_event;
  widget_class->leave_notify_event = du_chart_leave_notify_event;
  widget_class->scroll_event = du_chart_scroll_event;
  widget_class->query_tooltip = du_chart_query_tooltip;

  klass->item_size = sizeof (DuChartItem);

  properties[PROP_MAX_DEPTH] =
  g_param_spec_uint ("max-depth", NULL, NULL,
                     DU_CHART_MIN_DEPTH, DU_CHART_MAX_DEPTH, DU_CHART_MAX_DEPTH,
                     G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_ROOT] =
  g_param_spec_pointer ("root", NULL, NULL,
                        G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_HIGHLIGHTED_ITEM] =
  g_param_spec_pointer ("highlighted-item", NULL, NULL,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, N_PROPERTIES, properties);

  /**
   * DuChart::item-activated:
   * @chart : a #DuChart.
   * @node  : the #DuNode to show.
   *
   * Emitted when the user clicks a folder, or the centre to go up.
   **/
  chart_signals[ITEM_ACTIVATED] =
  g_signal_new ("item-activated",
                G_TYPE_FROM_CLASS (klass),
                G_SIGNAL_RUN_LAST,
                G_STRUCT_OFFSET (DuChartClass, item_activated),
                NULL, NULL, NULL,
                G_TYPE_NONE, 1, G_TYPE_POINTER);

  /**
   * DuChart::trash-item:
   * @chart : a #DuChart.
   * @node  : the #DuNode to move to the trash.
   *
   * Emitted when the user chooses "Move to Trash" in the context menu.
   **/
  chart_signals[TRASH_ITEM] =
  g_signal_new ("trash-item",
                G_TYPE_FROM_CLASS (klass),
                G_SIGNAL_RUN_LAST,
                G_STRUCT_OFFSET (DuChartClass, trash_item),
                NULL, NULL, NULL,
                G_TYPE_NONE, 1, G_TYPE_POINTER);
}



static void
du_chart_item_free (gpointer data)
{
  DuChartItem *item = data;

  g_free (item->name);
  g_free (item);
}



static void
du_chart_open_file (GtkMenuItem *menu_item,
                    DuChart     *chart)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);
  GtkWidget      *toplevel;
  GError         *error = NULL;
  gchar          *path;
  gchar          *uri;

  if (priv->scan == NULL || priv->popover_node == NULL)
    return;

  path = du_node_get_path (priv->scan, priv->popover_node);
  uri = g_filename_to_uri (path, NULL, NULL);
  toplevel = gtk_widget_get_toplevel (GTK_WIDGET (chart));

  if (uri != NULL
      && !gtk_show_uri_on_window (GTK_IS_WINDOW (toplevel) ? GTK_WINDOW (toplevel) : NULL,
                                  uri, gtk_get_current_event_time (), &error))
    {
      g_warning ("Failed to open \"%s\": %s", path, error->message);
      g_error_free (error);
    }

  g_free (uri);
  g_free (path);
}



static void
du_chart_copy_path (GtkMenuItem *menu_item,
                    DuChart     *chart)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);
  GtkClipboard   *clipboard;
  gchar          *path;
  gchar          *display;

  if (priv->scan == NULL || priv->popover_node == NULL)
    return;

  path = du_node_get_path (priv->scan, priv->popover_node);
  display = g_filename_display_name (path);
  clipboard = gtk_widget_get_clipboard (GTK_WIDGET (chart), GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_text (clipboard, display, -1);

  g_free (display);
  g_free (path);
}



static void
du_chart_trash_file (GtkMenuItem *menu_item,
                     DuChart     *chart)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);

  if (priv->popover_node != NULL)
    g_signal_emit (chart, chart_signals[TRASH_ITEM], 0, priv->popover_node);
}



static void
du_chart_move_up_root_cb (GtkMenuItem *menu_item,
                          DuChart     *chart)
{
  du_chart_move_up_root (chart);
}



static void
du_chart_zoom_in_cb (GtkMenuItem *menu_item,
                     DuChart     *chart)
{
  du_chart_zoom_in (chart);
}



static void
du_chart_zoom_out_cb (GtkMenuItem *menu_item,
                      DuChart     *chart)
{
  du_chart_zoom_out (chart);
}



static GtkWidget *
du_chart_add_menu_item (GtkWidget   *menu,
                        const gchar *label,
                        GCallback    callback,
                        DuChart     *chart)
{
  GtkWidget *item;

  item = gtk_menu_item_new_with_mnemonic (label);
  g_signal_connect (item, "activate", callback, chart);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
  gtk_widget_show (item);

  return item;
}



static void
du_chart_build_context_menu (DuChart *chart)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);
  GtkWidget      *menu;
  GtkWidget      *separator;

  menu = gtk_menu_new ();
  gtk_menu_attach_to_widget (GTK_MENU (menu), GTK_WIDGET (chart), NULL);

  priv->open_item = du_chart_add_menu_item (menu, _("_Open Folder"), G_CALLBACK (du_chart_open_file), chart);
  priv->copy_item = du_chart_add_menu_item (menu, _("_Copy Path to Clipboard"), G_CALLBACK (du_chart_copy_path), chart);
  priv->trash_item = du_chart_add_menu_item (menu, _("Mo_ve to Trash"), G_CALLBACK (du_chart_trash_file), chart);

  separator = gtk_separator_menu_item_new ();
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), separator);
  gtk_widget_show (separator);

  priv->up_item = du_chart_add_menu_item (menu, _("Go to _Parent Folder"), G_CALLBACK (du_chart_move_up_root_cb), chart);
  priv->zoom_in_item = du_chart_add_menu_item (menu, _("Zoom _In"), G_CALLBACK (du_chart_zoom_in_cb), chart);
  priv->zoom_out_item = du_chart_add_menu_item (menu, _("Zoom _Out"), G_CALLBACK (du_chart_zoom_out_cb), chart);

  priv->context_menu = menu;
}



static void
du_chart_init (DuChart *chart)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);

  priv->max_depth = DU_CHART_MAX_DEPTH;
  priv->items = g_ptr_array_new_with_free_func (du_chart_item_free);

  gtk_widget_add_events (GTK_WIDGET (chart),
                         GDK_BUTTON_PRESS_MASK
                         | GDK_POINTER_MOTION_MASK
                         | GDK_LEAVE_NOTIFY_MASK
                         | GDK_SCROLL_MASK
                         | GDK_SMOOTH_SCROLL_MASK);

  du_chart_build_context_menu (chart);

  gdk_rgba_parse (&priv->chart_colors[0], "#e01b24");
  gdk_rgba_parse (&priv->chart_colors[1], "#ff7800");
  gdk_rgba_parse (&priv->chart_colors[2], "#f6d32d");
  gdk_rgba_parse (&priv->chart_colors[3], "#33d17a");
  gdk_rgba_parse (&priv->chart_colors[4], "#3584e4");
  gdk_rgba_parse (&priv->chart_colors[5], "#9141ac");
}



static void
du_chart_finalize (GObject *object)
{
  DuChartPrivate *priv = du_chart_get_instance_private (DU_CHART (object));

  g_ptr_array_unref (priv->items);
  if (priv->scan != NULL)
    du_scan_unref (priv->scan);

  G_OBJECT_CLASS (du_chart_parent_class)->finalize (object);
}



static void
du_chart_get_property (GObject    *object,
                       guint       prop_id,
                       GValue     *value,
                       GParamSpec *pspec)
{
  DuChart        *chart = DU_CHART (object);
  DuChartPrivate *priv = du_chart_get_instance_private (chart);

  switch (prop_id)
    {
    case PROP_MAX_DEPTH:
      g_value_set_uint (value, priv->max_depth);
      break;

    case PROP_ROOT:
      g_value_set_pointer (value, du_chart_get_root (chart));
      break;

    case PROP_HIGHLIGHTED_ITEM:
      g_value_set_pointer (value, priv->highlighted_item);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}



static void
du_chart_set_property (GObject      *object,
                       guint         prop_id,
                       const GValue *value,
                       GParamSpec   *pspec)
{
  DuChart *chart = DU_CHART (object);

  switch (prop_id)
    {
    case PROP_MAX_DEPTH:
      du_chart_set_max_depth (chart, g_value_get_uint (value));
      break;

    case PROP_ROOT:
      du_chart_set_root (chart, g_value_get_pointer (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}



static void
du_chart_set_highlighted_item (DuChart     *chart,
                               DuChartItem *item)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);

  if (priv->highlighted_item == item)
    return;

  priv->highlighted_item = item;
  gtk_widget_queue_draw (GTK_WIDGET (chart));
  g_object_notify_by_pspec (G_OBJECT (chart), properties[PROP_HIGHLIGHTED_ITEM]);
}



static gboolean
du_chart_highlight_item_at_point (DuChart *chart,
                                  gdouble  x,
                                  gdouble  y)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);
  DuChartClass   *klass = DU_CHART_GET_CLASS (chart);
  guint           n;

  for (n = priv->items->len; n-- > 0;)
    {
      DuChartItem *item = g_ptr_array_index (priv->items, n);

      if (item->visible && klass->is_point_over_item (chart, item, x, y))
        {
          du_chart_set_highlighted_item (chart, item);
          return TRUE;
        }
    }

  du_chart_set_highlighted_item (chart, NULL);
  return FALSE;
}



static DuChartItem *
du_chart_add_item (DuChart     *chart,
                   guint        depth,
                   gdouble      rel_start,
                   gdouble      rel_size,
                   DuNode      *node,
                   DuChartItem *parent)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);
  DuChartItem    *item;

  item = g_malloc0 (DU_CHART_GET_CLASS (chart)->item_size);
  item->node = node;
  item->size = node->size;
  item->has_children = node->children != NULL;
  item->depth = depth;
  item->rel_start = rel_start;
  item->rel_size = rel_size;
  item->parent = parent;

  if (node->parent == NULL)
    item->name = g_filename_display_basename (du_scan_get_path (priv->scan));
  else
    item->name = g_filename_display_name (node->name);

  g_ptr_array_add (priv->items, item);

  return item;
}



static void
du_chart_get_items (DuChart *chart)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);
  DuChartClass   *klass = DU_CHART_GET_CLASS (chart);
  DuNode         *highlighted_node = NULL;
  DuNode         *root;
  DuNode         *child;
  guint64         root_size;
  guint           n;

  /* the items are rebuilt; keep the highlight on the same folder */
  if (priv->highlighted_item != NULL)
    highlighted_node = priv->highlighted_item->node;
  priv->highlighted_item = NULL;

  g_ptr_array_set_size (priv->items, 0);
  priv->model_changed = FALSE;

  if (priv->scan == NULL)
    return;

  /* the scanner thread adds and resorts the upper levels of the tree */
  du_scan_lock (priv->scan);

  root = du_chart_get_root (chart);
  if (root == NULL)
    {
      du_scan_unlock (priv->scan);
      return;
    }

  root_size = root->size;
  du_chart_add_item (chart, 0, 0, 100, root, NULL);

  for (n = 0; n < priv->items->len; n++)
    {
      DuChartItem *item = g_ptr_array_index (priv->items, n);
      gdouble      rel_start = 0;

      klass->calculate_item_geometry (chart, item);

      if (item->node == highlighted_node)
        priv->highlighted_item = item;

      if (!item->visible || !item->has_children || item->depth >= priv->max_depth)
        continue;

      for (child = item->node->children; child != NULL; child = child->next)
        {
          gdouble percent;

          /* the children are sorted by size; anything below 0.1 % of the
           * chart is too small to be drawn, and a folder with many
           * thousands of sub folders should not slow down each redraw */
          if (child->size * 1000 < root_size)
            break;

          percent = item->size > 0 ? 100.0 * child->size / item->size : 0;
          du_chart_add_item (chart, item->depth + 1, rel_start, percent, child, item);
          rel_start += percent;
        }
    }

  du_scan_unlock (priv->scan);

  if (highlighted_node != NULL && priv->highlighted_item == NULL)
    g_object_notify_by_pspec (G_OBJECT (chart), properties[PROP_HIGHLIGHTED_ITEM]);
}



static gboolean
du_chart_draw (GtkWidget *widget,
               cairo_t   *cr)
{
  DuChart        *chart = DU_CHART (widget);
  DuChartPrivate *priv = du_chart_get_instance_private (chart);
  DuChartClass   *klass = DU_CHART_GET_CLASS (chart);
  guint           n;

  if (priv->scan == NULL)
    return FALSE;

  if (priv->model_changed || priv->items->len == 0
      || ((DuChartItem *) g_ptr_array_index (priv->items, 0))->node != du_chart_get_root (chart))
    du_chart_get_items (chart);

  cairo_save (cr);

  for (n = 0; n < priv->items->len; n++)
    {
      DuChartItem *item = g_ptr_array_index (priv->items, n);

      if (item->visible && item->depth <= priv->max_depth)
        klass->draw_item (chart, cr, item, item == priv->highlighted_item);
    }

  cairo_restore (cr);

  if (klass->post_draw != NULL)
    klass->post_draw (chart, cr);

  return FALSE;
}



static void
du_chart_size_allocate (GtkWidget     *widget,
                        GtkAllocation *allocation)
{
  DuChartPrivate *priv = du_chart_get_instance_private (DU_CHART (widget));

  GTK_WIDGET_CLASS (du_chart_parent_class)->size_allocate (widget, allocation);

  /* the geometry depends on the size, and so does which items are visible */
  priv->model_changed = TRUE;
}



static void
du_chart_show_popover_at (DuChart  *chart,
                          GdkEvent *event)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);
  DuChartClass   *klass = DU_CHART_GET_CLASS (chart);
  gboolean        enable = priv->highlighted_item != NULL;

  priv->popover_node = enable ? priv->highlighted_item->node : NULL;

  gtk_widget_set_sensitive (priv->open_item, enable);
  gtk_widget_set_sensitive (priv->copy_item, enable);
  gtk_widget_set_sensitive (priv->trash_item, enable && priv->popover_node->parent != NULL);
  gtk_widget_set_sensitive (priv->up_item, du_chart_get_root (chart) != NULL && du_chart_get_root (chart)->parent != NULL);
  gtk_widget_set_sensitive (priv->zoom_in_item, klass->can_zoom_in (chart));
  gtk_widget_set_sensitive (priv->zoom_out_item, klass->can_zoom_out (chart));

  gtk_menu_popup_at_pointer (GTK_MENU (priv->context_menu), event);
}



static gboolean
du_chart_button_press_event (GtkWidget      *widget,
                             GdkEventButton *event)
{
  DuChart        *chart = DU_CHART (widget);
  DuChartPrivate *priv = du_chart_get_instance_private (chart);

  if (event->type != GDK_BUTTON_PRESS)
    return FALSE;

  switch (event->button)
    {
    case GDK_BUTTON_PRIMARY:
      if (du_chart_highlight_item_at_point (chart, event->x, event->y))
        {
          if (du_chart_get_root (chart) == priv->highlighted_item->node)
            du_chart_move_up_root (chart);
          else
            g_signal_emit (chart, chart_signals[ITEM_ACTIVATED], 0, priv->highlighted_item->node);
        }
      return TRUE;

    case GDK_BUTTON_SECONDARY:
      du_chart_highlight_item_at_point (chart, event->x, event->y);
      du_chart_show_popover_at (chart, (GdkEvent *) event);
      return TRUE;

    case GDK_BUTTON_MIDDLE:
      du_chart_move_up_root (chart);
      return TRUE;

    default:
      return FALSE;
    }
}



static gboolean
du_chart_motion_notify_event (GtkWidget      *widget,
                              GdkEventMotion *event)
{
  gtk_widget_set_has_tooltip (widget, du_chart_highlight_item_at_point (DU_CHART (widget), event->x, event->y));

  return FALSE;
}



static gboolean
du_chart_leave_notify_event (GtkWidget        *widget,
                             GdkEventCrossing *event)
{
  DuChartPrivate *priv = du_chart_get_instance_private (DU_CHART (widget));

  /* leaving into the context menu keeps the highlight */
  if (!gtk_widget_get_visible (priv->context_menu))
    du_chart_set_highlighted_item (DU_CHART (widget), NULL);

  return FALSE;
}



static gboolean
du_chart_scroll_event (GtkWidget      *widget,
                       GdkEventScroll *event)
{
  DuChart *chart = DU_CHART (widget);
  gdouble  dx = 0;
  gdouble  dy = 0;

  switch (event->direction)
    {
    case GDK_SCROLL_UP:
      dy = -1;
      break;

    case GDK_SCROLL_DOWN:
      dy = 1;
      break;

    case GDK_SCROLL_LEFT:
      dx = -1;
      break;

    case GDK_SCROLL_RIGHT:
      dx = 1;
      break;

    case GDK_SCROLL_SMOOTH:
      gdk_event_get_scroll_deltas ((GdkEvent *) event, &dx, &dy);
      break;
    }

  /* up or to the left */
  if (dx > 0.0 || dy < 0.0)
    {
      du_chart_zoom_out (chart);
      return TRUE;
    }

  /* down or to the right */
  if (dx < 0.0 || dy > 0.0)
    {
      du_chart_zoom_in (chart);
      return TRUE;
    }

  return FALSE;
}



static gboolean
du_chart_query_tooltip (GtkWidget  *widget,
                        gint        x,
                        gint        y,
                        gboolean    keyboard_tooltip,
                        GtkTooltip *tooltip)
{
  DuChartPrivate *priv = du_chart_get_instance_private (DU_CHART (widget));
  DuChartItem    *item = priv->highlighted_item;
  gchar          *size;
  gchar          *text;

  if (item == NULL)
    return FALSE;

  gtk_tooltip_set_tip_area (tooltip, &item->rect);

  size = g_format_size (item->size);
  text = g_strdup_printf ("%s\n%s", item->name, size);
  gtk_tooltip_set_text (tooltip, text);

  g_free (text);
  g_free (size);

  return TRUE;
}



static void
du_chart_interpolate_colors (const GdkRGBA *colora,
                             const GdkRGBA *colorb,
                             gdouble        percentage,
                             GdkRGBA       *color)
{
  color->red = colora->red - (colora->red - colorb->red) * percentage;
  color->green = colora->green - (colora->green - colorb->green) * percentage;
  color->blue = colora->blue - (colora->blue - colorb->blue) * percentage;
  color->alpha = 1.0;
}



/**
 * du_chart_get_item_color:
 * @chart        : a #DuChart.
 * @rel_position : the position of the item, 0 … 200.
 * @depth        : the depth of the item.
 * @highlighted  : the item is under the pointer.
 * @color        : return location for the colour.
 *
 * The fill colour of an item: the hue follows the position, deeper items
 * are darker.
 **/
void
du_chart_get_item_color (DuChart *chart,
                         gdouble  rel_position,
                         guint    depth,
                         gboolean highlighted,
                         GdkRGBA *color)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);
  gdouble         intensity;
  gdouble         maximum;
  gint            color_number;
  gint            next_color_number;

  if (depth == 0)
    {
      gdk_rgba_parse (color, highlighted ? "#e0e2dd" : "#d3d6d1");
      return;
    }

  intensity = 1 - (((depth - 1) * 0.3) / DU_CHART_MAX_DEPTH);

  color_number = CLAMP ((gint) (rel_position / (100.0 / 3)), 0, NUM_COLORS - 1);
  next_color_number = (color_number + 1) % NUM_COLORS;

  du_chart_interpolate_colors (&priv->chart_colors[color_number],
                               &priv->chart_colors[next_color_number],
                               (rel_position - color_number * 100.0 / 3) / (100.0 / 3),
                               color);

  color->red *= intensity;
  color->green *= intensity;
  color->blue *= intensity;

  if (highlighted)
    {
      maximum = MAX (color->red, MAX (color->green, color->blue));
      if (maximum > 0)
        {
          color->red /= maximum;
          color->green /= maximum;
          color->blue /= maximum;
        }
    }
}



/**
 * du_chart_set_scan:
 * @chart : a #DuChart.
 * @scan  : the scan to show, or %NULL.
 *
 * Shows @scan, starting at its root.
 **/
void
du_chart_set_scan (DuChart *chart,
                   DuScan  *scan)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);

  g_return_if_fail (DU_IS_CHART (chart));

  if (priv->scan == scan)
    return;

  /* the items point into the old tree */
  priv->highlighted_item = NULL;
  priv->popover_node = NULL;
  g_ptr_array_set_size (priv->items, 0);

  if (priv->scan != NULL)
    du_scan_unref (priv->scan);
  priv->scan = scan != NULL ? du_scan_ref (scan) : NULL;
  priv->root = NULL;
  priv->model_changed = TRUE;

  g_object_notify_by_pspec (G_OBJECT (chart), properties[PROP_ROOT]);
  g_object_notify_by_pspec (G_OBJECT (chart), properties[PROP_HIGHLIGHTED_ITEM]);
  gtk_widget_queue_draw (GTK_WIDGET (chart));
}



DuScan *
du_chart_get_scan (DuChart *chart)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);

  g_return_val_if_fail (DU_IS_CHART (chart), NULL);

  return priv->scan;
}



/**
 * du_chart_set_root:
 * @chart : a #DuChart.
 * @root  : a node of the chart's scan, or %NULL for the root of the scan.
 *
 * Shows @root in the centre.
 **/
void
du_chart_set_root (DuChart *chart,
                   DuNode  *root)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);

  g_return_if_fail (DU_IS_CHART (chart));

  if (priv->scan == NULL || du_chart_get_root (chart) == root)
    return;

  priv->root = root;
  du_chart_set_highlighted_item (chart, NULL);

  g_object_notify_by_pspec (G_OBJECT (chart), properties[PROP_ROOT]);
  gtk_widget_queue_draw (GTK_WIDGET (chart));
}



/**
 * du_chart_get_root:
 * @chart : a #DuChart.
 *
 * Returns: the node in the centre, or %NULL if the scan has not started.
 **/
DuNode *
du_chart_get_root (DuChart *chart)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);

  g_return_val_if_fail (DU_IS_CHART (chart), NULL);

  if (priv->root != NULL)
    return priv->root;

  if (priv->scan != NULL)
    return du_scan_get_root (priv->scan);

  return NULL;
}



/**
 * du_chart_update:
 * @chart : a #DuChart.
 *
 * Redraws the chart after the scan has changed the tree.
 **/
void
du_chart_update (DuChart *chart)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);

  g_return_if_fail (DU_IS_CHART (chart));

  priv->model_changed = TRUE;
  if (gtk_widget_get_realized (GTK_WIDGET (chart)))
    gtk_widget_queue_draw (GTK_WIDGET (chart));
}



void
du_chart_set_max_depth (DuChart *chart,
                        guint    max_depth)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);

  g_return_if_fail (DU_IS_CHART (chart));

  max_depth = CLAMP (max_depth, DU_CHART_MIN_DEPTH, DU_CHART_MAX_DEPTH);
  if (priv->max_depth == max_depth)
    return;

  priv->max_depth = max_depth;
  priv->model_changed = TRUE;

  g_object_notify_by_pspec (G_OBJECT (chart), properties[PROP_MAX_DEPTH]);
  gtk_widget_queue_draw (GTK_WIDGET (chart));
}



guint
du_chart_get_max_depth (DuChart *chart)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);

  g_return_val_if_fail (DU_IS_CHART (chart), DU_CHART_MAX_DEPTH);

  return priv->max_depth;
}



/**
 * du_chart_get_highlighted_item:
 * @chart : a #DuChart.
 *
 * Returns: the item under the pointer, or %NULL. Valid until the next redraw.
 **/
DuChartItem *
du_chart_get_highlighted_item (DuChart *chart)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);

  g_return_val_if_fail (DU_IS_CHART (chart), NULL);

  return priv->highlighted_item;
}



void
du_chart_move_up_root (DuChart *chart)
{
  DuNode *root = du_chart_get_root (chart);

  if (root != NULL && root->parent != NULL)
    {
      du_chart_set_root (chart, root->parent);
      g_signal_emit (chart, chart_signals[ITEM_ACTIVATED], 0, root->parent);
    }
}



void
du_chart_zoom_in (DuChart *chart)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);

  if (DU_CHART_GET_CLASS (chart)->can_zoom_in (chart))
    du_chart_set_max_depth (chart, priv->max_depth - 1);
}



void
du_chart_zoom_out (DuChart *chart)
{
  DuChartPrivate *priv = du_chart_get_instance_private (chart);

  if (DU_CHART_GET_CLASS (chart)->can_zoom_out (chart))
    du_chart_set_max_depth (chart, priv->max_depth + 1);
}

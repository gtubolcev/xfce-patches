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

/* du-scan-test PATH [DEPTH]: scan PATH like the plugin does and print the
 * folder sizes in bytes, for comparison with "du -xB1 --max-depth=DEPTH". */

#include "thunar-plugin/du-scanner.h"

#include <stdlib.h>



static void
print_node (DuScan *scan,
            DuNode *node,
            guint   depth,
            guint   max_depth)
{
  DuNode *child;
  gchar  *path;

  if (depth < max_depth)
    for (child = node->children; child != NULL; child = child->next)
      print_node (scan, child, depth + 1, max_depth);

  path = du_node_get_path (scan, node);
  g_print ("%" G_GUINT64_FORMAT "\t%s%s\n", node->size, path, node->unreadable ? "\t(unreadable)" : "");
  g_free (path);
}



int
main (int    argc,
      char **argv)
{
  GMainContext *context;
  DuScan       *scan;
  guint64       items;
  guint         n_unreadable;
  guint         max_depth;

  if (argc < 2)
    {
      g_printerr ("usage: %s PATH [DEPTH]\n", argv[0]);
      return 2;
    }

  max_depth = argc > 2 ? (guint) atoi (argv[2]) : 1;
  context = g_main_context_default ();

  scan = du_scan_new (argv[1]);
  du_scan_start (scan, NULL);
  while (du_scan_get_state (scan) == DU_SCAN_RUNNING)
    g_main_context_iteration (context, TRUE);

  if (du_scan_get_state (scan) == DU_SCAN_FAILED)
    {
      g_printerr ("%s\n", du_scan_get_error (scan)->message);
      du_scan_unref (scan);
      return 1;
    }

  du_scan_lock (scan);
  print_node (scan, du_scan_get_root (scan), 0, max_depth);
  du_scan_unlock (scan);

  du_scan_get_progress (scan, NULL, &items, &n_unreadable);
  g_printerr ("%" G_GUINT64_FORMAT " items, %u unreadable folders\n", items, n_unreadable);

  du_scan_unref (scan);
  return 0;
}

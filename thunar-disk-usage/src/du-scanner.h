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

#ifndef __DU_SCANNER_H__
#define __DU_SCANNER_H__

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _DuNode DuNode;
typedef struct _DuScan DuScan;

/**
 * DuNode:
 * @name        : file name (not the full path), in the file system encoding.
 * @size        : bytes used on disk by this folder and everything below it.
 * @files_size  : bytes used by the files directly in this folder.
 * @n_items     : number of files and folders below this folder.
 * @parent      : the parent folder, %NULL for the root.
 * @children    : the first sub folder; siblings are sorted by size, largest first.
 * @next        : the next sibling.
 * @unreadable  : the folder could not be read (its contents are missing).
 *
 * A folder of a scan. Only folders get a node; files are summed up in their
 * folder, so a scan of / needs memory for the folders only.
 **/
struct _DuNode
{
  gchar  *name;
  guint64 size;
  guint64 files_size;
  guint64 n_items;
  DuNode *parent;
  DuNode *children;
  DuNode *next;
  guint   unreadable : 1;
};

typedef enum
{
  DU_SCAN_IDLE,
  DU_SCAN_RUNNING,
  DU_SCAN_DONE,
  DU_SCAN_CANCELLED,
  DU_SCAN_FAILED,
} DuScanState;

DuScan *
du_scan_new (const gchar *path);
DuScan *
du_scan_ref (DuScan *scan);
void
du_scan_unref (DuScan *scan);

const gchar *
du_scan_get_path (DuScan *scan);
DuScanState
du_scan_get_state (DuScan *scan);
const GError *
du_scan_get_error (DuScan *scan);

void
du_scan_start (DuScan       *scan,
               GCancellable *cancellable);
void
du_scan_cancel (DuScan *scan);

guint
du_scan_get_generation (DuScan *scan);
void
du_scan_get_progress (DuScan  *scan,
                      guint64 *bytes,
                      guint64 *items,
                      guint   *n_unreadable);

void
du_scan_lock (DuScan *scan);
void
du_scan_unlock (DuScan *scan);
DuNode *
du_scan_get_root (DuScan *scan);

gchar *
du_node_get_path (DuScan *scan,
                  DuNode *node);
DuNode *
du_node_find (DuNode      *root,
              const gchar *relative_path);

G_END_DECLS

#endif /* !__DU_SCANNER_H__ */

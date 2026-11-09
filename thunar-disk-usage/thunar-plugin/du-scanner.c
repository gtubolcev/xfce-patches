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

#include "du-scanner.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * The scan walks the tree with openat()/fstatat() in a worker thread, like
 * "du -x": the size is the space used on disk (st_blocks), a file with
 * several hard links is counted once, and other file systems mounted below
 * the folder are skipped.
 *
 * Folders up to DU_PUBLISH_DEPTH below the root are "shared": attached to
 * the tree before they are scanned, and every size change is propagated
 * to their ancestors under the lock. Deeper folders are scanned privately
 * and attached once complete. So the UI can draw a growing tree while the
 * scan runs: it takes the lock, reads, and redraws when the generation has
 * changed.
 */



struct _DuScan
{
  gint   ref_count;
  gchar *path;

  GMutex      lock;
  DuNode     *root;
  DuScanState state;
  GError     *error;

  GCancellable *cancellable;

  /* updated by the worker, read by the UI without the lock */
  guint   generation;
  guint64 progress_bytes;
  guint64 progress_items;
  guint   n_unreadable;
};

typedef struct
{
  DuScan     *scan;
  dev_t       dev;
  GHashTable *inodes;
} DuWalk;

/* subtrees up to this depth below the root are attached to the shared tree
 * as soon as they are complete */
#define DU_PUBLISH_DEPTH 2



static void
du_node_free (DuNode *node)
{
  DuNode *child;
  DuNode *next;

  for (child = node->children; child != NULL; child = next)
    {
      next = child->next;
      du_node_free (child);
    }

  g_free (node->name);
  g_slice_free (DuNode, node);
}



static DuNode *
du_node_new (const gchar *name,
             DuNode      *parent)
{
  DuNode *node;

  node = g_slice_new0 (DuNode);
  node->name = g_strdup (name);
  node->parent = parent;

  return node;
}



/* insert @child into the sorted children of @parent */
static void
du_node_insert_sorted (DuNode *parent,
                       DuNode *child)
{
  DuNode **p;

  for (p = &parent->children; *p != NULL && (*p)->size >= child->size; p = &(*p)->next)
    ;

  child->next = *p;
  child->parent = parent;
  *p = child;
}



/* returns TRUE if this (dev, ino) is seen for the first time */
static gboolean
du_walk_first_link (DuWalk            *walk,
                    const struct stat *st)
{
  guint64 *key;

  if (walk->inodes == NULL)
    walk->inodes = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);

  /* the device is the same for the whole walk (-x), so the inode is enough */
  key = g_new (guint64, 1);
  *key = (guint64) st->st_ino;

  if (g_hash_table_contains (walk->inodes, key))
    {
      g_free (key);
      return FALSE;
    }

  g_hash_table_add (walk->inodes, key);
  return TRUE;
}



/* move @node to its place among its siblings after its size has grown */
static void
du_node_resort (DuNode *node)
{
  DuNode  *parent = node->parent;
  DuNode **p;

  if (parent == NULL)
    return;

  for (p = &parent->children; *p != node; p = &(*p)->next)
    ;
  *p = node->next;

  du_node_insert_sorted (parent, node);
}



/* add @bytes and @items to @node and, if it is shared, to its ancestors;
 * a shared node is changed under the lock only */
static void
du_walk_add (DuScan  *scan,
             DuNode  *node,
             gboolean shared,
             guint64  bytes,
             guint64  items)
{
  DuNode *a;

  if (!shared)
    {
      node->size += bytes;
      node->n_items += items;
      return;
    }

  g_mutex_lock (&scan->lock);
  for (a = node; a != NULL; a = a->parent)
    {
      a->size += bytes;
      a->n_items += items;
    }
  if (bytes > 0)
    g_atomic_int_inc (&scan->generation);
  g_mutex_unlock (&scan->lock);
}



/*
 * Scans the folder @fd (owned; closed here) into @node, which is at @depth
 * below the root. @node->size already holds the folder's own blocks. If
 * @shared, @node is attached to the shared tree.
 */
static void
du_walk_folder (DuWalk  *walk,
                DuNode  *node,
                gint     fd,
                guint    depth,
                gboolean shared)
{
  DuScan        *scan = walk->scan;
  DIR           *dp;
  struct dirent *de;
  struct stat    st;
  guint64        bytes;
  DuNode        *child;
  gint           child_fd;
  gboolean       child_shared;

  dp = fdopendir (fd);
  if (G_UNLIKELY (dp == NULL))
    {
      close (fd);
      node->unreadable = TRUE;
      g_atomic_int_inc (&scan->n_unreadable);
      return;
    }

  while ((de = readdir (dp)) != NULL)
    {
      if (g_cancellable_is_cancelled (scan->cancellable))
        break;

      if (strcmp (de->d_name, ".") == 0 || strcmp (de->d_name, "..") == 0)
        continue;

      if (fstatat (dirfd (dp), de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        continue;

      /* another file system mounted here: skip it, as du -x does */
      if (st.st_dev != walk->dev)
        continue;

      bytes = (guint64) st.st_blocks * 512;
      scan->progress_items++;

      if (S_ISDIR (st.st_mode))
        {
          scan->progress_bytes += bytes;

          child = du_node_new (de->d_name, NULL);
          child_shared = shared && depth + 1 <= DU_PUBLISH_DEPTH;

          if (child_shared)
            {
              /* attach it now, empty, and let it grow in place */
              g_mutex_lock (&scan->lock);
              du_node_insert_sorted (node, child);
              g_mutex_unlock (&scan->lock);
              du_walk_add (scan, child, TRUE, bytes, 0);
              du_walk_add (scan, node, TRUE, 0, 1);
            }
          else
            child->size = bytes;

          child_fd = openat (dirfd (dp), de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
          if (child_fd >= 0)
            du_walk_folder (walk, child, child_fd, depth + 1, child_shared);
          else
            {
              child->unreadable = TRUE;
              g_atomic_int_inc (&scan->n_unreadable);
            }

          if (child_shared)
            {
              g_mutex_lock (&scan->lock);
              du_node_resort (child);
              g_atomic_int_inc (&scan->generation);
              g_mutex_unlock (&scan->lock);
            }
          else if (shared)
            {
              /* a complete private subtree joins the shared tree */
              g_mutex_lock (&scan->lock);
              du_node_insert_sorted (node, child);
              g_mutex_unlock (&scan->lock);
              du_walk_add (scan, node, TRUE, child->size, child->n_items + 1);
            }
          else
            {
              du_node_insert_sorted (node, child);
              du_walk_add (scan, node, FALSE, child->size, child->n_items + 1);
            }
        }
      else
        {
          /* a file with several links is counted once */
          if (st.st_nlink > 1 && !du_walk_first_link (walk, &st))
            bytes = 0;

          scan->progress_bytes += bytes;

          if (shared)
            g_mutex_lock (&scan->lock);
          node->files_size += bytes;
          if (shared)
            g_mutex_unlock (&scan->lock);

          du_walk_add (scan, node, shared, bytes, 1);
        }
    }

  closedir (dp);
}



static void
du_scan_thread (GTask        *task,
                gpointer      source_object,
                gpointer      task_data,
                GCancellable *cancellable)
{
  DuScan     *scan = task_data;
  DuWalk      walk = { scan, 0, NULL };
  struct stat st;
  DuNode     *root;
  gint        fd;
  gchar      *name;

  fd = open (scan->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0 || fstat (fd, &st) != 0)
    {
      gint errsv = errno;

      if (fd >= 0)
        close (fd);
      g_task_return_new_error (task, G_IO_ERROR, g_io_error_from_errno (errsv),
                               "%s: %s", scan->path, g_strerror (errsv));
      return;
    }

  walk.dev = st.st_dev;

  name = g_path_get_basename (scan->path);
  root = du_node_new (name, NULL);
  root->size = (guint64) st.st_blocks * 512;
  g_free (name);

  g_mutex_lock (&scan->lock);
  scan->root = root;
  g_mutex_unlock (&scan->lock);

  du_walk_folder (&walk, root, fd, 0, TRUE);

  if (walk.inodes != NULL)
    g_hash_table_destroy (walk.inodes);

  g_atomic_int_inc (&scan->generation);

  if (g_task_return_error_if_cancelled (task))
    return;

  g_task_return_boolean (task, TRUE);
}



static void
du_scan_done (GObject      *source_object,
              GAsyncResult *result,
              gpointer      user_data)
{
  DuScan *scan = user_data;
  GError *error = NULL;

  g_mutex_lock (&scan->lock);
  if (g_task_propagate_boolean (G_TASK (result), &error))
    scan->state = DU_SCAN_DONE;
  else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      scan->state = DU_SCAN_CANCELLED;
      g_clear_error (&error);
    }
  else
    {
      scan->state = DU_SCAN_FAILED;
      scan->error = error;
    }
  g_mutex_unlock (&scan->lock);

  g_atomic_int_inc (&scan->generation);
  du_scan_unref (scan);
}



/**
 * du_scan_new:
 * @path : absolute path of a local folder.
 *
 * Returns: a new scan of @path, not started yet.
 **/
DuScan *
du_scan_new (const gchar *path)
{
  DuScan *scan;

  g_return_val_if_fail (path != NULL && g_path_is_absolute (path), NULL);

  scan = g_new0 (DuScan, 1);
  scan->ref_count = 1;
  scan->path = g_strdup (path);
  scan->state = DU_SCAN_IDLE;
  g_mutex_init (&scan->lock);

  return scan;
}



DuScan *
du_scan_ref (DuScan *scan)
{
  g_return_val_if_fail (scan != NULL, NULL);

  g_atomic_int_inc (&scan->ref_count);
  return scan;
}



void
du_scan_unref (DuScan *scan)
{
  g_return_if_fail (scan != NULL);

  if (!g_atomic_int_dec_and_test (&scan->ref_count))
    return;

  if (scan->root != NULL)
    du_node_free (scan->root);
  g_clear_error (&scan->error);
  g_clear_object (&scan->cancellable);
  g_mutex_clear (&scan->lock);
  g_free (scan->path);
  g_free (scan);
}



const gchar *
du_scan_get_path (DuScan *scan)
{
  return scan->path;
}



DuScanState
du_scan_get_state (DuScan *scan)
{
  DuScanState state;

  g_mutex_lock (&scan->lock);
  state = scan->state;
  g_mutex_unlock (&scan->lock);

  return state;
}



/* only valid in the DU_SCAN_FAILED state */
const GError *
du_scan_get_error (DuScan *scan)
{
  return scan->error;
}



/**
 * du_scan_start:
 * @scan        : an idle #DuScan.
 * @cancellable : a #GCancellable or %NULL.
 *
 * Starts the scan in a worker thread. The scan holds a reference on itself
 * until the worker is done, so it may be unreffed (after du_scan_cancel())
 * while it runs.
 **/
void
du_scan_start (DuScan       *scan,
               GCancellable *cancellable)
{
  GTask *task;

  g_return_if_fail (scan != NULL);
  g_return_if_fail (scan->state == DU_SCAN_IDLE);

  scan->cancellable = cancellable != NULL ? g_object_ref (cancellable) : g_cancellable_new ();
  scan->state = DU_SCAN_RUNNING;

  task = g_task_new (NULL, scan->cancellable, du_scan_done, du_scan_ref (scan));
  g_task_set_task_data (task, scan, NULL);
  g_task_set_return_on_cancel (task, FALSE);
  g_task_run_in_thread (task, du_scan_thread);
  g_object_unref (task);
}



void
du_scan_cancel (DuScan *scan)
{
  if (scan->cancellable != NULL)
    g_cancellable_cancel (scan->cancellable);
}



/**
 * du_scan_get_generation:
 *
 * Returns: a counter that changes whenever the tree has grown; the UI polls
 *          it and redraws when it differs from the last value.
 **/
guint
du_scan_get_generation (DuScan *scan)
{
  return (guint) g_atomic_int_get (&scan->generation);
}



void
du_scan_get_progress (DuScan  *scan,
                      guint64 *bytes,
                      guint64 *items,
                      guint   *n_unreadable)
{
  /* approximate values for a status line, read without the lock */
  if (bytes != NULL)
    *bytes = scan->progress_bytes;
  if (items != NULL)
    *items = scan->progress_items;
  if (n_unreadable != NULL)
    *n_unreadable = (guint) g_atomic_int_get (&scan->n_unreadable);
}



/* the tree may only be read between du_scan_lock() and du_scan_unlock() */
void
du_scan_lock (DuScan *scan)
{
  g_mutex_lock (&scan->lock);
}



void
du_scan_unlock (DuScan *scan)
{
  g_mutex_unlock (&scan->lock);
}



DuNode *
du_scan_get_root (DuScan *scan)
{
  return scan->root;
}



/**
 * du_node_get_path:
 *
 * Returns: the absolute path of @node; free with g_free().
 **/
gchar *
du_node_get_path (DuScan *scan,
                  DuNode *node)
{
  GPtrArray *parts;
  gchar     *path;
  gchar     *relative;

  parts = g_ptr_array_new ();
  for (; node != NULL && node->parent != NULL; node = node->parent)
    g_ptr_array_insert (parts, 0, node->name);
  g_ptr_array_add (parts, NULL);

  relative = g_build_filenamev ((gchar **) parts->pdata);
  path = g_build_filename (scan->path, relative, NULL);

  g_free (relative);
  g_ptr_array_free (parts, TRUE);

  return path;
}



/**
 * du_node_find:
 * @root          : a node.
 * @relative_path : a path relative to @root, "" for @root itself.
 *
 * Returns: the node of @relative_path below @root, or %NULL.
 **/
DuNode *
du_node_find (DuNode      *root,
              const gchar *relative_path)
{
  gchar **parts;
  DuNode *node = root;
  guint   n;

  parts = g_strsplit (relative_path, G_DIR_SEPARATOR_S, -1);
  for (n = 0; parts[n] != NULL && node != NULL; n++)
    {
      DuNode *child;

      if (*parts[n] == '\0')
        continue;

      for (child = node->children; child != NULL; child = child->next)
        if (strcmp (child->name, parts[n]) == 0)
          break;
      node = child;
    }
  g_strfreev (parts);

  return node;
}

/*
 * Copyright (C) 2001 Sun Microsystems, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Authors:
 *     Mark McLoughlin <mark@skynet.ie>
 */

#include <config.h>
#include <locale.h>

#include "markup-tree.c"
#include "markup-write-state.c"

guint
_gconf_mode_t_to_mode (mode_t orig)
{
  /* I don't think this is portable. */
  guint mode = 0;
  guint fullmask = S_IRWXG | S_IRWXU | S_IRWXO;
  
  mode = orig & fullmask;
  
  g_return_val_if_fail (mode <= 0777, 0700);
  
  return mode;
}

static void
recursively_load_entire_tree (MarkupDir *dir)
{
  GSList *tmp;

  load_entries (dir);
  load_subdirs (dir);

  tmp = dir->subdirs;
  while (tmp != NULL)
    {
      MarkupDir *subdir = tmp->data;

      recursively_load_entire_tree (subdir);

      tmp = tmp->next;
    }
}

static gboolean
merge_tree (const char *root_dir)
{
  struct stat statbuf;
  guint dir_mode;
  guint file_mode;
  MarkupTree *tree;
  GError *error;

  if (stat (root_dir, &statbuf) == 0)
    {
      dir_mode = _gconf_mode_t_to_mode (statbuf.st_mode);
      /* dir_mode without search bits */
      file_mode = dir_mode & (~0111);
    }
  else
    {
      fprintf (stderr, _("Cannot find directory %s\n"), root_dir);
      return FALSE;

    }

  tree = markup_tree_get (root_dir, dir_mode, file_mode, FALSE);

  recursively_load_entire_tree (tree->root);

  error = NULL;
  save_tree (tree->root, TRUE, file_mode, &error);
  if (error)
    {
      char *markup_file;

      markup_file = markup_dir_build_path (tree->root, TRUE, TRUE);
      fprintf (stderr, _("Error saving GConf tree to '%s': %s\n"),
	       markup_file,
	       error->message);
      g_error_free (error);
      g_free (markup_file);
      markup_tree_unref (tree);
      return FALSE;
    }

  tree->root->entries_need_save = FALSE;
  tree->root->some_subdir_needs_sync = FALSE;

  markup_tree_unref (tree);

  return TRUE;
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  _gconf_init_i18n ();
  textdomain (GETTEXT_PACKAGE);

  if (argc != 2)
    {
      fprintf (stderr, _("Usage: %s <dir>\n"), argv [0]);
      return 1;
    }

  if (!strcmp (argv [1], "--help"))
    {
      printf (_("Usage: %s <dir>\n"
		"  Merges a markup backend filesystem hierarchy like:\n"
		"    dir/%%gconf.xml\n"
		"        subdir1/%%gconf.xml\n"
		"        subdir2/%%gconf.xml\n"
		"  to:\n"
		"    dir/%%gconf-tree.xml\n"), argv [0]);
      return 0;
    }

  return !merge_tree (argv [1]);
}

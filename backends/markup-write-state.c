/* GConf
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
 */

#include <config.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <gconf/gconf-internals.h>
#include "markup-write-state.h"

#define STATE_FILENAME "gconf-reliable-writes-state"

#define STATE_STRING_LEN 2
#define STATE_OK         "OK"
#define STATE_WRITING_V1 "V1"
#define STATE_WRITING_V2 "V2"

#define d(x) 


static gchar *
write_state_get_filename (void)
{
  return g_build_filename (g_get_home_dir (), ".gconfd", STATE_FILENAME, NULL);
}

MarkupWriteState
markup_write_state_read (void)
{
  gchar *filename;
  FILE  *f;
  gchar  buf[STATE_STRING_LEN+1];
  
  filename = write_state_get_filename ();
  f = fopen (filename, "r");
  g_free (filename);

  if (f == NULL)
    return MARKUP_WRITE_STATE_INIT;

  if (!fgets (buf, STATE_STRING_LEN+1, f))
    return MARKUP_WRITE_STATE_INIT;
  
  fclose (f);
  
  if (strncmp (buf, STATE_OK, STATE_STRING_LEN) == 0)
    return MARKUP_WRITE_STATE_OK;
  else if (strncmp (buf, STATE_WRITING_V1, STATE_STRING_LEN) == 0)
    return MARKUP_WRITE_STATE_WRITING_V1;
  else if (strncmp (buf, STATE_WRITING_V2, STATE_STRING_LEN) == 0)
    return MARKUP_WRITE_STATE_WRITING_V2;
  
  return MARKUP_WRITE_STATE_INIT;
}

gboolean
markup_write_state_write (MarkupWriteState state)
{
  gchar       *filename;
  gchar       *tmp_filename;
  gint         fd;
  const gchar *str;
  gboolean     ret;

  str = NULL;

  switch (state)
    {
    case MARKUP_WRITE_STATE_INIT:
      break;
    case MARKUP_WRITE_STATE_OK:
      str = STATE_OK;
      break;
    case MARKUP_WRITE_STATE_WRITING_V1:
      str = STATE_WRITING_V1;
      break;
    case MARKUP_WRITE_STATE_WRITING_V2:
      str = STATE_WRITING_V2;
      break;
    }

  /* The daemon dir is already created by the daemon on startup. Use O_SYNC so
   * that we know that the file is really written and use a temp file and move
   * it after it's written so we don't change the state to an empty file in case
   * of failure.
   */
  
  filename = write_state_get_filename ();

  if (state == MARKUP_WRITE_STATE_INIT)
    {
      unlink (filename);
      g_free (filename);
      return TRUE;
    }
  
  tmp_filename = g_strconcat (filename, ".tmp", NULL);
  fd = open (tmp_filename, O_SYNC | O_CREAT | O_WRONLY, 0666);

 try_again:
  
  if (fd < 0)
    {
      gconf_log (GCL_ERR, "Could not open state file for writing %s: %s",
	filename, g_strerror (errno));
      
      g_free (filename);
      g_free (tmp_filename);
      return FALSE;
    }

  if (write (fd, str, STATE_STRING_LEN) != STATE_STRING_LEN)
    {
      if (errno == EINTR)
	goto try_again;
      else
	ret = FALSE;
    }
  else
    ret = TRUE;

  close (fd);

  if (ret)
    ret = rename (tmp_filename, filename) == 0;

  if (!ret)
    goto try_again;
  
  g_free (filename);
  g_free (tmp_filename);

  return ret;
}

static gboolean
markup_write_state_rm (const gchar *path)
{
  struct stat  buf;
  GDir        *dir;
  const gchar *name;
  gchar       *child;
  gboolean     ret;

  stat (path, &buf);

  if (S_ISDIR (buf.st_mode))
    {
      dir = g_dir_open (path, 0, NULL);      

      /* Recurse. */
      while ((name = g_dir_read_name (dir)))
	{
	  child = g_build_filename (path, name, NULL);
	  markup_write_state_rm (child);
	}
      
      g_dir_close (dir);

      /* And the remove self. */
      ret = rmdir (path) == 0;
    }
  else
    {
      ret = unlink (path) == 0;
    }
  
  return ret;
}

static gsize
safe_read (int fd, guchar *buf, size_t count)
{
  gssize num_read;

  do
    {
      num_read = read (fd, buf, count);
    }
  while (num_read < 0 && errno == EINTR);
  
  return num_read;
}

static gsize
safe_write (int fd, guchar *buf, size_t count)
{
  gssize num_written;

  do
    {
      num_written = write (fd, buf, count);
    }
  while (num_written < 0 && errno == EINTR);
  
  return num_written;
}


static gboolean
markup_write_state_copy_file (const gchar *src_path,
			      const gchar *dst_path,
			      mode_t       mode)
{
  gint      src_fd, dst_fd;
  gchar    *tmp_dst_path;
  gssize    num_read;
  guchar    buf[4092];
  gboolean  ret;
  
  src_fd = open (src_path, O_RDONLY);
  if (src_fd < 0)
    return FALSE;

  tmp_dst_path = g_strconcat (dst_path, ".tmp", NULL);;

  dst_fd = open (tmp_dst_path, O_SYNC | O_CREAT | O_WRONLY, mode);
  if (dst_fd < 0)
    {
      close (src_fd);
      g_free (tmp_dst_path);
      return FALSE;
    }

  while ((num_read = safe_read (src_fd, buf, 4092)) > 0)
    safe_write (dst_fd, buf, num_read);
  
  close (src_fd);
  close (dst_fd);

  if (num_read == -1)
    {
      unlink (tmp_dst_path);
      g_free (tmp_dst_path);
      return FALSE;
    }
  
  ret = (rename (tmp_dst_path, dst_path) == 0);

  g_free (tmp_dst_path);

  return ret;
}
  
static gboolean
markup_write_state_copy_recurse (const gchar *src_dir,
				 const gchar *dst_dir,
				 mode_t       mode)
{
  gint         errcode;
  GDir        *dir;
  const gchar *name;
  gchar       *full_src, *full_dst;
  struct stat  buf;
  gboolean     ret;
  
  errcode = mkdir (dst_dir, mode);
  if (errcode != 0)
    return FALSE;
  
  dir = g_dir_open (src_dir, 0, NULL);
  if (!dir) 
    return FALSE;

  ret = TRUE;
  
  while ((name = g_dir_read_name (dir)))
    {
      full_src = g_build_filename (src_dir, name, NULL);
      full_dst = g_build_filename (dst_dir, name, NULL);

      stat (full_src, &buf);
      mode = buf.st_mode & (S_IRWXU | S_IRWXG | S_IRWXG);
      if (S_ISDIR (buf.st_mode))
	{
	  if (!markup_write_state_copy_recurse (full_src, full_dst, mode))
	    {
	      g_free (full_src);
	      g_free (full_dst);
	      ret = FALSE;
	      break;
	    }
	}
      else if (S_ISREG (buf.st_mode))
	{
	  if (!markup_write_state_copy_file (full_src, full_dst, mode))
	    {
	      g_free (full_src);
	      g_free (full_dst);
	      ret = FALSE;
	      break;
	    }    
	}
      
      g_free (full_src);
      g_free (full_dst);
    }
  
  g_dir_close (dir);
  
  return ret;
}

static gboolean
markup_write_state_copy_dir (const gchar *src_dir,
			     const gchar *dst_dir)
{
  struct stat buf;
  mode_t      mode;
  
  /* First try to remove the dest dir. We don't care about the return value
   * really, the subsequent copying will catch any real errorrs.
   */
  markup_write_state_rm (dst_dir);

  stat (src_dir, &buf);
  if (!S_ISDIR (buf.st_mode))
    return FALSE;

  mode = buf.st_mode & (S_IRWXU | S_IRWXG | S_IRWXG);

  return markup_write_state_copy_recurse (src_dir, dst_dir, mode);
}

gboolean
markup_write_state_copy_to_v2 (const gchar *root_dir)
{
  gchar    *copy_dir;
  gboolean  ret;

  copy_dir = g_strconcat (root_dir, "-copy", NULL);

  d(g_print ("Copying to v2, %s -> %s\n", root_dir, copy_dir));

  ret = markup_write_state_copy_dir (root_dir, copy_dir);
  g_free (copy_dir);

  return ret;
}

gboolean
markup_write_state_copy_to_v1 (const gchar *root_dir)
{
  gchar    *copy_dir;
  gboolean  ret;
  
  copy_dir = g_strconcat (root_dir, "-copy", NULL);

  d(g_print ("Copying to v1, %s -> %s\n", copy_dir, root_dir));

  ret = markup_write_state_copy_dir (copy_dir, root_dir);
  g_free (copy_dir);

  return ret;
}

gboolean
markup_write_state_ensure_consistent (MarkupWriteState  current_state,
				      const gchar      *root_dir,
				      gboolean         *need_reload)
{
  gboolean ret;

  ret = FALSE;

  d(g_print ("\nEnsure consistent state\n"));
  
  switch (current_state)
    {
    case MARKUP_WRITE_STATE_INIT:
      /* This is the case where there is no state file, e.g. when starting the
       * first time. It's treated as if only v1 is OK.
       */
      d(g_print ("State is \"init\"\n"));
      ret = markup_write_state_copy_to_v2 (root_dir);
      break;
      
    case MARKUP_WRITE_STATE_OK:
      /* Both copies are OK, we are consistent. */
      d(g_print ("State is \"OK\"\n"));
      return TRUE;
      
    case MARKUP_WRITE_STATE_WRITING_V1:
      /* V2 is OK, copy to V1. */
      d(g_print ("State is \"writing to v1\"\n"));
      ret = markup_write_state_copy_to_v1 (root_dir);
      if  (need_reload)
	*need_reload = TRUE;
      break;

    case MARKUP_WRITE_STATE_WRITING_V2:
      /* V1 is OK, copy to V2. */
      d(g_print ("State is \"writing to v2\"\n"));
      ret = markup_write_state_copy_to_v2 (root_dir);
      break;
    }

  /* If we successfully copied above, the state is now OK. */
  if (ret)
    ret = markup_write_state_write (MARKUP_WRITE_STATE_OK);
  
  if (!ret && need_reload)
    *need_reload = FALSE;
  
  return ret;
}

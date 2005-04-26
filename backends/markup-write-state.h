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

#ifndef MARKUP_WRITE_STATE_H
#define MARKUP_WRITE_STATE_H

#include <glib.h>

typedef enum {
  MARKUP_WRITE_STATE_OK,         /* OK, both v1 and v2 are up to date. */
  MARKUP_WRITE_STATE_WRITING_V1, /* V2 is up to date. */
  MARKUP_WRITE_STATE_WRITING_V2, /* V1 is up to date. */
  MARKUP_WRITE_STATE_INIT        /* Initial state, handle as V1 is up to date. */
} MarkupWriteState;

MarkupWriteState markup_write_state_read              (void);
gboolean         markup_write_state_write             (MarkupWriteState  state);
gboolean         markup_write_state_copy_to_v2        (const gchar      *root_dir);
gboolean         markup_write_state_copy_to_v1        (const gchar      *root_dir);
gboolean         markup_write_state_ensure_consistent (MarkupWriteState  current_state,
						       const gchar      *root_dir,
						       gboolean         *need_reload);

#endif /* MARKUP_WRITE_STATE_H */

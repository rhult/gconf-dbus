/* GConf
 * Copyright (C) 1999, 2000 Red Hat Inc.
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

#ifndef GCONF_GCONF_CORBA_H
#define GCONF_GCONF_CORBA_H

#include "GConfX.h"
#include "gconf-internals.h"
#include "gconf-value.h"
#include "gconf-schema.h"

GConfValue*   gconf_value_from_corba_value              (const ConfigValue   *value);
ConfigValue*  gconf_corba_value_from_gconf_value        (const GConfValue    *value);
void          gconf_fill_corba_value_from_gconf_value   (const GConfValue    *value,
							 ConfigValue         *dest);
ConfigValue*  gconf_invalid_corba_value                 (void);
void          gconf_fill_corba_schema_from_gconf_schema (const GConfSchema   *sc,
							 ConfigSchema        *dest);
ConfigSchema* gconf_corba_schema_from_gconf_schema      (const GConfSchema   *sc);
GConfSchema*  gconf_schema_from_corba_schema            (const ConfigSchema  *cs);
void          gconf_daemon_blow_away_locks              (void);
GConfLock*    gconf_get_lock_or_current_holder          (const gchar         *lock_directory,
							 ConfigServer        *current_server,
							 GError             **err);
ConfigServer  gconf_get_current_lock_holder             (const gchar         *lock_directory,
							 GString             *failure_log);
gchar*        gconf_object_to_string                    (CORBA_Object         obj,
							 GError             **err);
gboolean      gconf_CORBA_Object_equal                  (gconstpointer        a,
							 gconstpointer        b);
guint         gconf_CORBA_Object_hash                   (gconstpointer        key);
ConfigServer  gconf_activate_server                     (gboolean             start_if_not_found,
							 GError             **error);
void          gconf_set_daemon_ior                      (const gchar         *ior);
const gchar*  gconf_get_daemon_ior                      (void);
CORBA_ORB     gconf_orb_get                             (void);
int           gconf_orb_release                         (void);
char*         gconf_get_lock_dir                        (void);
char*         gconf_get_daemon_dir                      (void);
gboolean      gconf_release_lock                        (GConfLock           *lock,
							 GError             **err);


#endif 

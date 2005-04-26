/* GConf
 * Copyright (C) 1999, 2000 Red Hat Inc.
 * Copyright (C) 2003       CodeFactory AB
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

#ifndef GCONF_GCONFD_CORBA_H
#define GCONF_GCONFD_CORBA_H

#include "GConfX.h"
#include "gconf-database.h"

gboolean gconfd_corba_init (void);

PortableServer_POA gconf_corba_get_poa ();

GConfValue*  gconf_value_from_corba_value            (const ConfigValue *value);
ConfigValue* gconf_corba_value_from_gconf_value      (const GConfValue  *value);
void         gconf_fill_corba_value_from_gconf_value (const GConfValue  *value,
                                                      ConfigValue       *dest);
ConfigValue* gconf_invalid_corba_value               (void);

void          gconf_fill_corba_schema_from_gconf_schema (const GConfSchema  *sc,
                                                         ConfigSchema       *dest);
ConfigSchema* gconf_corba_schema_from_gconf_schema      (const GConfSchema  *sc);
GConfSchema*  gconf_schema_from_corba_schema            (const ConfigSchema *cs);

gchar*    gconf_object_to_string (CORBA_Object   obj,
				  GError       **err);

gboolean gconfd_logfile_change_listener (GConfDatabase *db,
                                         gboolean add,
                                         guint connection_id,
                                         ConfigListener listener,
                                         const gchar *where,
                                         GError **err);

void gconfd_corba_logfile_save (void);
void gconfd_corba_logfile_read (void);
void gconfd_corba_log_client_add (const ConfigListener client);
void gconfd_corba_log_client_remove (const ConfigListener client);

void     gconfd_corba_drop_old_clients (void);
guint    gconfd_corba_client_count     (void);
gboolean gconf_corba_set_exception      (GError            **error,
					 CORBA_Environment  *ev);
gboolean gconfd_corba_check_in_shutdown (CORBA_Environment  *ev);



#endif

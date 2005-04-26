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
#ifndef GCONF_GCONF_DATABASE_CORBA_H
#define GCONF_GCONF_DATABASE_CORBA_H

#include "gconf-database.h"
#include "GConfX.h"

typedef struct _GConfDatabaseCorba  GConfDatabaseCorba;

void gconf_database_corba_init   (GConfDatabase *db);
void gconf_database_corba_deinit (GConfDatabase *db);

ConfigDatabase gconf_database_corba_get_objref (GConfDatabase *db);

void                gconf_database_corba_drop_dead_listeners (GConfDatabase *db);

CORBA_unsigned_long gconf_database_corba_add_listener     (GConfDatabase       *db,
							   ConfigListener       who,
							   const char          *name,
							   const gchar         *where);
void                gconf_database_corba_remove_listener  (GConfDatabase       *db,
							   CORBA_unsigned_long  cnxn);

CORBA_unsigned_long gconf_database_corba_readd_listener   (GConfDatabase       *db,
							   ConfigListener       who,
							   const char          *name,
							   const gchar         *where);

void                gconf_database_corba_notify_listeners (GConfDatabase       *db,
							   const gchar         *key,
							   const GConfValue    *value,
							   gboolean             is_default,
							   gboolean             is_writable);


#endif


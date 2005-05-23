/* -*- mode: C; c-file-style: "gnu" -*- */

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

#include <config.h>

#include <popt.h>
#include "gconf.h"
#include "gconf-dbus-utils.h"
#include "gconf-internals.h"
#include "gconf-sources.h"
#include "gconf-locale.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <unistd.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>

#define d(x) 

#define BUS_RULE    "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus'"
#define NOTIFY_RULE "type='method_call',interface='org.gnome.GConf.Database'"

struct _GConfEngine {
  guint refcount;

  gchar *database;

  GHashTable *notify_dirs;
  GHashTable *notify_ids;

  /* If non-NULL, this is a local engine;
     local engines don't do notification! */
  GConfSources* local_sources;
  
  /* An address if this is not the default engine;
   * NULL if it's the default
   */
  gchar *address;

  gpointer user_data;
  GDestroyNotify dnotify;

  gpointer owner;
  int owner_use_count;
  
  guint is_default : 1;

  /* If TRUE, this is a local engine (and therefore
   * has no ctable and no notifications)
   */
  guint is_local : 1;
};

typedef struct {
  gchar* namespace_section;
  guint client_id;

  GConfEngine* conf;             /* Engine we're associated with */
  GConfNotifyFunc func;
  gpointer user_data;
} GConfCnxn;

typedef struct {
  GList *cnxns;     /* List of connections to be notified below the dir */
} CnxnsData;


static DBusConnection *global_conn = NULL;
static gboolean        service_running = FALSE;
static gboolean        needs_reconnect = FALSE;
static GConfEngine    *default_engine = NULL;
static GHashTable     *engines_by_db = NULL;
static GHashTable     *engines_by_address = NULL;

static gboolean     ensure_dbus_connection      (void);
static gboolean     ensure_service              (gboolean          start_if_not_found,
						 GError          **err);
static gboolean     ensure_database             (GConfEngine      *conf,
						 gboolean          start_if_not_found,
						 GError          **err);
static void         gconf_engine_detach         (GConfEngine      *conf);
static void         gconf_engine_set_database   (GConfEngine      *conf,
						 const gchar      *db);
static const gchar *gconf_engine_get_database   (GConfEngine      *conf,
						 gboolean          start_if_not_found,
						 GError          **err);



static void         register_engine             (GConfEngine      *conf);
static void         unregister_engine           (GConfEngine      *conf);
static GConfCnxn *  gconf_cnxn_new              (GConfEngine      *conf,
						 const gchar      *namespace_section,
						 GConfNotifyFunc   func,
						 gpointer          user_data);
static void         gconf_cnxn_destroy          (GConfCnxn        *cnxn);
static void         gconf_cnxn_notify           (GConfCnxn        *cnxn,
						 GConfEntry       *entry);

static GConfCnxn *  gconf_cnxn_lookup_id        (GConfEngine      *conf,
						 guint             client_id);
static GList *      gconf_cnxn_lookup_dir       (GConfEngine      *conf,
						 const gchar      *dir);
static void         gconf_cnxn_insert           (GConfEngine      *conf,
						 const gchar      *dir,
						 guint              client_id,
						 GConfCnxn        *cnxn);
static void         gconf_cnxn_remove           (GConfEngine      *conf,
						 GConfCnxn        *cnxn);
static gboolean     send_notify_add             (GConfEngine      *conf,
						 GConfCnxn        *cnxn,
						 GError          **err);
static void         reinitialize_databases      (void);
static DBusHandlerResult
gconf_dbus_message_filter                       (DBusConnection   *dbus_conn,
						 DBusMessage      *message,
						 gpointer          user_data);
static GConfEngine *lookup_engine_by_address    (const gchar      *address);
static GConfEngine *lookup_engine_by_database   (const gchar      *db);
static gboolean     gconf_handle_dbus_exception (DBusMessage      *message,
						 DBusError        *derr,
						 GError          **gerr);
static void         gconf_detach_config_server  (void);
static DBusHandlerResult
                    handle_notify               (DBusConnection   *connection,
						 DBusMessage      *message,
						 GConfEngine      *conf);


#define CHECK_OWNER_USE(engine)   \
  do { if ((engine)->owner && (engine)->owner_use_count == 0) \
     g_warning ("%s: You can't use a GConfEngine that has an active GConfClient wrapper object. Use GConfClient API instead.", G_GNUC_FUNCTION);  \
  } while (0)



static GConfError
dbus_error_name_to_gconf_errno (const char *name)
{
  int i;
  struct
  {
    const char *name;
    GConfError error;
  } errors [] = {
    { GCONF_DBUS_ERROR_FAILED, GCONF_ERROR_FAILED },
    { GCONF_DBUS_ERROR_NO_PERMISSION, GCONF_ERROR_NO_PERMISSION },
    { GCONF_DBUS_ERROR_BAD_ADDRESS, GCONF_ERROR_BAD_ADDRESS },
    { GCONF_DBUS_ERROR_BAD_KEY, GCONF_ERROR_BAD_KEY },
    { GCONF_DBUS_ERROR_PARSE_ERROR, GCONF_ERROR_PARSE_ERROR },
    { GCONF_DBUS_ERROR_CORRUPT, GCONF_ERROR_CORRUPT },
    { GCONF_DBUS_ERROR_TYPE_MISMATCH, GCONF_ERROR_TYPE_MISMATCH },
    { GCONF_DBUS_ERROR_IS_DIR, GCONF_ERROR_IS_DIR },
    { GCONF_DBUS_ERROR_IS_KEY, GCONF_ERROR_IS_KEY },
    { GCONF_DBUS_ERROR_OVERRIDDEN, GCONF_ERROR_OVERRIDDEN },
    { GCONF_DBUS_ERROR_LOCK_FAILED, GCONF_ERROR_LOCK_FAILED },
    { GCONF_DBUS_ERROR_NO_WRITABLE_DATABASE, GCONF_ERROR_NO_WRITABLE_DATABASE },
    { GCONF_DBUS_ERROR_IN_SHUTDOWN, GCONF_ERROR_IN_SHUTDOWN },
  };

  for (i = 0; i < G_N_ELEMENTS (errors); i++)
    {
      if (strcmp (name, errors[i].name) == 0)
	return errors[i].error;
    }

  g_assert_not_reached ();
  
  return GCONF_ERROR_SUCCESS;
}

/* If no error is detected, return FALSE with no side-effects. If an error is
 * detected, return TRUE, set gerr, unref message and free derr.
 */
static gboolean
gconf_handle_dbus_exception (DBusMessage *message, DBusError *derr, GError **gerr)
{
  char *error_string;
  const char *name;

  if (message == NULL)
    {
      if (derr && dbus_error_is_set (derr))
	{
	  if (gerr)
	    {
	      *gerr = gconf_error_new (GCONF_ERROR_NO_SERVER, _("D-BUS error: %s"),
				       derr->message);
	    }
	}
      else 
	{
	  if (gerr)
	    *gerr = gconf_error_new (GCONF_ERROR_FAILED, _("Unknown error"));
	}

      if (derr)
	dbus_error_free (derr);

      return TRUE;
    }
    
 if (dbus_message_get_type (message) != DBUS_MESSAGE_TYPE_ERROR)
    return FALSE;

  name = dbus_message_get_member (message);

  dbus_message_get_args (message, NULL,
			 DBUS_TYPE_STRING, &error_string,
			 0);
  
  if (g_str_has_prefix (name, "org.freedesktop.DBus.Error"))
    {
      if (gerr)
	*gerr = gconf_error_new (GCONF_ERROR_NO_SERVER, _("D-BUS error: %s"),
				error_string);
    }
  else if (g_str_has_prefix (name, "org.gnome.GConf.Error"))
    {
      if (gerr)
	{
	  GConfError en;
	  
	  en = dbus_error_name_to_gconf_errno (name);
	  *gerr = gconf_error_new (en, error_string);
	}
    }
  else
    {
      if (gerr)
	*gerr = gconf_error_new (GCONF_ERROR_FAILED, _("Unknown error %s: %s"),
				 name, error_string);
    }

  dbus_message_unref (message);
  
  return TRUE;
}

static GConfEngine*
gconf_engine_blank (gboolean remote)
{
  GConfEngine* conf;

  _gconf_init_i18n ();
  
  conf = g_new0 (GConfEngine, 1);

  conf->refcount = 1;
  
  conf->owner = NULL;
  conf->owner_use_count = 0;
  
  if (remote)
    {
      conf->database = NULL;

      conf->notify_dirs = g_hash_table_new_full (g_str_hash, g_str_equal,
						 g_free, NULL);

      conf->notify_ids = g_hash_table_new (NULL, NULL);
      
      conf->local_sources = NULL;
      conf->is_local = FALSE;
      conf->is_default = TRUE;
    }
  else
    {
      conf->database = NULL;
      conf->notify_ids = NULL;
      conf->notify_dirs = NULL;
      conf->local_sources = NULL;
      conf->is_local = TRUE;
      conf->is_default = FALSE;
    }
  
  return conf;
}

void
gconf_engine_set_owner (GConfEngine *engine,
                        gpointer     client)
{
  g_return_if_fail (engine->owner_use_count == 0);
  
  engine->owner = client;
}

void
gconf_engine_push_owner_usage (GConfEngine *engine,
                               gpointer     client)
{
  g_return_if_fail (engine->owner == client);

  engine->owner_use_count += 1;
}

void
gconf_engine_pop_owner_usage  (GConfEngine *engine,
                               gpointer     client)
{
  g_return_if_fail (engine->owner == client);
  g_return_if_fail (engine->owner_use_count > 0);

  engine->owner_use_count -= 1;
}

static GConfEngine *
lookup_engine_by_database (const gchar *db)
{
  if (engines_by_db)
    return g_hash_table_lookup (engines_by_db, db);
  else
    return NULL;
}

static void
database_hash_value_destroy (gpointer value)
{
  GConfEngine *conf = value;

  g_free (conf->database);
  conf->database = NULL;
}

static void
gconf_engine_set_database (GConfEngine *conf,
                           const gchar *db)
{
  gconf_engine_detach (conf);

  conf->database = g_strdup (db);

  if (engines_by_db == NULL)
    engines_by_db = g_hash_table_new_full (g_str_hash,
					   g_str_equal,
					   NULL,
					   database_hash_value_destroy);
  
  g_hash_table_insert (engines_by_db, conf->database, conf);  
}

static void
gconf_engine_detach (GConfEngine *conf)
{
  if (conf->database != NULL)
    {
      g_hash_table_remove (engines_by_db, conf->database);
    }
}

static gboolean
ensure_dbus_connection (void)
{
  const gchar *address;
  DBusError error;

  if (global_conn != NULL)
    return TRUE;
 
#ifdef USE_SYSTEM_BUS
  address = g_getenv ("DBUS_SYSTEM_BUS_ADDRESS");

  if (!address)
    address = GCONF_SYSTEM_BUS_ADDRESS;
#else
  address = g_getenv ("DBUS_SESSION_BUS_ADDRESS");
#endif

  if (!address) {
    return FALSE;
  }

  dbus_error_init (&error);

  global_conn = dbus_connection_open (address, &error);
  if (!global_conn) 
    {
      g_warning ("Failed to connect to the D-BUS daemon: %s", error.message);
      
      dbus_error_free (&error);
      return FALSE;
    }
	
  if (!dbus_bus_register (global_conn, &error)) 
    {
      g_warning ("Failed to register with the D-BUS daemon: %s", error.message);
      
      dbus_connection_disconnect (global_conn);
      dbus_connection_unref (global_conn);
      
      global_conn = NULL;
		
      dbus_error_free (&error);
      return FALSE;
    }

  dbus_connection_setup_with_g_main (global_conn, NULL);

  dbus_bus_add_match (global_conn, BUS_RULE, NULL);
  dbus_bus_add_match (global_conn, NOTIFY_RULE, NULL);

  dbus_connection_add_filter (global_conn, gconf_dbus_message_filter,
			      NULL, NULL);
  
  return TRUE;
}

static gboolean
ensure_service (gboolean  start_if_not_found,
		GError   **err)
{
  DBusError error;
  DBusMessage *message, *reply;

  if (global_conn == NULL)
    {
      if (!ensure_dbus_connection ())
	{
	  g_set_error (err, GCONF_ERROR,
		       GCONF_ERROR_NO_SERVER,
		       _("No D-BUS daemon running\n"));
	  return FALSE;
	}

      g_assert (global_conn != NULL);
    }

  if (service_running)
    return TRUE;
  
  if (start_if_not_found)
    {
      d(g_print ("* activate_service, activating\n"));

      message = dbus_message_new_method_call (DBUS_SERVICE_ORG_FREEDESKTOP_DBUS,
					      DBUS_PATH_ORG_FREEDESKTOP_DBUS,
					      DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
					      "ActivateService");
      
      dbus_message_append_args (message,
				DBUS_TYPE_STRING, GCONF_DBUS_SERVICE,
				DBUS_TYPE_UINT32, 0,
				0);

      dbus_error_init (&error);
      
      reply = dbus_connection_send_with_reply_and_block (global_conn,
							 message, -1,
							 &error);
      
      dbus_message_unref (message);

      if (reply == NULL)
	{
	  const gchar *msg;
	  
	  if (dbus_error_is_set (&error))
	    msg = error.message;
	  else
	    msg = _("Unknown error");
	  
	  g_set_error (err, GCONF_ERROR,
		       GCONF_ERROR_NO_SERVER,
		       _("Failed to activate configuration server: %s\n"),
		       msg);

	  if (dbus_error_is_set (&error))
	    dbus_error_free (&error);
	  
	  return FALSE;
	}
      
      if (dbus_message_get_type (reply) == DBUS_MESSAGE_TYPE_ERROR)
	{
	  gchar *error_message;

	  dbus_message_get_args (reply, NULL,
				 DBUS_TYPE_STRING, &error_message,
				 0);
	  g_set_error (err, GCONF_ERROR,
		       GCONF_ERROR_NO_SERVER,
		       _("Failed to activate configuration server: %s\n"),
		       error_message);
	  dbus_free (error_message);
	  dbus_message_unref (reply);

	  return FALSE;
	}

      service_running = TRUE;
      
      return TRUE;
    }
  
  return FALSE;
}

static gboolean
ensure_database (GConfEngine *conf,
		 gboolean start_if_not_found,
		 GError **err)
{
  DBusMessage *message, *reply;
  DBusError error;
  gchar *db;

  g_return_val_if_fail (!conf->is_local, TRUE);

  if (!ensure_service (start_if_not_found, err))
    return FALSE;

  if (needs_reconnect)
    {
      /* Re-connect notifications and re-get database names from the previous
       * (if any) instance of the GConf service.
       */
      needs_reconnect = FALSE;
      reinitialize_databases ();
    }
  
  if (conf->database != NULL)
    return TRUE;
  
  if (conf->is_default)
    {
      message = dbus_message_new_method_call (GCONF_DBUS_SERVICE,
					      GCONF_DBUS_SERVER_OBJECT,
					      GCONF_DBUS_SERVER_INTERFACE,
					      GCONF_DBUS_SERVER_GET_DEFAULT_DB);
    }
  else
    {
      message = dbus_message_new_method_call (GCONF_DBUS_SERVICE,
					      GCONF_DBUS_SERVER_OBJECT,
					      GCONF_DBUS_SERVER_INTERFACE,
					      GCONF_DBUS_SERVER_GET_DB);
      dbus_message_append_args (message,
				DBUS_TYPE_STRING, conf->address,
				DBUS_TYPE_INVALID);
    }

  dbus_error_init (&error);
  reply = dbus_connection_send_with_reply_and_block (global_conn,
						     message, -1, &error);
  
  dbus_message_unref (message);
  
  if (gconf_handle_dbus_exception (reply, &error, err))
    return FALSE;

  dbus_message_get_args (reply,
			 NULL,
			 DBUS_TYPE_STRING, &db,
			 DBUS_TYPE_INVALID);

  dbus_message_unref (reply);

  if (db == NULL)
    {
      if (err)
        *err = gconf_error_new (GCONF_ERROR_BAD_ADDRESS,
				_("Server couldn't resolve the address `%s'"),
				conf->address ? conf->address : "default");
      
      return FALSE;
    }

  gconf_engine_set_database (conf, db);

  dbus_free (db);

  return TRUE;
}

static const gchar *
gconf_engine_get_database (GConfEngine *conf,
                           gboolean start_if_not_found,
                           GError **err)
{
  if (!ensure_database (conf, start_if_not_found, err))
    return NULL;
  else
    return conf->database;
}

static gboolean
gconf_engine_is_local (GConfEngine* conf)
{
  return conf->is_local;
}

static void
register_engine (GConfEngine *conf)
{
  g_return_if_fail (conf->address != NULL);

  if (engines_by_address == NULL)
    engines_by_address = g_hash_table_new (g_str_hash, g_str_equal);

  g_hash_table_insert (engines_by_address, conf->address, conf);
}

static void
unregister_engine (GConfEngine *conf)
{
  g_return_if_fail (conf->address != NULL);
  g_return_if_fail (engines_by_address != NULL);
  
  g_hash_table_remove (engines_by_address, conf->address);

  if (g_hash_table_size (engines_by_address) == 0)
    {
      g_hash_table_destroy (engines_by_address);
      
      engines_by_address = NULL;
    }
}

static GConfEngine *
lookup_engine_by_address (const gchar *address)
{
  if (engines_by_address)
    return g_hash_table_lookup (engines_by_address, address);
  else
    return NULL;
}


/*
 * Connection maintenance
 */

static GConfCnxn *
gconf_cnxn_new (GConfEngine *conf,
		const gchar *namespace_section,
		GConfNotifyFunc func,
		gpointer user_data)
{
  GConfCnxn *cnxn;
  static guint next_id = 1;
  
  cnxn = g_new0 (GConfCnxn, 1);

  cnxn->namespace_section = g_strdup (namespace_section);
  cnxn->conf = conf;
  cnxn->client_id = next_id;
  cnxn->func = func;
  cnxn->user_data = user_data;

  ++next_id;

  return cnxn;
}

static void      
gconf_cnxn_destroy (GConfCnxn* cnxn)
{
  g_free (cnxn->namespace_section);
  g_free (cnxn);
}

static void       
gconf_cnxn_notify (GConfCnxn* cnxn,
		   GConfEntry *entry)
{
  (*cnxn->func) (cnxn->conf, cnxn->client_id,
		 entry,
		 cnxn->user_data);
}

static GList *
gconf_cnxn_lookup_dir (GConfEngine *conf, const gchar *dir)
{
  CnxnsData *data;

  data = g_hash_table_lookup (conf->notify_dirs, dir);

  if (data == NULL)
    return NULL;

  return data->cnxns;
}

static GConfCnxn *
gconf_cnxn_lookup_id (GConfEngine *conf, guint client_id)
{
  gint id = client_id;

  return g_hash_table_lookup (conf->notify_ids, GINT_TO_POINTER (id));
}

static void
gconf_cnxn_insert (GConfEngine *conf, const gchar *dir, guint client_id, GConfCnxn *cnxn)
{
  CnxnsData *data;
  gint id = client_id;

  data = g_hash_table_lookup (conf->notify_dirs, dir);

  if (data == NULL)
    {
      data = g_new (CnxnsData, 1);
      data->cnxns = NULL;
      g_hash_table_insert (conf->notify_dirs, g_strdup (dir), data);
    }
  
  data->cnxns = g_list_prepend (data->cnxns, cnxn);

  g_hash_table_insert (conf->notify_ids, GINT_TO_POINTER (id), cnxn);
}

static void
gconf_cnxn_remove (GConfEngine *conf, GConfCnxn *cnxn)
{
  CnxnsData *data;
  gint id = cnxn->client_id;

  g_hash_table_remove (conf->notify_ids, GINT_TO_POINTER (id));

  data = g_hash_table_lookup (conf->notify_dirs, cnxn->namespace_section);
  if (data)
    {
      data->cnxns = g_list_remove (data->cnxns, cnxn);

      if (data->cnxns == NULL)
	{
	  g_hash_table_remove (conf->notify_dirs, cnxn->namespace_section);
	  g_free (data);

	  gconf_cnxn_destroy (cnxn);
	}
    }
}
  

/*
 *  Public Interface
 */

GConfEngine*
gconf_engine_get_local      (const gchar* address,
                             GError** err)
{
  GConfEngine *conf;
  GConfSource *source;

  g_return_val_if_fail (address != NULL, NULL);
  g_return_val_if_fail (err == NULL || *err == NULL, NULL);
  
  source = gconf_resolve_address (address, err);

  if (source == NULL)
    return NULL;
  
  conf = gconf_engine_blank (FALSE);

  conf->local_sources = gconf_sources_new_from_source (source);

  g_assert (gconf_engine_is_local (conf));
  
  return conf;
}

GConfEngine*
gconf_engine_get_default (void)
{
  GConfEngine* conf = NULL;
  
  if (default_engine)
    conf = default_engine;
  
  if (conf == NULL)
    {
      conf = gconf_engine_blank (TRUE);

      conf->is_default = TRUE;

      default_engine = conf;
    }
  else
    conf->refcount += 1;
  
  return conf;
}

GConfEngine*
gconf_engine_get_for_address (const gchar* address, GError** err)
{
  GConfEngine* conf;

  conf = lookup_engine_by_address (address);

  if (conf == NULL)
    {
      conf = gconf_engine_blank (TRUE);

      conf->is_default = FALSE;
      conf->address = g_strdup (address);

      if (!ensure_database (conf, TRUE, err))
        {
          gconf_engine_unref (conf);
          return NULL;
        }
      
      register_engine (conf);
    }
  else
    conf->refcount += 1;
  
  return conf;
}

void
gconf_engine_ref (GConfEngine* conf)
{
  g_return_if_fail (conf != NULL);
  g_return_if_fail (conf->refcount > 0);

  conf->refcount += 1;
}

void         
gconf_engine_unref (GConfEngine* conf)
{
  g_return_if_fail (conf != NULL);
  g_return_if_fail (conf->refcount > 0);

  conf->refcount -= 1;
  
  if (conf->refcount == 0)
    {
      if (gconf_engine_is_local (conf))
        {
          if (conf->local_sources != NULL)
            gconf_sources_free (conf->local_sources);
        }
      else
        {
          /* Remove all connections associated with this GConf */

	  /* FIXME: remove notify_ids from hash when we have
	     add/remove_notify. */

          if (conf->dnotify)
            {
              (* conf->dnotify) (conf->user_data);
            }
          
          /* do this after removing the notifications,
             to avoid funky race conditions */
          if (conf->address)
            unregister_engine (conf);

          /* Release the ConfigDatabase */
          gconf_engine_detach (conf);

          if (conf->notify_ids)
	    g_hash_table_destroy (conf->notify_ids);
          if (conf->notify_dirs)
	    g_hash_table_destroy (conf->notify_dirs);
        }
      
      if (conf == default_engine)
        default_engine = NULL;
      
      g_free(conf);
    }
}

void
gconf_engine_set_user_data  (GConfEngine   *engine,
                             gpointer       data,
                             GDestroyNotify dnotify)
{
  if (engine->dnotify)
    {
      (* engine->dnotify) (engine->user_data);
    }

  engine->dnotify = dnotify;
  engine->user_data = data;
}

gpointer
gconf_engine_get_user_data (GConfEngine   *engine)
{
  return engine->user_data;
}

static gboolean
send_notify_add (GConfEngine *conf,
		 GConfCnxn *cnxn,
		 GError **err)
{
  const gchar *db;
  DBusMessage *message, *reply;
  DBusError error;
    
  db = gconf_engine_get_database (conf, TRUE, err);
  
  if (db == NULL)
    {
      g_return_val_if_fail(err == NULL || *err != NULL, 0);
      
      return FALSE;
    }

  message = dbus_message_new_method_call (GCONF_DBUS_SERVICE,
					  db,
					  GCONF_DBUS_DATABASE_INTERFACE,
					  GCONF_DBUS_DATABASE_ADD_NOTIFY);
  
  dbus_message_append_args (message,
			    DBUS_TYPE_STRING, cnxn->namespace_section,
			    DBUS_TYPE_INVALID);

  dbus_error_init (&error);
  reply = dbus_connection_send_with_reply_and_block (global_conn,
						     message, -1, &error);
  dbus_message_unref (message);
  
  if (gconf_handle_dbus_exception (reply, &error, err))
    return FALSE;
  
  dbus_message_unref (reply);

  return TRUE;
}

guint
gconf_engine_notify_add (GConfEngine* conf,
			 const gchar* namespace_section,
			 GConfNotifyFunc func,
			 gpointer user_data,
			 GError** err)
{
  GConfCnxn *cnxn;
  
  g_return_val_if_fail (!gconf_engine_is_local (conf), 0);

  CHECK_OWNER_USE (conf);
  
  if (gconf_engine_is_local (conf))
    {
      if (err)
        *err = gconf_error_new (GCONF_ERROR_LOCAL_ENGINE,
				_("Can't add notifications to a local configuration source"));

      return 0;
    }

  cnxn = gconf_cnxn_new (conf, namespace_section, func, user_data);
  gconf_cnxn_insert (conf, namespace_section, cnxn->client_id, cnxn);
  
  if (!send_notify_add (conf, cnxn, err))
    {
      gconf_cnxn_remove (conf, cnxn);
      return 0;
    }

  return cnxn->client_id;
}

void         
gconf_engine_notify_remove (GConfEngine* conf,
			    guint client_id)
{
  GConfCnxn *cnxn;
  const gchar *db;
  gchar *namespace_section = NULL;
  DBusMessage *message, *reply;
  DBusError error;
  
  CHECK_OWNER_USE (conf);
  
  if (gconf_engine_is_local(conf))
    return;

  cnxn = gconf_cnxn_lookup_id (conf, client_id);
  if (cnxn != NULL)
    {
      namespace_section = g_strdup (cnxn->namespace_section);
      gconf_cnxn_remove (conf, cnxn);
    }
  
  g_return_if_fail (cnxn != NULL); 
  
  db = gconf_engine_get_database (conf, TRUE, NULL);
  
  if (db == NULL)
    return;

  d(g_print ("notify_remove, id = %d\n", client_id));
  
  message = dbus_message_new_method_call (GCONF_DBUS_SERVICE,
					  db,
					  GCONF_DBUS_DATABASE_INTERFACE,
					  GCONF_DBUS_DATABASE_REMOVE_NOTIFY);
  
  dbus_message_append_args (message,
			    DBUS_TYPE_STRING, namespace_section,
			    DBUS_TYPE_INVALID);

  dbus_error_init (&error);
  reply = dbus_connection_send_with_reply_and_block (global_conn, message, -1, &error);
  dbus_message_unref (message);
  
  if (dbus_error_is_set (&error))
    dbus_error_free (&error);
  
  g_free (namespace_section);
  
  dbus_message_unref (reply);
}

GConfValue *
gconf_engine_get_fuller (GConfEngine *conf,
                         const gchar *key,
                         const gchar *locale,
                         gboolean use_schema_default,
                         gboolean *is_default_p,
                         gboolean *is_writable_p,
                         gchar   **schema_name_p,
                         GError **err)
{
  GConfValue* val;
  const gchar *db;
  gboolean is_default = FALSE;
  gboolean is_writable = TRUE;
  gchar *schema_name = NULL;
  DBusMessage *message, *reply;
  DBusError error;
  DBusMessageIter iter;
  gboolean success;
  
  g_return_val_if_fail(conf != NULL, NULL);
  g_return_val_if_fail(key != NULL, NULL);
  g_return_val_if_fail(err == NULL || *err == NULL, NULL);

  CHECK_OWNER_USE (conf);
  
  if (!gconf_key_check (key, err))
    return NULL;

  if (gconf_engine_is_local (conf))
    {
      gchar **locale_list;
      
      locale_list = gconf_split_locale (locale);
      
      val = gconf_sources_query_value (conf->local_sources,
				       key,
				       (const gchar**)locale_list,
				       use_schema_default,
				       &is_default,
				       &is_writable,
				       schema_name_p ? &schema_name : NULL,
				       err);

      if (locale_list != NULL)
        g_strfreev(locale_list);
      
      if (is_default_p)
        *is_default_p = is_default;

      if (is_writable_p)
        *is_writable_p = is_writable;

      if (schema_name_p)
        *schema_name_p = schema_name;
      else
        g_free (schema_name);
      
      return val;
    }

  g_assert (!gconf_engine_is_local (conf));

  db = gconf_engine_get_database (conf, TRUE, err);

  if (db == NULL)
    {
      g_return_val_if_fail(err == NULL || *err != NULL, NULL);
      return NULL;
    }

  if (schema_name_p)
    *schema_name_p = NULL;

  message = dbus_message_new_method_call (GCONF_DBUS_SERVICE,
					  db,
					  GCONF_DBUS_DATABASE_INTERFACE,
					  GCONF_DBUS_DATABASE_LOOKUP_EXTENDED);
  
  dbus_message_append_args (message,
			    DBUS_TYPE_STRING, key,
			    DBUS_TYPE_STRING, (locale ? locale : gconf_current_locale()),
			    DBUS_TYPE_BOOLEAN, use_schema_default,
			    DBUS_TYPE_INVALID);

  dbus_error_init (&error);
  reply = dbus_connection_send_with_reply_and_block (global_conn, message, -1, &error);
  dbus_message_unref (message);

  if (gconf_handle_dbus_exception (reply, &error, err))
    return NULL;

  dbus_message_iter_init (reply, &iter);
  success = gconf_dbus_get_entry_values_from_message_iter (&iter,
							   NULL,
							   &val,
							   &is_default,
							   &is_writable,
							   &schema_name);
  
  dbus_message_unref (reply);
  
  if (!success)
    {
      if (err)
	g_set_error (err, GCONF_ERROR,
		     GCONF_ERROR_FAILED,
		     _("Couldn't get value"));
      
      g_free (schema_name);
      
      return NULL;
    }
  
  if (is_default_p)
    *is_default_p = !!is_default;
  
  if (is_writable_p)
    *is_writable_p = !!is_writable;
  
  if (schema_name && schema_name[0] != '/')
    {
      g_free (schema_name);
      schema_name = NULL;
    }
  
  if (schema_name_p)
    *schema_name_p = schema_name;
  else 
    g_free (schema_name);
  
  return val;
}

GConfValue *
gconf_engine_get_full (GConfEngine *conf,
                       const gchar *key,
                       const gchar *locale,
                       gboolean use_schema_default,
                       gboolean *is_default_p,
                       gboolean *is_writable_p,
                       GError **err)
{
  return gconf_engine_get_fuller (conf, key, locale, use_schema_default,
                                  is_default_p, is_writable_p,
                                  NULL, err);
}

GConfEntry*
gconf_engine_get_entry (GConfEngine* conf,
			const gchar* key,
			const gchar* locale,
			gboolean use_schema_default,
			GError** err)
{
  gboolean is_writable = TRUE;
  gboolean is_default = FALSE;
  GConfValue *val;
  GError *error;
  GConfEntry *entry;
  gchar *schema_name;

  CHECK_OWNER_USE (conf);
  
  schema_name = NULL;
  error = NULL;
  val = gconf_engine_get_fuller (conf, key, locale, use_schema_default,
                                 &is_default, &is_writable,
                                 &schema_name, &error);
  if (error != NULL)
    {
      g_propagate_error (err, error);
      return NULL;
    }

  entry = gconf_entry_new_nocopy (g_strdup (key), val);

  gconf_entry_set_is_default (entry, is_default);
  gconf_entry_set_is_writable (entry, is_writable);
  gconf_entry_set_schema_name (entry, schema_name);

  g_free (schema_name);

  return entry;
}
     
GConfValue*  
gconf_engine_get (GConfEngine* conf, const gchar* key, GError** err)
{
  return gconf_engine_get_with_locale (conf, key, NULL, err);
}

GConfValue*
gconf_engine_get_with_locale (GConfEngine* conf, const gchar* key,
			      const gchar* locale,
			      GError** err)
{
  return gconf_engine_get_full (conf, key, locale, TRUE,
				NULL, NULL, err);
}

GConfValue*
gconf_engine_get_without_default (GConfEngine* conf, const gchar* key,
				  GError** err)
{
  return gconf_engine_get_full (conf, key, NULL, FALSE, NULL, NULL, err);
}

GConfValue*
gconf_engine_get_default_from_schema (GConfEngine* conf,
                                      const gchar* key,
                                      GError** err)
{
  return gconf_engine_get_fuller (conf, key, NULL, 
				  TRUE, /* use_schema_default */
				  NULL, NULL, NULL, err);
}

gboolean
gconf_engine_set (GConfEngine* conf, const gchar* key,
                  const GConfValue* value, GError** err)
{
  const gchar *db;
  DBusMessage *message, *reply;
  DBusError error;

  g_return_val_if_fail(conf != NULL, FALSE);
  g_return_val_if_fail(key != NULL, FALSE);
  g_return_val_if_fail(value != NULL, FALSE);
  g_return_val_if_fail(value->type != GCONF_VALUE_INVALID, FALSE);
  g_return_val_if_fail( (value->type != GCONF_VALUE_STRING) ||
                        (gconf_value_get_string(value) != NULL) , FALSE );
  g_return_val_if_fail( (value->type != GCONF_VALUE_LIST) ||
                        (gconf_value_get_list_type(value) != GCONF_VALUE_INVALID), FALSE);
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);

  CHECK_OWNER_USE (conf);

  if (!gconf_key_check (key, err))
    return FALSE;

  if (!gconf_value_validate (value, err))
    return FALSE;
  
  if (gconf_engine_is_local (conf))
    {
      GError* error = NULL;
      
      gconf_sources_set_value (conf->local_sources, key, value, &error);

      if (error != NULL)
        {
          if (err)
            *err = error;
          else
            {
              g_error_free (error);
            }
          return FALSE;
        }
      
      return TRUE;
    }

  g_assert (!gconf_engine_is_local (conf));
  
  db = gconf_engine_get_database (conf, TRUE, err);

  if (db == NULL)
    {
      g_return_val_if_fail(err == NULL || *err != NULL, FALSE);
      
      return FALSE;
    }

  message = dbus_message_new_method_call (GCONF_DBUS_SERVICE,
					  db,
					  GCONF_DBUS_DATABASE_INTERFACE,
					  GCONF_DBUS_DATABASE_SET);
  
  dbus_message_append_args (message,
			    DBUS_TYPE_STRING, key,
			    DBUS_TYPE_INVALID);

  gconf_dbus_message_append_gconf_value (message, value);

  dbus_error_init (&error);
  reply = dbus_connection_send_with_reply_and_block (global_conn, message, -1, &error);
  dbus_message_unref (message);

  if (gconf_handle_dbus_exception (reply, &error, err))
    return FALSE;

  g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

  dbus_message_unref (reply);

  return TRUE;
}

gboolean
gconf_engine_unset (GConfEngine* conf, const gchar* key, GError** err)
{
  const gchar *db;
  DBusMessage *message, *reply;
  DBusError error;

  g_return_val_if_fail (conf != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);
  g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

  CHECK_OWNER_USE (conf);
  
  if (!gconf_key_check (key, err))
    return FALSE;

  if (gconf_engine_is_local (conf))
    {
      GError* error = NULL;
      
      gconf_sources_unset_value (conf->local_sources, key, NULL, &error);

      if (error != NULL)
        {
          if (err)
            *err = error;
          else
            {
              g_error_free (error);
            }
          return FALSE;
        }
      
      return TRUE;
    }

  g_assert(!gconf_engine_is_local(conf));
  
  db = gconf_engine_get_database (conf, TRUE, err);

  if (db == NULL)
    {
      g_return_val_if_fail(err == NULL || *err != NULL, FALSE);

      return FALSE;
    }

  message = dbus_message_new_method_call (GCONF_DBUS_SERVICE,
					  db,
					  GCONF_DBUS_DATABASE_INTERFACE,
					  GCONF_DBUS_DATABASE_UNSET);
  
  dbus_message_append_args (message,
			    DBUS_TYPE_STRING, key,
			    DBUS_TYPE_STRING, "",
			    DBUS_TYPE_INVALID);

  dbus_error_init (&error);
  reply = dbus_connection_send_with_reply_and_block (global_conn, message, -1, &error);
  dbus_message_unref (message);
  
  if (gconf_handle_dbus_exception (reply, &error, err))
    return FALSE;

  g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

  dbus_message_unref (reply);

  return TRUE;
}

/**
 * gconf_engine_recursive_unset:
 * @engine: a #GConfEngine
 * @key: a key or directory name
 * @flags: change how the unset is done
 * @err: return location for a #GError, or %NULL to ignore errors
 * 
 * Unsets all keys below @key, including @key itself.  If any unset
 * fails, continues on to unset as much as it can. The first
 * failure is returned in @err.
 *
 * Returns: %FALSE if error is set
 **/
gboolean
gconf_engine_recursive_unset (GConfEngine    *conf,
                              const char     *key,
                              GConfUnsetFlags flags,
                              GError        **err)
{
  const gchar *db;
  DBusMessage *message, *reply;
  DBusError error;
  guint dbus_flags;
  
  g_return_val_if_fail (conf != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);
  g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

  CHECK_OWNER_USE (conf);
  
  if (!gconf_key_check (key, err))
    return FALSE;

  if (gconf_engine_is_local (conf))
    {
      GError* error = NULL;
      
      gconf_sources_recursive_unset (conf->local_sources, key, NULL,
                                     flags, NULL, &error);

      if (error != NULL)
        {
          if (err)
            *err = error;
          else
            {
              g_error_free (error);
            }
          return FALSE;
        }
      
      return TRUE;
    }

  g_assert (!gconf_engine_is_local (conf));
  
  dbus_flags = 0;
  if (flags & GCONF_UNSET_INCLUDING_SCHEMA_NAMES)
    dbus_flags |= GCONF_DBUS_UNSET_INCLUDING_SCHEMA_NAMES;

  db = gconf_engine_get_database (conf, TRUE, err);

  if (db == NULL)
    {
      g_return_val_if_fail (err == NULL || *err != NULL, FALSE);

      return FALSE;
    }

  message = dbus_message_new_method_call (GCONF_DBUS_SERVICE,
					  db,
					  GCONF_DBUS_DATABASE_INTERFACE,
					  GCONF_DBUS_DATABASE_RECURSIVE_UNSET);
  
  dbus_message_append_args (message,
			    DBUS_TYPE_STRING, key,
			    DBUS_TYPE_STRING, "",
			    DBUS_TYPE_UINT32, dbus_flags,
			    DBUS_TYPE_INVALID);

  dbus_error_init (&error);
  reply = dbus_connection_send_with_reply_and_block (global_conn, message, -1, &error);
  dbus_message_unref (message);
  
  if (gconf_handle_dbus_exception (reply, &error, err))
    return FALSE;

  g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

  dbus_message_unref (reply);

  return TRUE;
}

gboolean
gconf_engine_associate_schema  (GConfEngine* conf, const gchar* key,
                                const gchar* schema_key, GError** err)
{
  const gchar *db;
  DBusMessage *message, *reply;
  DBusError error;
  
  g_return_val_if_fail (conf != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);
  g_return_val_if_fail (err == NULL || *err == NULL, FALSE);
  
  if (!gconf_key_check (key, err))
    return FALSE;

  if (schema_key && !gconf_key_check (schema_key, err))
    return FALSE;

  if (gconf_engine_is_local(conf))
    {
      GError* error = NULL;
      
      gconf_sources_set_schema (conf->local_sources, key, schema_key, &error);

      if (error != NULL)
        {
          if (err)
            *err = error;
          else
            {
              g_error_free(error);
            }
          return FALSE;
        }
      
      return TRUE;
    }

  g_assert (!gconf_engine_is_local (conf));
  
  db = gconf_engine_get_database (conf, TRUE, err);

  if (db == NULL)
    {
      g_return_val_if_fail (err == NULL || *err != NULL, FALSE);

      return FALSE;
    }

  message = dbus_message_new_method_call (GCONF_DBUS_SERVICE,
					  db,
					  GCONF_DBUS_DATABASE_INTERFACE,
					  GCONF_DBUS_DATABASE_SET_SCHEMA);

   /* Empty schema string means unset. */
  dbus_message_append_args (message,
			    DBUS_TYPE_STRING, key,
			    DBUS_TYPE_STRING, schema_key ? schema_key : "",
			    DBUS_TYPE_INVALID);

  dbus_error_init (&error);
  reply = dbus_connection_send_with_reply_and_block (global_conn, message, -1, &error);
  dbus_message_unref (message);
  
  if (gconf_handle_dbus_exception (reply, &error, err))
    return FALSE;
  
  g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

  dbus_message_unref (reply);

  return TRUE;
}

static void
qualify_entries (GSList *entries, const char *dir)
{
  GSList *tmp = entries;
  
  while (tmp != NULL)
    {
      GConfEntry *entry = tmp->data;
      gchar *full;

      full = gconf_concat_dir_and_key (dir, entry->key);

      g_free (entry->key);
      entry->key = full;

      tmp = g_slist_next (tmp);
    }
}

GSList*      
gconf_engine_all_entries (GConfEngine* conf, const gchar* dir, GError** err)
{
  GSList* entries = NULL;
  const gchar *db;
  DBusMessage *message, *reply;
  DBusError error;
  DBusMessageIter iter;
  gchar *key, *schema_name;
  gboolean is_default;
  gboolean is_writable;

  g_return_val_if_fail(conf != NULL, NULL);
  g_return_val_if_fail(dir != NULL, NULL);
  g_return_val_if_fail(err == NULL || *err == NULL, NULL);

  CHECK_OWNER_USE (conf);
  
  if (!gconf_key_check(dir, err))
    return NULL;

  if (gconf_engine_is_local(conf))
    {
      GError* error = NULL;
      gchar** locale_list;
      GSList* retval;
      
      locale_list = gconf_split_locale(gconf_current_locale());
      
      retval = gconf_sources_all_entries(conf->local_sources,
                                         dir,
                                         (const gchar**)locale_list,
                                         &error);

      if (locale_list)
        g_strfreev(locale_list);
      
      if (error != NULL)
        {
          if (err)
            *err = error;
          else
            {
              g_error_free(error);
            }

          g_assert(retval == NULL);
          
          return NULL;
        }

      qualify_entries (retval, dir);
      
      return retval;
    }

  g_assert(!gconf_engine_is_local(conf));
  
  db = gconf_engine_get_database (conf, TRUE, err);

  if (db == NULL)
    {
      g_return_val_if_fail(err == NULL || *err != NULL, NULL);

      return NULL;
    }

  message = dbus_message_new_method_call (GCONF_DBUS_SERVICE,
					  db,
					  GCONF_DBUS_DATABASE_INTERFACE,
					  GCONF_DBUS_DATABASE_GET_ALL_ENTRIES);

  dbus_message_append_args (message,
			    DBUS_TYPE_STRING, dir,
			    DBUS_TYPE_STRING, gconf_current_locale (),
			    DBUS_TYPE_INVALID);
  
  dbus_error_init (&error);
  reply = dbus_connection_send_with_reply_and_block (global_conn, message, -1, &error);
  dbus_message_unref (message);
  
  if (gconf_handle_dbus_exception (reply, &error, err))
    return FALSE;
  
  g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

  dbus_message_iter_init (reply, &iter);

  while (1)
    {
      GConfEntry *entry;
      GConfValue *value;

      if (!gconf_dbus_get_entry_values_from_message_iter (&iter,
							  &key,
							  &value,
							  &is_default,
							  &is_writable,
							  &schema_name))
	break;

      entry =  gconf_entry_new_nocopy (gconf_concat_dir_and_key (dir, key),
				       value);
      
      gconf_entry_set_is_default (entry, is_default);
      gconf_entry_set_is_writable (entry, is_writable);

      /* Empty string means no schema name. */
      if (*schema_name != '\0')
	gconf_entry_set_schema_name (entry, schema_name);

      entries = g_slist_prepend (entries, entry);

      g_free (key);
      g_free (schema_name);

      if (!dbus_message_iter_next (&iter))
	break;
    }

  dbus_message_unref (reply);

  return entries;
}

static void
qualify_keys (GSList *keys, const char *dir)
{
  GSList *tmp = keys;
  while (tmp != NULL)
    {
      char *key = tmp->data;
      gchar *full;

      full = gconf_concat_dir_and_key (dir, key);

      g_free (tmp->data);
      tmp->data = full;

      tmp = g_slist_next (tmp);
    }
}

GSList*      
gconf_engine_all_dirs(GConfEngine* conf, const gchar* dir, GError** err)
{
  GSList* subdirs = NULL;
  const gchar *db;
  DBusMessage *message, *reply;
  DBusError error;
  DBusMessageIter iter;
  gchar *key;
  
  g_return_val_if_fail(conf != NULL, NULL);
  g_return_val_if_fail(dir != NULL, NULL);
  g_return_val_if_fail(err == NULL || *err == NULL, NULL);
  
  CHECK_OWNER_USE (conf);
  
  if (!gconf_key_check(dir, err))
    return NULL;
  
  if (gconf_engine_is_local(conf))
    {
      GError* error = NULL;
      GSList* retval;
      
      retval = gconf_sources_all_dirs(conf->local_sources,
                                      dir,
                                      &error);
      
      if (error != NULL)
        {
          if (err)
            *err = error;
          else
            {
              g_error_free(error);
            }

          g_assert(retval == NULL);
          
          return NULL;
        }
      
      qualify_keys (retval, dir);
      
      return retval;
    }
  
  g_assert(!gconf_engine_is_local(conf));

  db = gconf_engine_get_database (conf, TRUE, err);

  if (db == NULL)
    {
      g_return_val_if_fail (err == NULL || *err != NULL, NULL);
      
      return NULL;
    }
  
  message = dbus_message_new_method_call (GCONF_DBUS_SERVICE,
					  db,
					  GCONF_DBUS_DATABASE_INTERFACE,
					  GCONF_DBUS_DATABASE_GET_ALL_DIRS);

  dbus_message_append_args (message,
			    DBUS_TYPE_STRING, dir,
			    DBUS_TYPE_INVALID);
  
  dbus_error_init (&error);
  reply = dbus_connection_send_with_reply_and_block (global_conn, message, -1, &error);
  dbus_message_unref (message);
  
  if (gconf_handle_dbus_exception (reply, &error, err))
    return FALSE;

  g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

  dbus_message_iter_init (reply, &iter);

  while (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_STRING)
    {
      gchar* s;
            
      key = dbus_message_iter_get_string (&iter);
      
      s = gconf_concat_dir_and_key (dir, key);
      subdirs = g_slist_prepend (subdirs, s);
      
      dbus_free (key);
      
      if (!dbus_message_iter_next (&iter))
	break;
    }
  
  dbus_message_unref (reply);

  return subdirs;
}

/* annoyingly, this is REQUIRED for local sources */
void 
gconf_engine_suggest_sync(GConfEngine* conf, GError** err)
{
  g_return_if_fail(conf != NULL);
  g_return_if_fail(err == NULL || *err == NULL);

  CHECK_OWNER_USE (conf);
  
  if (gconf_engine_is_local(conf))
    {
      GError* error = NULL;
      
      gconf_sources_sync_all(conf->local_sources,
                             &error);
      
      if (error != NULL)
        {
          if (err)
            *err = error;
          else
            {
              g_error_free(error);
            }
          return;
        }
      
      return;
    }

  /* Do nothing for non-local case. */
}

void 
gconf_clear_cache(GConfEngine* conf, GError** err)
{
  g_return_if_fail(conf != NULL);
  g_return_if_fail(err == NULL || *err == NULL);

  /* don't disallow non-owner use here since you can't do this
   * via GConfClient API and calling this function won't break
   * GConfClient anyway
   */
  
  if (gconf_engine_is_local(conf))
    {
      GError* error = NULL;
      
      gconf_sources_clear_cache(conf->local_sources);
      
      if (error != NULL)
        {
          if (err)
            *err = error;
          else
            {
              g_error_free(error);
            }
          return;
        }
      
      return;
    }

  /* Do nothing for non-local case. */
}

void 
gconf_synchronous_sync(GConfEngine* conf, GError** err)
{
  g_return_if_fail(conf != NULL);
  g_return_if_fail(err == NULL || *err == NULL);

  if (gconf_engine_is_local(conf))
    {
      GError* error = NULL;
      
      gconf_sources_sync_all(conf->local_sources, &error);
      
      if (error != NULL)
        {
          if (err)
            *err = error;
          else
            {
              g_error_free(error);
            }
          return;
        }
      
      return;
    }

  /* Do nothing for non-local case. */
}

gboolean
gconf_engine_dir_exists (GConfEngine *conf, const gchar *dir, GError** err)
{
  const gchar *db;
  dbus_bool_t exists;
  DBusMessage *message, *reply;
  DBusError error;
  
  g_return_val_if_fail(conf != NULL, FALSE);
  g_return_val_if_fail(dir != NULL, FALSE);
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);

  CHECK_OWNER_USE (conf);
  
  if (!gconf_key_check(dir, err))
    return FALSE;
  
  if (gconf_engine_is_local(conf))
    {
      return gconf_sources_dir_exists(conf->local_sources,
                                      dir,
                                      err);
    }

  g_assert(!gconf_engine_is_local(conf));
  
  db = gconf_engine_get_database(conf, TRUE, err);
  
  if (db == NULL)
    {
      g_return_val_if_fail(err == NULL || *err != NULL, FALSE);

      return FALSE;
    }

  message = dbus_message_new_method_call (GCONF_DBUS_SERVICE,
					  db,
					  GCONF_DBUS_DATABASE_INTERFACE,
					  GCONF_DBUS_DATABASE_DIR_EXISTS);
  
  dbus_message_append_args (message,
			    DBUS_TYPE_STRING, dir,
			    DBUS_TYPE_INVALID);
  
  dbus_error_init (&error);
  reply = dbus_connection_send_with_reply_and_block (global_conn, message, -1, &error);
  dbus_message_unref (message);
 
  if (gconf_handle_dbus_exception (reply, &error, err))
    return FALSE;

  g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

  dbus_message_get_args (reply,
			 &error,
			 DBUS_TYPE_BOOLEAN, &exists,
			 DBUS_TYPE_INVALID);

  return !!exists;
}

void
gconf_engine_remove_dir (GConfEngine* conf,
                         const gchar* dir,
                         GError** err)
{
  g_return_if_fail(conf != NULL);
  g_return_if_fail(dir != NULL);
  g_return_if_fail(err == NULL || *err == NULL);

  /* FIXME we have no GConfClient method for doing this */
  /*   CHECK_OWNER_USE (conf); */
  
  if (!gconf_key_check(dir, err))
    return;

  if (gconf_engine_is_local(conf))
    {
      gconf_sources_remove_dir(conf->local_sources, dir, err);
      return;
    }
}

static void
cnxn_get_all_func (gpointer key,
		   gpointer value,
		   gpointer user_data)
{
  GList **list = user_data;
  
  *list = g_list_prepend (*list, value);
}

static void
engines_by_db_get_all_func (gpointer key,
			    gpointer value,
			    gpointer user_data)
{
  GList **list = user_data;

  *list = g_list_prepend (*list, value);
}

static void
reinitialize_databases (void)
{
  GList *engines = NULL, *engine;
  GList *cnxns, *l;
  GConfEngine *conf;
  
  if (engines_by_db)
    g_hash_table_foreach (engines_by_db,
			  engines_by_db_get_all_func,
			  &engines);

  /* Reset databases. */
  for (engine = engines; engine; engine = engine->next)
    {
      conf = engine->data;

      g_hash_table_remove (engines_by_db, conf->database);  
      ensure_database (conf, FALSE, NULL);
    }
  
  /* Re-add notifications. */
  for (engine = engines; engine; engine = engine->next)
    {
      conf = engine->data;
      
      cnxns = NULL;
      g_hash_table_foreach (conf->notify_ids,
			    cnxn_get_all_func,
			    &cnxns);
      
      for (l = cnxns; l; l = l->next)
	{
	  GConfCnxn *cnxn = l->data;
	  
	  send_notify_add (conf, cnxn, NULL);
	}
      
      g_list_free (cnxns);
    }
  
  g_list_free (engines);
}

static DBusHandlerResult
gconf_dbus_message_filter (DBusConnection    *dbus_conn,
			   DBusMessage       *message,
			   gpointer           user_data)
{
  DBusMessageIter iter;
  char *service;

#if 0
  d(g_print ("Message: %s, from %s\n",
	     dbus_message_get_member (message),
	     dbus_message_get_sender (message)));
#endif
  
  if (dbus_message_is_method_call (message,
				   GCONF_DBUS_CLIENT_INTERFACE,
				   "Notify"))
    {
      return handle_notify (dbus_conn, message, NULL);
    }
  else if (dbus_message_is_signal (message,
				   DBUS_INTERFACE_ORG_FREEDESKTOP_LOCAL,
				   "Disconnected"))
    {
      /* Note: This is a terminal situation. We can't live without the bus and
       * we don't support starting it after the client. We could add support for
       * that though.
       */

      dbus_connection_unref (global_conn);
      global_conn = NULL;
      service_running = FALSE;
            
      d(g_print ("****** Disconnected!\n"));
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;      
    }
  else if (dbus_message_is_signal (message,
				   DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
				   "ServiceOwnerChanged"))
    {
      char *owner;

      dbus_message_iter_init (message, &iter);
      
      service = dbus_message_iter_get_string (&iter);
      
      if (strcmp (service, GCONF_DBUS_SERVICE) != 0)
	{
	  dbus_free (service);
	  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

      dbus_free (service);

      if (!dbus_message_iter_next (&iter)) 
	{
	  d(g_print ("Malformed ServiceOwnerChanged signal\n"));
	  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
     
      
      owner = dbus_message_iter_get_string (&iter); 
      if (strcmp (owner, "") == 0) 
	{
	  /* GConfd is back. */
	  service_running = TRUE;

	  if (needs_reconnect)
	    {
	      needs_reconnect = FALSE;
	      reinitialize_databases ();
	    }
	  
	  d(g_print ("*** Gconf Service created\n"));
	}

      dbus_free (owner);

      if (!dbus_message_iter_next (&iter)) {
	d(g_print ("Malformed ServiceOwnerChanged signal\n"));
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
      }
      
      owner = dbus_message_iter_get_string (&iter);
      if (strcmp (owner, "") == 0) 
	{
	  /* GConfd is gone, set the state so we can detect that we're down. */
	  service_running = FALSE;
	  needs_reconnect = TRUE;

	  d(g_print ("*** GConf Service deleted\n"));
	}
      
      dbus_free (owner);

      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;      
    }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
} 

/* FIXME: What should this do in the D-BUS case? */
static void
gconf_detach_config_server(void)
{  
  if (engines_by_db != NULL)
    {
      g_hash_table_destroy (engines_by_db);
      engines_by_db = NULL;
    }
}

/**
 * gconf_debug_shutdown:
 * @void: 
 * 
 * Detach from the config server and release
 * all related resources
 **/
int
gconf_debug_shutdown (void)
{
  gconf_detach_config_server ();

  return 0;
}

static DBusHandlerResult
handle_notify (DBusConnection *connection,
	       DBusMessage *message,
	       GConfEngine *conf2)
{
  GConfEngine *conf;
  gchar *key, *schema_name;
  gboolean is_default, is_writable;
  DBusMessageIter iter;
  GConfValue *value;
  GConfEntry* entry;
  GList *list, *l;
  gboolean match = FALSE;
  gchar *namespace_section, *db;

  dbus_message_iter_init (message, &iter);

  db = dbus_message_iter_get_string (&iter);

  if (!dbus_message_iter_next (&iter))
    {
      dbus_free (db);
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
  
  namespace_section = dbus_message_iter_get_string (&iter);

  if (!dbus_message_iter_next (&iter))
    {
      dbus_free (db);
      dbus_free (namespace_section);
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

  conf = lookup_engine_by_database (db);

  g_return_val_if_fail (conf != NULL, DBUS_HANDLER_RESULT_NOT_YET_HANDLED);
  if (conf == NULL)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  
  dbus_free (db);
  
  if (!gconf_dbus_get_entry_values_from_message_iter (&iter,
						      &key,
						      &value,
						      &is_default,
						      &is_writable,
						      &schema_name))
    {
      dbus_free (namespace_section);
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
  
  if (value == NULL)
    {
      g_free (key);
      g_free (schema_name);
      dbus_free (namespace_section);
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
  
  d(g_print ("Got notify on %s (%s)\n", key, namespace_section));

  list = gconf_cnxn_lookup_dir (conf, namespace_section);
  for (l = list; l; l = l->next)
    {
      GConfCnxn *cnxn = l->data;

      d(g_print ("match? %s\n", cnxn->namespace_section));
      
      if (strcmp (cnxn->namespace_section, namespace_section) == 0)
	{
	  d(g_print ("yes: %s\n", key));
	  
	  entry = gconf_entry_new (g_strdup (key), value);
	  gconf_cnxn_notify (cnxn, entry);
	  gconf_entry_free (entry);
	  
	  match = TRUE;
	}
    }

  gconf_value_free (value);

  g_free (key);
  g_free (schema_name);
  dbus_free (namespace_section);
  
  if (!match)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  
  return DBUS_HANDLER_RESULT_HANDLED;
}


/*
 * Daemon control
 */

void          
gconf_shutdown_daemon (GError** err)
{
  DBusMessage *message;

  /* Don't want to spawn it if it's already down */
  if (global_conn == NULL || !service_running)
    return;
  
  message = dbus_message_new_method_call (GCONF_DBUS_SERVICE,
					  GCONF_DBUS_SERVER_OBJECT,
					  GCONF_DBUS_SERVER_INTERFACE,
					  GCONF_DBUS_SERVER_SHUTDOWN);

  dbus_connection_send (global_conn, message, 0);
  dbus_connection_flush (global_conn);

  dbus_message_unref (message);
}

gboolean
gconf_ping_daemon (void)
{
  return service_running;
}

gboolean
gconf_spawn_daemon (GError **err)
{
  return ensure_service (TRUE, err);
}



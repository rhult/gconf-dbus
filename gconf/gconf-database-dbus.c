/* -*- mode: C; c-file-style: "gnu" -*- */
/* GConf
 * Copyright (C) 2003, 2004 Imendio HB
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
#include <string.h>
#include "gconfd.h"
#include "gconf-dbus-utils.h"
#include "gconfd-dbus.h"
#include "gconf-database-dbus.h"

#define DATABASE_OBJECT_PATH "/org/gnome/GConf/Database"

static GHashTable *databases = NULL;
static GConfDatabaseDBus *default_db = NULL;
static gint object_nr = 0;

struct _GConfDatabaseDBus {
  GConfDatabase  *db;
  DBusConnection *conn;
  
  char     *address;
  char     *object_path;
  
  /* Information about clients that want notification. */
  GHashTable *notifications;
};

typedef struct {
  char  *namespace_section;
  GList *clients;
} NotificationData;


static void           database_unregistered_func (DBusConnection  *connection,
                                                  GConfDatabaseDBus *db);
static DBusHandlerResult
database_message_func                            (DBusConnection  *connection,
                                                  DBusMessage     *message,
                                                  GConfDatabaseDBus *db);
static DBusHandlerResult
database_filter_func                             (DBusConnection  *connection,
						  DBusMessage     *message,
						  GConfDatabaseDBus *db);
static DBusHandlerResult
database_handle_service_owner_changed            (DBusConnection  *connection,
						  DBusMessage     *message,
						  GConfDatabaseDBus *db);
static void           database_handle_lookup     (DBusConnection  *conn,
                                                  DBusMessage     *message,
                                                  GConfDatabaseDBus *db);
static void           database_handle_lookup_ext (DBusConnection  *conn,
                                                  DBusMessage     *message,
                                                  GConfDatabaseDBus *db);
static void           database_handle_set        (DBusConnection  *conn,
                                                  DBusMessage     *message,
                                                  GConfDatabaseDBus *db);
static void           database_handle_unset      (DBusConnection  *conn,
                                                  DBusMessage     *message,
                                                  GConfDatabaseDBus *db);
static void
database_handle_recursive_unset                  (DBusConnection  *conn,
                                                  DBusMessage     *message,
                                                  GConfDatabaseDBus *db);
static void           database_handle_dir_exists (DBusConnection  *conn,
                                                  DBusMessage     *message,
                                                  GConfDatabaseDBus *db);
static void
database_handle_get_all_entries                  (DBusConnection  *conn,
                                                  DBusMessage     *message,
                                                  GConfDatabaseDBus *db);
static void
database_handle_get_all_dirs                     (DBusConnection  *conn,
                                                  DBusMessage     *message,
                                                  GConfDatabaseDBus *db);
static void           database_handle_set_schema (DBusConnection  *conn,
                                                  DBusMessage     *message,
                                                  GConfDatabaseDBus *db);
static void           database_handle_add_notify (DBusConnection  *conn,
                                                  DBusMessage     *message,
                                                  GConfDatabaseDBus *db);
static gboolean
database_remove_notification_data                (GConfDatabaseDBus *db,
						  NotificationData *notification,
						  const char       *client);
static void
database_handle_remove_notify                    (DBusConnection  *conn,
                                                  DBusMessage     *message,
                                                  GConfDatabaseDBus *db);
static void           ensure_initialized         (void);
static void           database_removed           (GConfDatabaseDBus *db);

static DBusObjectPathVTable
database_vtable = {
        (DBusObjectPathUnregisterFunction) database_unregistered_func,
        (DBusObjectPathMessageFunction)    database_message_func,
        NULL,
};
 
static void
database_unregistered_func (DBusConnection *connection, GConfDatabaseDBus *db)
{
}

static DBusHandlerResult
database_message_func (DBusConnection  *connection,
                       DBusMessage     *message,
                       GConfDatabaseDBus *db)
{
  if (gconfd_dbus_check_in_shutdown (connection, message))
    {
      return DBUS_HANDLER_RESULT_HANDLED;
    }

  if (dbus_message_is_method_call (message,
				   GCONF_DBUS_DATABASE_INTERFACE,
				   GCONF_DBUS_DATABASE_LOOKUP)) {
    database_handle_lookup (connection, message, db);
  }
  else if (dbus_message_is_method_call (message,
					GCONF_DBUS_DATABASE_INTERFACE,
					GCONF_DBUS_DATABASE_LOOKUP_EXTENDED)) {
    database_handle_lookup_ext (connection, message, db);
  }
  else if (dbus_message_is_method_call (message,
					GCONF_DBUS_DATABASE_INTERFACE,
					GCONF_DBUS_DATABASE_SET)) {
    database_handle_set (connection, message, db);
  }
  else if (dbus_message_is_method_call (message,
					GCONF_DBUS_DATABASE_INTERFACE,
					GCONF_DBUS_DATABASE_UNSET)) {
    database_handle_unset (connection, message, db);
  }
  else if (dbus_message_is_method_call (message,
					GCONF_DBUS_DATABASE_INTERFACE,
					GCONF_DBUS_DATABASE_RECURSIVE_UNSET)) {
    database_handle_recursive_unset (connection, message, db);
  }
  else if (dbus_message_is_method_call (message,
					GCONF_DBUS_DATABASE_INTERFACE,
					GCONF_DBUS_DATABASE_DIR_EXISTS)) {
    database_handle_dir_exists (connection, message, db);
  }
  else if (dbus_message_is_method_call (message,
					GCONF_DBUS_DATABASE_INTERFACE,
					GCONF_DBUS_DATABASE_GET_ALL_ENTRIES)) {
    database_handle_get_all_entries (connection, message, db);
  }
  else if (dbus_message_is_method_call (message,
					GCONF_DBUS_DATABASE_INTERFACE,
					GCONF_DBUS_DATABASE_GET_ALL_DIRS)) {
    database_handle_get_all_dirs (connection, message, db);
  }
  else if (dbus_message_is_method_call (message,
					GCONF_DBUS_DATABASE_INTERFACE,
					GCONF_DBUS_DATABASE_SET_SCHEMA)) {
    database_handle_set_schema (connection, message, db);
  }
  else if (dbus_message_is_method_call (message,
					GCONF_DBUS_DATABASE_INTERFACE,
					GCONF_DBUS_DATABASE_ADD_NOTIFY)) {
	  database_handle_add_notify (connection, message, db);
  }
  else if (dbus_message_is_method_call (message,
					GCONF_DBUS_DATABASE_INTERFACE,
					GCONF_DBUS_DATABASE_REMOVE_NOTIFY)) {
	  database_handle_remove_notify (connection, message, db);
  } else {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  return DBUS_HANDLER_RESULT_HANDLED;
}

static void
get_all_notifications_func (gpointer key,
			    gpointer value,
			    gpointer user_data)
{
  GList **list = user_data;
  
  *list = g_list_prepend (*list, value);
}

static DBusHandlerResult
database_filter_func (DBusConnection  *connection,
		      DBusMessage     *message,
		      GConfDatabaseDBus *db)
{
  if (dbus_message_is_signal (message,
			      DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                              "ServiceOwnerChanged"))
    return database_handle_service_owner_changed (connection, message, db);

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult
database_handle_service_owner_changed (DBusConnection *connection,
				       DBusMessage *message,
				       GConfDatabaseDBus *db)
{  
  DBusMessageIter iter;
  char *service;
  GList *notifications = NULL, *l;
  NotificationData *notification;
  char *owner;
  
  /* FIXME: This might be a bit too slow to do like this. We could add a hash
   * table that maps client base service names to notification data, instead of
   * going through the entire list of notifications and clients.
   */
  dbus_message_iter_init (message, &iter);
  service = dbus_message_iter_get_string (&iter);

  if (!dbus_message_iter_next (&iter))
    {
      g_warning ("Misformated ServiceOwnerChanged message");
      dbus_free (service);
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
  
  if (!dbus_message_iter_next (&iter))
    {
      g_warning ("Misformated ServiceOwnerChanged message");
      dbus_free (service);
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

  owner = dbus_message_iter_get_string (&iter);
  if (strcmp (owner, "") != 0) 
    {
      /* Service still exist, don't remove notifications */
      dbus_free (service);
      dbus_free (owner);
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
  dbus_free (owner);
  
  g_hash_table_foreach (db->notifications, get_all_notifications_func,
			&notifications);
  
  for (l = notifications; l; l = l->next)
    {
      notification = l->data;
      
      database_remove_notification_data (db, notification, service);
    }
  
  g_list_free (notifications);
  dbus_free (service);

  return DBUS_HANDLER_RESULT_HANDLED;
}
    
static void
database_handle_lookup (DBusConnection  *conn,
                        DBusMessage     *message,
                        GConfDatabaseDBus *db)
{
  GConfValue *value;
  DBusMessage *reply;
  gchar *key;
  gchar *locale;
  GConfLocaleList *locales;
  gboolean use_schema_default;
  GError *gerror = NULL;
  
  if (!gconfd_dbus_get_message_args (conn, message,
				     DBUS_TYPE_STRING, &key,
				     DBUS_TYPE_STRING, &locale,
				     DBUS_TYPE_BOOLEAN, &use_schema_default,
				     0))
    return;
  
  locales = gconfd_locale_cache_lookup (locale);
  dbus_free (locale);
  
  value = gconf_database_query_value (db->db, key, locales->list, 
				      use_schema_default,
				      NULL, NULL, NULL, &gerror);

  dbus_free (key);

  if (gconfd_dbus_set_exception (conn, message, &gerror))
    goto fail;

  reply = dbus_message_new_method_return (message);
  gconf_dbus_message_append_gconf_value (reply, value);
  dbus_connection_send (conn, reply, NULL);
  dbus_message_unref (reply);
  
 fail:
  if (value)
    gconf_value_free (value);
}

static void 
database_handle_lookup_ext (DBusConnection  *conn,
			    DBusMessage     *message,
			    GConfDatabaseDBus *db)
{
  GConfValue *value;
  gchar *schema_name;
  gboolean value_is_default;
  gboolean value_is_writable;
  DBusMessage *reply;
  gchar *key;
  gchar *locale;
  GConfLocaleList *locales;
  gboolean use_schema_default;
  GError *gerror = NULL;
  
  if (!gconfd_dbus_get_message_args (conn, message,
				     DBUS_TYPE_STRING, &key,
				     DBUS_TYPE_STRING, &locale,
				     DBUS_TYPE_BOOLEAN, &use_schema_default,
				     0))
    return;
  
  locales = gconfd_locale_cache_lookup (locale);
  dbus_free (locale);
  
  value = gconf_database_query_value (db->db, key, locales->list,
				      use_schema_default,
				      &schema_name, &value_is_default, 
				      &value_is_writable, &gerror);
  
  if (gconfd_dbus_set_exception (conn, message, &gerror))
    goto fail;
  
  reply = dbus_message_new_method_return (message);
  gconf_dbus_message_append_entry (reply,
				   key,
				   value,
				   value_is_default,
				   value_is_writable,
				   schema_name);
  
  dbus_connection_send (conn, reply, NULL);
  dbus_message_unref (reply);

 fail:
  dbus_free (key);
  
  if (value)
    gconf_value_free (value);
  
  g_free (schema_name);
}

static void
database_handle_set (DBusConnection *conn,
                     DBusMessage    *message,
                     GConfDatabaseDBus *db)
{
  gchar *key;
  GConfValue *value = NULL; 
  GError *gerror = NULL;
  DBusMessage *reply;
  DBusMessageIter iter;

  dbus_message_iter_init (message, &iter);

  key = dbus_message_iter_get_string (&iter);
  dbus_message_iter_next (&iter);
  value = gconf_dbus_create_gconf_value_from_message_iter (&iter);

  gconf_database_set (db->db, key, value, &gerror);
  dbus_free (key);
  gconf_value_free (value);

  if (gconfd_dbus_set_exception (conn, message, &gerror))
    return;

  reply = dbus_message_new_method_return (message);
  dbus_connection_send (conn, reply, NULL);
  dbus_message_unref (reply);
}

static void
database_handle_unset (DBusConnection *conn,
		       DBusMessage    *message,
		       GConfDatabaseDBus *db)
{
  gchar *key;
  gchar *locale;
  GError *gerror = NULL;
  DBusMessage *reply;

  if (!gconfd_dbus_get_message_args (conn, message,
				     DBUS_TYPE_STRING, &key,
				     DBUS_TYPE_STRING, &locale,
				     0))
    return;

  if (locale[0] == '\0')
    {
      dbus_free (locale);
      locale = NULL;
    }
  
  gconf_database_unset (db->db, key, locale, &gerror);
  dbus_free (key);
  dbus_free (locale);
  
  gconf_database_sync (db->db, NULL);
  
  if (gconfd_dbus_set_exception (conn, message, &gerror))
    return;
 
  reply = dbus_message_new_method_return (message);
  dbus_connection_send (conn, reply, NULL);
  dbus_message_unref (reply);

}
                                                                               
static void
database_handle_recursive_unset  (DBusConnection *conn,
                                  DBusMessage    *message,
                                  GConfDatabaseDBus *db)
{
  gchar *key;
  gchar *locale;
  GError      *gerror = NULL;
  guint32      unset_flags;
  DBusMessage *reply;
  
  if (!gconfd_dbus_get_message_args (conn, message, 
				     DBUS_TYPE_STRING, &key,
				     DBUS_TYPE_STRING, &locale,
				     DBUS_TYPE_UINT32, &unset_flags,
				     0))
    return;

  if (locale[0] == '\0')
    {
      dbus_free (locale);
      locale = NULL;
    }
  
  gconf_database_recursive_unset (db->db, key, locale, unset_flags, &gerror);
  dbus_free (key);
  dbus_free (locale);
  
  gconf_database_sync (db->db, NULL);
  
  if (gconfd_dbus_set_exception (conn, message, &gerror))
    return;

  reply = dbus_message_new_method_return (message);
  dbus_connection_send (conn, reply, NULL);
  dbus_message_unref (reply);
}
                                                                                
static void
database_handle_dir_exists (DBusConnection *conn,
                            DBusMessage    *message,
                            GConfDatabaseDBus *db)
{
  gboolean     exists;
  gchar *dir;
  GError      *gerror = NULL;
  DBusMessage *reply;
 
  if (!gconfd_dbus_get_message_args (conn, message, 
				     DBUS_TYPE_STRING, &dir,
				     0))
    return;

  exists = gconf_database_dir_exists (db->db, dir, &gerror);
  dbus_free (dir);

  if (gconfd_dbus_set_exception (conn, message, &gerror))
    return;

  reply = dbus_message_new_method_return (message);
  dbus_message_append_args (reply,
			    DBUS_TYPE_BOOLEAN, exists,
			    0);
  dbus_connection_send (conn, reply, NULL);
  dbus_message_unref (reply);
}
                                                                                
static void
database_handle_get_all_entries (DBusConnection *conn,
                                 DBusMessage    *message,
                                 GConfDatabaseDBus *db)
{
  GSList *entries, *l;
  gchar  *dir;
  gchar  *locale;
  GError *gerror = NULL;
  GConfLocaleList* locales;
  DBusMessage *reply;

  if (!gconfd_dbus_get_message_args (conn, message, 
				     DBUS_TYPE_STRING, &dir,
				     DBUS_TYPE_STRING, &locale,
				     0)) 
    return;

  locales = gconfd_locale_cache_lookup (locale);
  dbus_free (locale);

  entries = gconf_database_all_entries (db->db, dir, 
					locales->list, &gerror);
  dbus_free (dir);

  if (gconfd_dbus_set_exception (conn, message, &gerror))
    return;

  reply = dbus_message_new_method_return (message);
  
  for (l = entries; l; l = l->next)
    {
      GConfEntry *entry = l->data;

      gconf_dbus_message_append_entry (reply,
				       entry->key,
				       gconf_entry_get_value (entry),
				       gconf_entry_get_is_default (entry),
				       gconf_entry_get_is_writable (entry),
				       gconf_entry_get_schema_name (entry));
      gconf_entry_free (entry);
    }
  
  dbus_connection_send (conn, reply, NULL);
  dbus_message_unref (reply);

  g_slist_free (entries);
}
                                                                                
static void
database_handle_get_all_dirs (DBusConnection *conn,
                              DBusMessage    *message,
                              GConfDatabaseDBus *db)
{
  GSList *dirs, *l;
  gchar *dir;
  GError      *gerror = NULL;
  DBusMessage *reply;
  DBusMessageIter iter;

  if (!gconfd_dbus_get_message_args (conn, message,
				     DBUS_TYPE_STRING, &dir,
				     0)) 
    return;

  dirs = gconf_database_all_dirs (db->db, dir, &gerror);

  dbus_free (dir);

  if (gconfd_dbus_set_exception (conn, message, &gerror))
    return;
  
  reply = dbus_message_new_method_return (message);
  
  dbus_message_append_iter_init (reply, &iter);
  for (l = dirs; l; l = l->next) 
    {
      gchar *str = (gchar *) l->data;
      
      dbus_message_iter_append_string (&iter, str);
      
      g_free (l->data);
    }
 
  g_slist_free (dirs);
  
  dbus_connection_send (conn, reply, NULL);
  dbus_message_unref (reply);
}
                                                                                
static void
database_handle_set_schema (DBusConnection *conn,
                            DBusMessage    *message,
                            GConfDatabaseDBus *db)
{
  gchar *key;
  gchar *schema_key;
  GError      *gerror = NULL;
  DBusMessage *reply;

  if (!gconfd_dbus_get_message_args (conn, message,
				     DBUS_TYPE_STRING, &key,
				     DBUS_TYPE_STRING, &schema_key,
				     0)) 
    return;

  gconf_database_set_schema (db->db, key, schema_key, &gerror);
  dbus_free (key);
  dbus_free (schema_key);

  if (gconfd_dbus_set_exception (conn, message, &gerror))
    return;

  reply = dbus_message_new_method_return (message);
  dbus_connection_send (conn, reply, NULL);
  dbus_message_unref (reply);
}

static void
database_handle_add_notify (DBusConnection    *conn,
                            DBusMessage       *message,
                            GConfDatabaseDBus *db)
{
  gchar *namespace_section;
  DBusMessage *reply;
  const char *sender;
  NotificationData *notification;

  if (!gconfd_dbus_get_message_args (conn, message,
				     DBUS_TYPE_STRING, &namespace_section,
				     0)) 
    return;

  sender = dbus_message_get_sender (message);
  
  notification = g_hash_table_lookup (db->notifications, namespace_section);

  if (notification == NULL)
    {
      notification = g_new0 (NotificationData, 1);
      notification->namespace_section = g_strdup (namespace_section);

      g_hash_table_insert (db->notifications,
			   notification->namespace_section, notification);
    }
  
  notification->clients = g_list_prepend (notification->clients,
					  g_strdup (sender));
  
  dbus_free (namespace_section);

  reply = dbus_message_new_method_return (message);
  dbus_connection_send (conn, reply, NULL);
  dbus_message_unref (reply);
}

static gboolean
database_remove_notification_data (GConfDatabaseDBus *db,
				   NotificationData *notification,
				   const char *client)
{
  GList *element;
  
  element = g_list_find_custom (notification->clients, client,
			     (GCompareFunc)strcmp);
  if (element == NULL)
    return FALSE;
  
  notification->clients = g_list_remove_link (notification->clients, element);
  if (notification->clients == NULL)
    {
      g_hash_table_remove (db->notifications,
			   notification->namespace_section);

      g_free (notification->namespace_section);
      g_free (notification);
    }
  
  g_free (element->data);
  g_list_free_1 (element);

  return TRUE;
}

static void
database_handle_remove_notify (DBusConnection    *conn,
			       DBusMessage       *message,
			       GConfDatabaseDBus *db)
{
  gchar *namespace_section;
  DBusMessage *reply;
  const char *sender;
  NotificationData *notification;
  
  if (!gconfd_dbus_get_message_args (conn, message,
				     DBUS_TYPE_STRING, &namespace_section,
				     0)) 
    return;

  sender = dbus_message_get_sender (message);
  
  notification = g_hash_table_lookup (db->notifications, namespace_section);
  dbus_free (namespace_section);

  /* Notification can be NULL if the client and server get out of sync. */
  if (notification == NULL || !database_remove_notification_data (db, notification, sender))
    {
      gconf_log (GCL_DEBUG, _("Notification on %s doesn't exist"),
                 namespace_section);
    }
  
  reply = dbus_message_new_method_return (message);
  dbus_connection_send (conn, reply, NULL);
  dbus_message_unref (reply);
}

static void
database_removed (GConfDatabaseDBus *dbus_db)
{
  /* FIXME: Free stuff */
}

static void
ensure_initialized (void)
{
  if (!databases)
    databases = g_hash_table_new_full (g_int_hash,
				       g_int_equal,
				       NULL,
				       (GDestroyNotify) database_removed);
}

GConfDatabaseDBus *
gconf_database_dbus_get (DBusConnection *conn, const gchar *address,
			 GError **gerror)
{
  GConfDatabaseDBus  *dbus_db = NULL;
  GConfDatabase      *db;

  ensure_initialized ();

  db = gconfd_lookup_database (address);
  if (!db)
    {
      db = gconfd_obtain_database (address, gerror);
      if (!db)
	{
	  gconf_log (GCL_WARNING, _("Database not found: %s"), address);
	  return NULL;
	}
    }

  if (!address)
    dbus_db = default_db;
  else
    dbus_db = g_hash_table_lookup (databases, &db); 
  
  if (dbus_db) 
    return dbus_db;

  dbus_db = g_new0 (GConfDatabaseDBus, 1);
  dbus_db->db = db;
  dbus_db->conn = conn;

  if (!address)
    {
      dbus_db->address = NULL;
      default_db = dbus_db;
    }
  else 
    {
      dbus_db->address = g_strdup (address);
      g_hash_table_insert (databases, &db, dbus_db);
    }

  dbus_db->object_path = g_strdup_printf ("%s/%d", 
					  DATABASE_OBJECT_PATH, 
					  object_nr++);

  dbus_connection_register_object_path (conn, dbus_db->object_path,
					&database_vtable, dbus_db);

  dbus_db->notifications = g_hash_table_new (g_str_hash, g_str_equal);
 
  dbus_connection_add_filter (conn,
			      (DBusHandleMessageFunction)database_filter_func,
			      dbus_db, NULL);
  
  return dbus_db;
}

gboolean
database_foreach_unregister (gpointer key,
			     GConfDatabaseDBus *db,
			     gpointer user_data)
{
  dbus_connection_unregister_object_path (db->conn, db->object_path);

  return TRUE;
}

void 
gconf_database_dbus_unregister_all (void)
{
  ensure_initialized ();
  g_hash_table_foreach_remove (databases, 
			       (GHRFunc) database_foreach_unregister, NULL);
}

const char *
gconf_database_dbus_get_path (GConfDatabaseDBus *db)
{
  return db->object_path;
}

void
gconf_database_dbus_notify_listeners (GConfDatabase    *db,
				      const gchar      *key,
				      const GConfValue *value,
				      gboolean          is_default,
				      gboolean          is_writable)
{
  GConfDatabaseDBus *dbus_db = NULL;
  char *dir, *sep;
  GList *l;
  NotificationData *notification;
  DBusMessage *message;
  gboolean last;
  
  if (db == default_db->db)
    dbus_db = default_db;
  else
    dbus_db = g_hash_table_lookup (databases, &db);

  if (!dbus_db)
    return;

  dir = g_strdup (key);

  /* Lookup the key in the namespace hierarchy, start with the full key and then
   * remove the leaf, lookup again, remove the leaf, and so on until a match is
   * found. Notify the clients (identified by their base service) that
   * correspond to the found namespace.
   */
  last = FALSE;
  while (1)
    {
      notification = g_hash_table_lookup (dbus_db->notifications, dir);

      if (notification)
	{
	  for (l = notification->clients; l; l = l->next)
	    {
	      const char *base_service = l->data;
	      
	      message = dbus_message_new_method_call (base_service,
						      GCONF_DBUS_CLIENT_OBJECT,
						      GCONF_DBUS_CLIENT_INTERFACE,
						      "Notify");

	      dbus_message_append_args (message,
					DBUS_TYPE_STRING, dbus_db->object_path,
					DBUS_TYPE_STRING, dir,
					DBUS_TYPE_INVALID);
	      
	      gconf_dbus_message_append_entry (message,
					       key,
					       value,
					       is_default,
					       is_writable,
					       NULL);
	      
	      dbus_message_set_no_reply (message, TRUE);
	      
	      dbus_connection_send (dbus_db->conn, message, NULL);
	      dbus_message_unref (message);
	    }
	}

      if (last)
	break;
      
      sep = strrchr (dir, '/');

      /* Special case to catch notifications on the root. */
      if (sep == dir)
	{
	  last = TRUE;
	  sep[1] = '\0';
	}
      else
	*sep = '\0';
    }

  g_free (dir);
}

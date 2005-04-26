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

#include "gconf-database.h"
#include "gconf-listeners.h"
#include "gconf-sources.h"
#include "gconf-locale.h"

#include "gconfd.h"
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#ifdef HAVE_ORBIT
#include "gconf-database-corba.h"
#endif
#ifdef HAVE_DBUS
#include "gconfd-dbus.h"
#include "gconf-database-dbus.h"
#endif

#define SYNC_TIMEOUT 5000

/*
 * Forward decls
 */

static void gconf_database_really_sync(GConfDatabase* db);

GConfDatabase*
gconf_database_new (GConfSources  *sources)
{
  GConfDatabase* db;
  
  db = g_new0 (GConfDatabase, 1);

  db->listeners = gconf_listeners_new();

  db->sources = sources;

  db->last_access = time(NULL);

  db->sync_idle = 0;
  db->sync_timeout = 0;

  db->persistent_name = NULL;

#ifdef HAVE_ORBIT
  gconf_database_corba_init (db);
#endif
  
  return db;
}

void
gconf_database_free (GConfDatabase *db)
{
#ifdef HAVE_ORBIT
  gconf_database_corba_deinit (db);
#endif
  
  if (db->listeners != NULL)
    {
      gboolean need_sync = FALSE;
      
      g_assert(db->sources != NULL);

      if (db->sync_idle != 0)
        {
          g_source_remove(db->sync_idle);
          db->sync_idle = 0;
          need_sync = TRUE;
        }

      if (db->sync_timeout != 0)
        {
          g_source_remove(db->sync_timeout);
          db->sync_timeout = 0;
          need_sync = TRUE;
        }

      if (need_sync)
        gconf_database_really_sync(db);
      
      gconf_listeners_free(db->listeners);
      gconf_sources_free(db->sources);
    }

  g_free (db->persistent_name);
  
  g_free (db);
}
  

static gint
gconf_database_sync_idle (GConfDatabase* db)
{
  db->sync_idle = 0;

  /* could have been added before reaching the
   * idle
   */
  if (db->sync_timeout != 0)
    {
      g_source_remove (db->sync_timeout);
      db->sync_timeout = 0;
    }
  
  gconf_database_really_sync (db);
  
  /* Remove the idle function by returning FALSE */
  return FALSE; 
}

static gint
gconf_database_sync_timeout(GConfDatabase* db)
{
  db->sync_timeout = 0;
  
  /* Install the sync idle */
  if (db->sync_idle == 0)
    db->sync_idle = g_idle_add((GSourceFunc)gconf_database_sync_idle, db);

  gconf_log(GCL_DEBUG, "Sync queued one minute after changes occurred");
  
  /* Remove the timeout function by returning FALSE */
  return FALSE;
}

static void
gconf_database_really_sync(GConfDatabase* db)
{
  GError* error = NULL;
  
  if (!gconf_database_synchronous_sync(db, &error))
    {
      g_return_if_fail(error != NULL);

      gconf_log(GCL_ERR, _("Failed to sync one or more sources: %s"), 
                error->message);
      g_error_free(error);
    }
  else
    {
      gconf_log(GCL_DEBUG, "Sync completed without errors");
    }
}

static void
gconf_database_sync_nowish(GConfDatabase* db)
{
  /* Go ahead and sync as soon as the event loop quiets down */

  /* remove the scheduled sync */
  if (db->sync_timeout != 0)
    {
      g_source_remove(db->sync_timeout);
      db->sync_timeout = 0;
    }

  /* Schedule immediate post-quietdown sync */
  if (db->sync_idle == 0)
    db->sync_idle = g_idle_add((GSourceFunc)gconf_database_sync_idle, db);
}

static void
gconf_database_schedule_sync(GConfDatabase* db)
{
  /* Plan to sync within a minute or so */
  if (db->sync_idle != 0)
    return;
  else if (db->sync_timeout != 0)
    {
      g_source_remove (db->sync_timeout);
      db->sync_timeout = 0;
    }

  db->sync_timeout = g_timeout_add(SYNC_TIMEOUT, (GSourceFunc)gconf_database_sync_timeout, db);
}

GConfValue*
gconf_database_query_value (GConfDatabase  *db,
                            const gchar    *key,
                            const gchar   **locales,
                            gboolean        use_schema_default,
                            char          **schema_name,
                            gboolean       *value_is_default,
                            gboolean       *value_is_writable,
                            GError    **err)
{
  GConfValue* val;
  
  g_return_val_if_fail(err == NULL || *err == NULL, NULL);
  g_assert(db->listeners != NULL);
  
  db->last_access = time(NULL);
  
  val = gconf_sources_query_value(db->sources, key, locales,
                                  use_schema_default,
                                  value_is_default,
                                  value_is_writable,
                                  schema_name,
                                  err);
  
  if (err && *err != NULL)
    {
      gconf_log(GCL_ERR, _("Error getting value for `%s': %s"),
                key, (*err)->message);
    }
  
  return val;
}

GConfValue*
gconf_database_query_default_value (GConfDatabase  *db,
                                    const gchar    *key,
                                    const gchar   **locales,
                                    gboolean       *is_writable,
                                    GError    **err)
{  
  g_return_val_if_fail(err == NULL || *err == NULL, NULL);
  g_assert(db->listeners != NULL);
  
  db->last_access = time(NULL);

  return gconf_sources_query_default_value(db->sources, key, locales,
                                           is_writable,
                                           err);
}

void
gconf_database_set   (GConfDatabase      *db,
                      const gchar        *key,
                      GConfValue         *value,
                      GError        **err)
{
  GError *error = NULL;
  
  g_assert(db->listeners != NULL);
  g_return_if_fail(err == NULL || *err == NULL);
  
  db->last_access = time(NULL);

#if 0
  /* this really churns the logfile, so we avoid it */
  gconf_log(GCL_DEBUG, "Received request to set key `%s'", key);
#endif
  
  gconf_sources_set_value(db->sources, key, value, &error);

  if (error)
    {
      gconf_log(GCL_ERR, _("Error setting value for `%s': %s"),
                key, error->message);
      
      g_propagate_error (err, error);

      return;
    }
  else
    {
      gconf_database_schedule_sync(db);

#ifdef HAVE_ORBIT
      gconf_database_corba_notify_listeners(db, key, value,
					    /* Can't possibly be the default,
					       since we just set it,
					       and must be writable since
					       setting it succeeded.
					    */
					    FALSE,
					    TRUE);
#endif
#ifdef HAVE_DBUS
      gconf_database_dbus_notify_listeners(db, key, value,
					   /* Can't possibly be the default,
					      since we just set it,
					      and must be writable since
					      setting it succeeded.
					    */
					   FALSE,
					   TRUE);
#endif
      
    }
}

void
gconf_database_unset (GConfDatabase      *db,
                      const gchar        *key,
                      const gchar        *locale,
                      GError        **err)
{
  GError* error = NULL;
  
  g_return_if_fail(err == NULL || *err == NULL);
  
  g_assert(db->listeners != NULL);
  
  db->last_access = time(NULL);
  
  gconf_log(GCL_DEBUG, "Received request to unset key `%s'", key);

  gconf_sources_unset_value(db->sources, key, locale, &error);

  if (error != NULL)
    {
      gconf_log(GCL_ERR, _("Error unsetting `%s': %s"),
                key, error->message);

      if (err)
        *err = error;
      else
        g_error_free(error);

      error = NULL;
    }
  else
    {
      GConfValue* def_value;
      const gchar* locale_list[] = { NULL, NULL };
      gboolean is_writable = TRUE;

      /* This is a somewhat dubious optimization
       * that assumes that if the unset was successful
       * the default value is the new value. Which is
       * safe for now, I _think_
       */
      locale_list[0] = locale;
      def_value = gconf_database_query_default_value(db,
                                                     key,
                                                     locale_list,
                                                     &is_writable,
                                                     err);

      if (err && *err)
        gconf_log(GCL_ERR, _("Error getting default value for `%s': %s"),
                  key, (*err)->message);

          
      gconf_database_schedule_sync(db);

#ifdef HAVE_ORBIT
      gconf_database_corba_notify_listeners(db, key, def_value, TRUE, is_writable);
#endif
#ifdef HAVE_DBUS
      gconf_database_dbus_notify_listeners(db, key, def_value, TRUE, is_writable);
#endif
      
      if (def_value != NULL)
	gconf_value_free(def_value);
    }
}

void
gconf_database_recursive_unset (GConfDatabase      *db,
                                const gchar        *key,
                                const gchar        *locale,
                                GConfUnsetFlags     flags,
                                GError            **err)
{
  GError* error = NULL;
  GSList *notifies;
  GSList *tmp;
  
  g_return_if_fail (err == NULL || *err == NULL);
  
  g_assert (db->listeners != NULL);
  
  db->last_access = time (NULL);
  
  gconf_log (GCL_DEBUG, "Received request to recursively unset key \"%s\"", key);

  notifies = NULL;
  gconf_sources_recursive_unset (db->sources, key, locale,
                                 flags, &notifies, &error);

  /* We return the error but go ahead and finish the unset.
   * We're just returning the first error seen during the
   * unset process.
   */
  if (error != NULL)
    {
      gconf_log (GCL_ERR, _("Error unsetting \"%s\": %s"),
                 key, error->message);

      if (err)
        *err = error;
      else
        g_error_free (error);

      error = NULL;
    }
  
  tmp = notifies;
  while (tmp != NULL)
    {
      GConfValue* new_value;
      const gchar* locale_list[] = { NULL, NULL };
      gboolean is_writable = TRUE;
      gboolean is_default = TRUE;
      char *notify_key = tmp->data;
      
      locale_list[0] = locale;
      new_value = gconf_database_query_value (db,
                                              notify_key,
                                              locale_list,
                                              TRUE,
                                              NULL,
                                              &is_default,
                                              &is_writable,
                                              &error);

      if (error)
        {
          gconf_log (GCL_ERR, _("Error getting new value for \"%s\": %s"),
		     notify_key, error->message);
	  g_propagate_error (err, error);
	  error = NULL;
	}
      
      gconf_database_schedule_sync (db);

#ifdef HAVE_ORBIT
      gconf_database_corba_notify_listeners (db, notify_key, new_value,
					     is_default, is_writable);
#endif
#ifdef HAVE_DBUS
      gconf_database_dbus_notify_listeners (db, notify_key, new_value,
					     is_default, is_writable);
#endif

      if (new_value)
	gconf_value_free (new_value);
      g_free (notify_key);
      
      tmp = tmp->next;
    }

  g_slist_free (notifies);
}

gboolean
gconf_database_dir_exists  (GConfDatabase  *db,
                            const gchar    *dir,
                            GError    **err)
{
  gboolean ret;
  
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);
  
  g_assert(db->listeners != NULL);
  
  db->last_access = time(NULL);
  
  gconf_log(GCL_DEBUG, "Received dir_exists request for `%s'", dir);
  
  ret = gconf_sources_dir_exists(db->sources, dir, err);
  
  if (err && *err != NULL)
    {
      gconf_log(GCL_ERR, _("Error checking existence of `%s': %s"),
                 dir, (*err)->message);
      ret = FALSE;
    }

  return ret;
}

void
gconf_database_remove_dir  (GConfDatabase  *db,
                            const gchar    *dir,
                            GError    **err)
{  
  g_return_if_fail(err == NULL || *err == NULL);
  g_assert(db->listeners != NULL);
  
  db->last_access = time(NULL);
  
  gconf_log (GCL_DEBUG, "Received request to remove directory \"%s\"", dir);
  
  gconf_sources_remove_dir(db->sources, dir, err);

  if (err && *err != NULL)
    {
      gconf_log (GCL_ERR, _("Error removing directory \"%s\": %s"),
                 dir, (*err)->message);
    }
  else
    {
      gconf_database_schedule_sync(db);
    }
}

GSList*
gconf_database_all_entries (GConfDatabase  *db,
                            const gchar    *dir,
                            const gchar   **locales,
                            GError    **err)
{
  GSList* entries;
  
  g_return_val_if_fail(err == NULL || *err == NULL, NULL);
  
  g_assert(db->listeners != NULL);
  
  db->last_access = time(NULL);
  
  entries = gconf_sources_all_entries(db->sources, dir, locales, err);

  if (err && *err != NULL)
    {
      gconf_log(GCL_ERR, _("Failed to get all entries in `%s': %s"),
                 dir, (*err)->message);
    }

  return entries;
}

GSList*
gconf_database_all_dirs (GConfDatabase  *db,
                         const gchar    *dir,
                         GError    **err)
{
  GSList* subdirs;
  
  g_return_val_if_fail(err == NULL || *err == NULL, NULL);
  
  g_assert(db->listeners != NULL);
  
  db->last_access = time(NULL);
    
  gconf_log (GCL_DEBUG, "Received request to list all subdirs in `%s'", dir);

  subdirs = gconf_sources_all_dirs (db->sources, dir, err);

  if (err && *err != NULL)
    {
      gconf_log (GCL_ERR, _("Error listing dirs in `%s': %s"),
                 dir, (*err)->message);
    }
  return subdirs;
}

void
gconf_database_set_schema (GConfDatabase  *db,
                           const gchar    *key,
                           const gchar    *schema_key,
                           GError        **err)
{
  g_return_if_fail (err == NULL || *err == NULL);
  g_assert (db->listeners != NULL);
  
  db->last_access = time (NULL);
  
  gconf_sources_set_schema (db->sources, key, schema_key, err);

  if (err && *err != NULL)
    {
      gconf_log (GCL_ERR, _("Error setting schema for `%s': %s"),
                key, (*err)->message);
    }
  else
    {
      gconf_database_schedule_sync (db);
    }
}

void
gconf_database_sync (GConfDatabase  *db,
                     GError    **err)
{
  g_assert(db->listeners != NULL);
  
  db->last_access = time(NULL);
  
  gconf_log(GCL_DEBUG, "Received suggestion to sync all config data");

  gconf_database_sync_nowish(db);
}

gboolean
gconf_database_synchronous_sync (GConfDatabase  *db,
                                 GError    **err)
{  
  /* remove the scheduled syncs */
  if (db->sync_timeout != 0)
    {
      g_source_remove(db->sync_timeout);
      db->sync_timeout = 0;
    }

  if (db->sync_idle != 0)
    {
      g_source_remove(db->sync_idle);
      db->sync_idle = 0;
    }

  db->last_access = time(NULL);
  
  return gconf_sources_sync_all(db->sources, err);
}

void
gconf_database_clear_cache (GConfDatabase  *db,
                            GError    **err)
{
  g_assert(db->listeners != NULL);

  db->last_access = time(NULL);

  gconf_sources_clear_cache(db->sources);
}

const gchar*
gconf_database_get_persistent_name (GConfDatabase *db)
{
  if (db->persistent_name == NULL)
    {
      if (db->sources->sources)
        db->persistent_name =
          g_strdup (((GConfSource*)db->sources->sources->data)->address);
      else
        db->persistent_name = g_strdup ("empty");
    }

  return db->persistent_name;
}


/*
 * Locale hash
 */

static GConfLocaleCache* locale_cache = NULL;

GConfLocaleList *
gconfd_locale_cache_lookup(const gchar* locale)
{
  GConfLocaleList* locale_list;
  
  if (locale_cache == NULL)
    locale_cache = gconf_locale_cache_new();

  locale_list = gconf_locale_cache_get_list(locale_cache, locale);

  g_assert(locale_list != NULL);
  g_assert(locale_list->list != NULL);
  
  return locale_list;
}


void
gconfd_locale_cache_expire(void)
{
  if (locale_cache != NULL)
    gconf_locale_cache_expire(locale_cache, 60 * 30); /* 60 sec * 30 min */
}

void
gconfd_locale_cache_drop(void)
{
  if (locale_cache != NULL)
    {
      gconf_locale_cache_free(locale_cache);
      locale_cache = NULL;
    }
}

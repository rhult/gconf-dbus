/* GConf
 * Copyright (C) 1999, 2000 Red Hat Inc.
 * Developed by Havoc Pennington, some code in here borrowed from 
 * gnome-name-server and libgnorba (Elliot Lee)
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


/*
 * This is the per-user configuration daemon.
 * (has debug crap in it now)
 */

#include <config.h>

#include "gconf-internals.h"
#include "gconf-sources.h"
#include "gconf-listeners.h"
#include "gconf-locale.h"
#include "gconf-schema.h"
#include "gconf.h"
#include "gconfd.h"
#include "gconf-database.h"

#ifdef HAVE_ORBIT
#include "gconfd-corba.h"
#include "gconf-database-corba.h"
#include "gconf-corba-utils.h"
#endif

#ifdef HAVE_DBUS
#include "gconfd-dbus.h"
#include "gconf-database-dbus.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <syslog.h>
#include <time.h>
#include <sys/wait.h>
#include <locale.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#define DAEMON_DEBUG

/* This makes hash table safer when debugging */
#ifndef GCONF_ENABLE_DEBUG
#define safe_g_hash_table_insert g_hash_table_insert
#else
static void
safe_g_hash_table_insert(GHashTable* ht, gpointer key, gpointer value)
{
  gpointer oldkey = NULL, oldval = NULL;

  if (g_hash_table_lookup_extended(ht, key, &oldkey, &oldval))
    {
      gconf_log(GCL_WARNING, "Hash key `%s' is already in the table!",
                (gchar*) key);
      return;
    }
  else
    {
      g_hash_table_insert(ht, key, value);
    }
}
#endif

static GConfLock *daemon_lock = NULL;

/*
 * Declarations
 */

static void     gconf_main            (void);
static gboolean gconf_main_is_running (void);



static void    enter_shutdown          (void);

static void                 init_databases (void);
static void                 shutdown_databases (void);
static void                 set_default_database (GConfDatabase* db);
static void                 register_database (GConfDatabase* db);
static void                 unregister_database (GConfDatabase* db);
static void                 drop_old_databases (void);
static gboolean             no_databases_in_use (void);

static void gconf_handle_segv (int signum);

/*
 * Flag indicating that we are shutting down, so return errors
 * on any attempted operation. We do this instead of unregistering with
 * OAF or deactivating the server object, because we want to avoid
 * another gconfd starting up before we finish shutting down.
 */

static gboolean in_shutdown = FALSE;



/*
 * Main code
 */

/* This needs to be called before we register with OAF
 */
static void
gconf_server_load_sources(void)
{
  GSList* addresses;
  GList* tmp;
  gboolean have_writable = FALSE;
  gchar* conffile;
  GConfSources* sources = NULL;
  GError* error = NULL;
  
  conffile = g_strconcat(GCONF_CONFDIR, "/path", NULL);

  addresses = gconf_load_source_path(conffile, NULL);

  g_free(conffile);

#ifdef GCONF_ENABLE_DEBUG
  /* -- Debug only */
  
  if (addresses == NULL)
    {
      gconf_log(GCL_DEBUG, _("gconfd compiled with debugging; trying to load gconf.path from the source directory"));
      conffile = g_strconcat(GCONF_SRCDIR, "/gconf/gconf.path", NULL);
      addresses = gconf_load_source_path(conffile, NULL);
      g_free(conffile);
    }

  /* -- End of Debug Only */
#endif

  if (addresses == NULL)
    {      
      /* Try using the default address xml:readwrite:$(HOME)/.gconf */
      addresses = g_slist_append(addresses, g_strconcat("xml:readwrite:", g_get_home_dir(), "/.gconf", NULL));

      gconf_log(GCL_DEBUG, _("No configuration files found, trying to use the default config source `%s'"), (char *)addresses->data);
    }
  
  if (addresses == NULL)
    {
      /* We want to stay alive but do nothing, because otherwise every
         request would result in another failed gconfd being spawned.  
      */
      gconf_log(GCL_ERR, _("No configuration sources in the source path, configuration won't be saved; edit %s"), GCONF_CONFDIR"/path");
      /* don't request error since there aren't any addresses */
      sources = gconf_sources_new_from_addresses(NULL, NULL);

      /* Install the sources as the default database */
      set_default_database (gconf_database_new(sources));
    }
  else
    {
      sources = gconf_sources_new_from_addresses(addresses, &error);

      if (error != NULL)
        {
          gconf_log(GCL_ERR, _("Error loading some config sources: %s"),
                    error->message);

          g_error_free(error);
          error = NULL;
        }
      
      g_slist_free(addresses);

      g_assert(sources != NULL);

      if (sources->sources == NULL)
        gconf_log(GCL_ERR, _("No config source addresses successfully resolved, can't load or store config data"));
    
      tmp = sources->sources;

      while (tmp != NULL)
        {
          if (((GConfSource*)tmp->data)->flags & GCONF_SOURCE_ALL_WRITEABLE)
            {
              have_writable = TRUE;
              break;
            }

          tmp = g_list_next(tmp);
        }

      /* In this case, some sources may still return TRUE from their writable() function */
      if (!have_writable)
        gconf_log(GCL_WARNING, _("No writable config sources successfully resolved, may not be able to save some configuration changes"));

        
      /* Install the sources as the default database */
      set_default_database (gconf_database_new(sources));
    }
}

static void
signal_handler (int signo)
{
  static gint in_fatal = 0;

  /* avoid loops */
  if (in_fatal > 0)
    return;
  
  ++in_fatal;
  
  switch (signo) {
    /* Fast cleanup only */
  case SIGSEGV:
  case SIGBUS:
  case SIGILL:
    enter_shutdown ();
    gconf_log (GCL_ERR,
               _("Received signal %d, dumping core. Please report a GConf bug."),
               signo);
    if (g_getenv ("DISPLAY"))
      gconf_handle_segv (signo);
    abort ();
    break;

  case SIGFPE:
  case SIGPIPE:
    /* Go ahead and try the full cleanup on these,
     * though it could well not work out very well.
     */
    enter_shutdown ();

    /* let the fatal signals interrupt us */
    --in_fatal;
    
    gconf_log (GCL_ERR,
               _("Received signal %d, shutting down abnormally. Please file a GConf bug report."),
               signo);


    if (gconf_main_is_running ())
      gconf_main_quit ();
    
    break;

  case SIGTERM:
  case SIGHUP:
    enter_shutdown ();

    /* let the fatal signals interrupt us */
    --in_fatal;
    
    gconf_log (GCL_INFO,
               _("Received signal %d, shutting down cleanly"), signo);

    if (gconf_main_is_running ())
      gconf_main_quit ();
    break;

  case SIGUSR1:
    --in_fatal;
    
    /* it'd be nice to log a message here but it's not very safe, so */
    gconf_log_debug_messages = !gconf_log_debug_messages;
    break;
    
  default:
    break;
  }
}


static void
log_handler (const gchar   *log_domain,
             GLogLevelFlags log_level,
             const gchar   *message,
             gpointer       user_data)
{
  GConfLogPriority pri = GCL_WARNING;
  
  switch (log_level & G_LOG_LEVEL_MASK)
    {
    case G_LOG_LEVEL_ERROR:
    case G_LOG_LEVEL_CRITICAL:
      pri = GCL_ERR;
      break;

    case G_LOG_LEVEL_WARNING:
      pri = GCL_WARNING;
      break;

    case G_LOG_LEVEL_MESSAGE:
    case G_LOG_LEVEL_INFO:
      pri = GCL_INFO;
      break;

    case G_LOG_LEVEL_DEBUG:
      pri = GCL_DEBUG;
      break;

    default:
      break;
    }

  gconf_log (pri, "%s", message);
}

#ifdef HAVE_ORBIT
/* From ORBit2 */
/* There is a DOS attack if another user creates
 * the given directory and keeps us from creating
 * it
 */
static gboolean
test_safe_tmp_dir (const char *dirname)
{
  struct stat statbuf;
  int fd;

  fd = open (dirname, O_RDONLY);  
  if (fd < 0)
    {
      gconf_log (GCL_WARNING, _("Failed to open %s: %s"),
                 dirname, g_strerror (errno));
      return FALSE;
    }
  
  if (fstat (fd, &statbuf) != 0)
    {
      gconf_log (GCL_WARNING, _("Failed to stat %s: %s"),
                 dirname, g_strerror (errno));
      close (fd);
      return FALSE;
    }
  close (fd);
  
  if (statbuf.st_uid != getuid ())
    {
      gconf_log (GCL_WARNING, _("Owner of %s is not the current user"),
                 dirname);
      return FALSE;
    }
  
  if ((statbuf.st_mode & (S_IRWXG|S_IRWXO)) ||
      !S_ISDIR (statbuf.st_mode))
    {
      gconf_log (GCL_WARNING, _("Bad permissions %lo on directory %s"),
                 (unsigned long) statbuf.st_mode & 07777, dirname);
      return FALSE;
    }
  
  return TRUE;
}
#endif

int 
main(int argc, char** argv)
{
  struct sigaction act;
  sigset_t empty_mask;
  gchar* logname;
  const gchar* username;
  int exit_code = 0;
  GError *err;
  char *lock_dir;
  char *gconfd_dir;
  int dev_null_fd;
  int write_byte_fd;

  _gconf_init_i18n ();
  setlocale (LC_ALL, "");
  textdomain (GETTEXT_PACKAGE);
  
  /* Now this is an argument parser */
  if (argc > 1)
    write_byte_fd = atoi (argv[1]);
  else
    write_byte_fd = -1;
  
  /* This is so we don't prevent unmounting of devices. We divert
   * all messages to syslog
   */
  if (chdir ("/") < 0)
    {
       g_printerr ("Could not change to root directory: %s\n",
		g_strerror (errno));
       exit (1);
    }

#ifndef DAEMON_DEBUG
  if (!g_getenv ("GCONF_DEBUG_OUTPUT"))
    {
      dev_null_fd = open ("/dev/null", O_RDWR);
      if (dev_null_fd >= 0)
	{
	  dup2 (dev_null_fd, 0);
	  dup2 (dev_null_fd, 1);
	  dup2 (dev_null_fd, 2);
	}
    }
  else
#endif
    {
      gconf_log_debug_messages = TRUE;
    }
  
  umask (022);

#ifndef DAEMON_DEBUG
  gconf_set_daemon_mode(TRUE);
#endif
  
  /* Logs */
  username = g_get_user_name();
  logname = g_strdup_printf("gconfd (%s-%u)", username, (guint)getpid());

  openlog (logname, LOG_NDELAY, LOG_USER);

  g_log_set_handler (NULL, G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION,
                     log_handler, NULL);

  g_log_set_handler ("GLib", G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION,
                     log_handler, NULL);

  g_log_set_handler ("GLib-GObject", G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION,
                     log_handler, NULL);
  
  /* openlog() does not copy logname - what total brokenness.
     So we free it at the end of main() */
  
  gconf_log (GCL_INFO, _("starting (version %s), pid %u user '%s'"), 
             VERSION, (guint)getpid(), g_get_user_name());

#ifdef GCONF_ENABLE_DEBUG
  gconf_log (GCL_DEBUG, "GConf was built with debugging features enabled");
#endif
  
  /* Session setup */
  sigemptyset (&empty_mask);
  act.sa_handler = signal_handler;
  act.sa_mask    = empty_mask;
  act.sa_flags   = 0;
  sigaction (SIGTERM,  &act, 0);
  sigaction (SIGILL,  &act, 0);
  sigaction (SIGBUS,  &act, 0);
  sigaction (SIGFPE,  &act, 0);
  sigaction (SIGHUP,  &act, 0);
  sigaction (SIGSEGV, &act, 0);
  sigaction (SIGABRT, &act, 0);
  sigaction (SIGUSR1,  &act, 0);
  
  act.sa_handler = SIG_IGN;
#ifndef DAEMON_DEBUG
  sigaction (SIGINT, &act, 0);
#endif
  
  init_databases ();

#ifdef HAVE_ORBIT
  if (!gconfd_corba_init ())
    return 1;
#endif
#ifdef HAVE_DBUS
  if (!gconfd_dbus_init ())
    return 1;
#endif
  
  gconfd_dir = gconf_get_daemon_dir ();
  lock_dir = gconf_get_lock_dir ();
  
  if (mkdir (gconfd_dir, 0700) < 0 && errno != EEXIST)
    gconf_log (GCL_WARNING, _("Failed to create %s: %s"),
               gconfd_dir, g_strerror (errno));

#ifdef HAVE_ORBIT
  if (!test_safe_tmp_dir (gconfd_dir))
    {
      err = g_error_new (GCONF_ERROR,
                         GCONF_ERROR_LOCK_FAILED,
                         _("Directory %s has a problem, gconfd can't use it"),
                         gconfd_dir);
      daemon_lock = NULL;
    }
  else
    {
      err = NULL;

      daemon_lock = gconf_get_lock_or_current_holder (lock_dir, NULL, &err);
    }
#else
  /* FIXME: get rid of locks completely for dbus case, just get a fake one for
   * now.
   */
  daemon_lock = gconf_get_lock (NULL, NULL);
#endif
  
  if (daemon_lock != NULL)
    {
      /* This loads backends and so on. It needs to be done before
       * we can handle any requests, so before we hit the
       * main loop. if daemon_lock == NULL we won't hit the
       * main loop.
       */
      gconf_server_load_sources ();
    }

#if HAVE_ORBIT
  /* notify caller that we're done either getting the lock
   * or not getting it
   */
  if (write_byte_fd >= 0)
    {
      char buf[1] = { 'g' };
      if (write (write_byte_fd, buf, 1) != 1)
        {
          gconf_log (GCL_ERR, _("Failed to write byte to pipe fd %d so client program may hang: %s"), write_byte_fd, g_strerror (errno));
        }

      close (write_byte_fd);
    }
#else
  if (write_byte_fd >= 0)
    close (write_byte_fd);  
#endif
  
  if (daemon_lock == NULL)
    {
      g_assert (err);

      gconf_log (GCL_WARNING, _("Failed to get lock for daemon, exiting: %s"),
                 err->message);
      g_error_free (err);

      enter_shutdown ();
      shutdown_databases ();
      
      return 1;
    }  

#ifdef HAVE_ORBIT
  /* Read saved log file, if any */
  gconfd_corba_logfile_read ();
#endif
  
  gconf_main ();

  if (in_shutdown)
    exit_code = 1; /* means someone already called enter_shutdown() */
  
  /* This starts bouncing all incoming requests (and we aren't running
   * the main loop anyway, so they won't get processed)
   */
  enter_shutdown ();

#ifdef HAVE_ORBIT
  /* Save current state in logfile (may compress the logfile a good
   * bit)
   */
  gconfd_corba_logfile_save ();
#endif
  
  shutdown_databases ();

  gconfd_locale_cache_drop ();

  if (daemon_lock)
    {
      err = NULL;
      gconf_release_lock (daemon_lock, &err);
      if (err != NULL)
        {
          gconf_log (GCL_WARNING, _("Error releasing lockfile: %s"),
                     err->message);
          g_error_free (err);
        }
    }

  daemon_lock = NULL;

  gconf_log (GCL_INFO, _("Exiting"));

  closelog ();
  
  /* Can't do this due to stupid atexit() handler that calls g_log stuff */
  /*   g_free (logname); */
  
  return exit_code;
}

/*
 * Main loop
 */

static GSList* main_loops = NULL;
static guint timeout_id = 0;
static gboolean need_log_cleanup = FALSE;

static gint
get_client_count (void)
{
  int client_count = 0;

#ifdef HAVE_ORBIT
  client_count += gconfd_corba_client_count ();
#endif
#ifdef HAVE_DBUS
  client_count += gconfd_dbus_client_count ();
#endif

  return client_count;
}

static gboolean
periodic_cleanup_timeout(gpointer data)
{  
  /*gconf_log (GCL_DEBUG, "Performing periodic cleanup, expiring cache cruft");*/

#ifdef HAVE_ORBIT
  gconfd_corba_drop_old_clients ();
#endif
  drop_old_databases ();

  if (no_databases_in_use () && get_client_count () == 0)
    {
      gconf_log (GCL_INFO, _("GConf server is not in use, shutting down."));
      gconf_main_quit ();
      return FALSE;
    }
  
  /* expire old locale cache entries */
  gconfd_locale_cache_expire ();

  if (!need_log_cleanup)
    {
      /*gconf_log (GCL_DEBUG, "No log file saving needed in periodic cleanup handler");*/
      return TRUE;
    }

#ifdef HAVE_ORBIT
  /* Compress the running state file */
  gconfd_corba_logfile_save ();
#endif
  
  need_log_cleanup = FALSE;
  
  return TRUE;
}

void
gconfd_need_log_cleanup (void)
{
  need_log_cleanup = TRUE;
}

static void
gconf_main(void)
{
  GMainLoop* loop;

  loop = g_main_loop_new (NULL, TRUE);

  if (main_loops == NULL)
    {
      gulong timeout_len = 1000*60*0.5; /* 1 sec * 60 s/min * .5 min */
      
      g_assert(timeout_id == 0);
      timeout_id = g_timeout_add (timeout_len,
                                  periodic_cleanup_timeout,
                                  NULL);

    }
  
  main_loops = g_slist_prepend(main_loops, loop);

  g_main_loop_run (loop);

  main_loops = g_slist_remove(main_loops, loop);

  if (main_loops == NULL)
    {
      g_assert(timeout_id != 0);
      g_source_remove(timeout_id);
      timeout_id = 0;
    }
  
  g_main_loop_unref (loop);
}

void 
gconf_main_quit(void)
{
  g_return_if_fail(main_loops != NULL);

  g_main_loop_quit (main_loops->data);
}

static gboolean
gconf_main_is_running (void)
{
  return main_loops != NULL;
}

/*
 * Database storage
 */

static GList* db_list = NULL;
static GHashTable* dbs_by_address = NULL;
static GConfDatabase *default_db = NULL;

static void
init_databases (void)
{
  gconfd_need_log_cleanup ();
  
  g_assert(db_list == NULL);
  g_assert(dbs_by_address == NULL);
  
  dbs_by_address = g_hash_table_new (g_str_hash, g_str_equal);

  /* Default database isn't in the address hash since it has
     multiple addresses in a stack
  */
}

static void
set_default_database (GConfDatabase* db)
{
  gconfd_need_log_cleanup ();
  
  default_db = db;
  
  /* Default database isn't in the address hash since it has
     multiple addresses in a stack
  */
}

static void
register_database (GConfDatabase *db)
{
  gconfd_need_log_cleanup ();
  
  if (db->sources->sources)
    safe_g_hash_table_insert(dbs_by_address,
                             ((GConfSource*)db->sources->sources->data)->address,
                             db);
  
  db_list = g_list_prepend (db_list, db);
}

static void
unregister_database (GConfDatabase *db)
{
  gconfd_need_log_cleanup ();
  
  if (db->sources->sources)
    g_hash_table_remove(dbs_by_address,
                        ((GConfSource*)(db->sources->sources->data))->address);

  db_list = g_list_remove (db_list, db);

  gconf_database_free (db);
}

GConfDatabase*
gconfd_lookup_database (const gchar *address)
{
  if (address == NULL)
    return default_db;
  else
    return g_hash_table_lookup (dbs_by_address, address);
}

GList *
gconfd_get_database_list (void)
{
  return db_list;
}

GConfDatabase*
gconfd_obtain_database (const gchar *address,
			GError **err)
{
  
  GConfSources* sources;
  GSList* addresses = NULL;
  GError* error = NULL;
  GConfDatabase *db;

  db = gconfd_lookup_database (address);

  if (db)
    return db;

  addresses = g_slist_append(addresses, g_strdup(address));
  sources = gconf_sources_new_from_addresses(addresses, &error);
  g_slist_free (addresses);

  if (error != NULL)
    {
      if (err)
        *err = error;
      else
        g_error_free (error);

      return NULL;
    }
  
  if (sources == NULL)
    return NULL;

  db = gconf_database_new (sources);

  register_database (db);

  return db;
}

static void
drop_old_databases(void)
{
  GList *tmp_list;
  GList *dead = NULL;
  GTime now;
  
  now = time(NULL);

#ifdef HAVE_ORBIT
  gconf_database_corba_drop_dead_listeners (default_db);
#endif
  
  tmp_list = db_list;
  while (tmp_list)
    {
      GConfDatabase* db = tmp_list->data;

#ifdef HAVE_ORBIT
      /* Drop any listeners whose clients are gone. */
      gconf_database_corba_drop_dead_listeners (db);
#endif
      if (db->listeners &&                             /* not already hibernating */
          gconf_listeners_count(db->listeners) == 0 && /* Can hibernate */
          (now - db->last_access) > (60*20))           /* 20 minutes without access */
        {
          dead = g_list_prepend (dead, db);
        }
      
      tmp_list = g_list_next (tmp_list);
    }

  
  tmp_list = dead;
  while (tmp_list)
    {
      GConfDatabase* db = tmp_list->data;

      unregister_database (db);
            
      tmp_list = g_list_next (tmp_list);
    }

  g_list_free (dead);
}

static void
shutdown_databases (void)
{
  GList *tmp_list;  

  /* This may be called before we init fully,
   * so check that everything != NULL
   */
  
  tmp_list = db_list;

  while (tmp_list)
    {
      GConfDatabase *db = tmp_list->data;

      gconf_database_free (db);
      
      tmp_list = g_list_next (tmp_list);
    }

  g_list_free (db_list);
  db_list = NULL;

  if (dbs_by_address)
    g_hash_table_destroy(dbs_by_address);

  dbs_by_address = NULL;

  if (default_db)
    gconf_database_free (default_db);

  default_db = NULL;
}

static gboolean
no_databases_in_use (void)
{
  /* Only the default database still open, and
   * it has no listeners
   */
  return db_list == NULL &&
    gconf_listeners_count (default_db->listeners) == 0;
}

/*
 * Cleanup
 */

static void 
enter_shutdown(void)
{
  in_shutdown = TRUE;
}


/* Exceptions */

static void
gconf_handle_segv (int signum)
{
  return;
}

gboolean
gconfd_in_shutdown (void)
{
  return in_shutdown;
}

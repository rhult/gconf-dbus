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
#include "gconf-corba-utils.h"
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <locale.h>
#include <time.h>
#include <math.h>
#include "gconf.h"

static gchar* daemon_ior = NULL;

void
gconf_set_daemon_ior(const gchar* ior)
{
  if (daemon_ior != NULL)
    {
      g_free(daemon_ior);
      daemon_ior = NULL;
    }
      
  if (ior != NULL)
    daemon_ior = g_strdup(ior);
}

const gchar*
gconf_get_daemon_ior(void)
{
  return daemon_ior;
}

static CORBA_ORB gconf_orb = CORBA_OBJECT_NIL;      

CORBA_ORB
gconf_orb_get (void)
{
  if (gconf_orb == CORBA_OBJECT_NIL)
    {
      CORBA_Environment ev;
      int argc = 1;
      char *argv[] = { "gconf", NULL };

      _gconf_init_i18n ();
      
      CORBA_exception_init (&ev);
      
      gconf_orb = CORBA_ORB_init (&argc, argv, "orbit-local-orb", &ev);
      g_assert (ev._major == CORBA_NO_EXCEPTION);

      CORBA_exception_free (&ev);
    }

  return gconf_orb;
}

int
gconf_orb_release (void)
{
  int ret = 0;

  if (gconf_orb != CORBA_OBJECT_NIL)
    {
      CORBA_ORB orb = gconf_orb;
      CORBA_Environment ev;

      gconf_orb = CORBA_OBJECT_NIL;

      CORBA_exception_init (&ev);

      CORBA_ORB_destroy (orb, &ev);
      CORBA_Object_release ((CORBA_Object)orb, &ev);

      if (ev._major != CORBA_NO_EXCEPTION)
        {
          ret = 1;
        }
      CORBA_exception_free (&ev);
    }

  return ret;
}

gchar*
gconf_object_to_string (CORBA_Object obj,
                        GError **err)
{
  CORBA_Environment ev;
  gchar *ior;
  gchar *retval;
  
  CORBA_exception_init (&ev);

  ior = CORBA_ORB_object_to_string (gconf_orb_get (), obj, &ev);

  if (ior == NULL)
    {
      gconf_set_error (err,
                       GCONF_ERROR_FAILED,
                       _("Failed to convert object to IOR"));

      return NULL;
    }

  retval = g_strdup (ior);

  CORBA_free (ior);

  return retval;
}

gboolean
gconf_CORBA_Object_equal (gconstpointer a, gconstpointer b)
{
  CORBA_Environment ev;
  CORBA_Object _obj_a = (gpointer)a;
  CORBA_Object _obj_b = (gpointer)b;
  gboolean retval;

  CORBA_exception_init (&ev);
  retval = CORBA_Object_is_equivalent(_obj_a, _obj_b, &ev);
  CORBA_exception_free (&ev);

  return retval;
}

guint
gconf_CORBA_Object_hash (gconstpointer key)
{
  CORBA_Environment ev;
  CORBA_Object _obj = (gpointer)key;
  CORBA_unsigned_long retval;

  CORBA_exception_init (&ev);
  retval = CORBA_Object_hash(_obj, G_MAXUINT, &ev);
  CORBA_exception_free (&ev);

  return retval;
}


/*
 * Activation
 */

static void
set_cloexec (gint fd)
{
  fcntl (fd, F_SETFD, FD_CLOEXEC);
}

static void
close_fd_func (gpointer data)
{
  int *pipes = data;
  
  gint open_max;
  gint i;
  
  open_max = sysconf (_SC_OPEN_MAX);
  for (i = 3; i < open_max; i++)
    {
      /* don't close our write pipe */
      if (i != pipes[1])
        set_cloexec (i);
    }
}

ConfigServer
gconf_activate_server (gboolean  start_if_not_found,
                       GError  **error)
{
  ConfigServer server = CORBA_OBJECT_NIL;
  int p[2] = { -1, -1 };
  char buf[1];
  GError *tmp_err;
  char *argv[3];
  char *gconfd_dir;
  char *lock_dir;
  GString *failure_log;
  struct stat statbuf;
  CORBA_Environment ev;
  gboolean dir_accessible;

  failure_log = g_string_new (NULL);
  
  gconfd_dir = gconf_get_daemon_dir ();

  dir_accessible = stat (gconfd_dir, &statbuf) >= 0;

  if (!dir_accessible && errno != ENOENT)
    {
      server = CORBA_OBJECT_NIL;
      gconf_log (GCL_WARNING, _("Failed to stat %s: %s"),
		 gconfd_dir, g_strerror (errno));
     }
  else if (dir_accessible)
    {
      g_string_append (failure_log, " 1: ");
      lock_dir = gconf_get_lock_dir ();
      server = gconf_get_current_lock_holder (lock_dir, failure_log);
      g_free (lock_dir);
      
      /* Confirm server exists */
      CORBA_exception_init (&ev);

      if (!CORBA_Object_is_nil (server, &ev))
	{
	  ConfigServer_ping (server, &ev);
      
	  if (ev._major != CORBA_NO_EXCEPTION)
	    {
	      server = CORBA_OBJECT_NIL;

	      g_string_append_printf (failure_log,
				      _("Server ping error: %s"),
				      CORBA_exception_id (&ev));
	    }
	}

      CORBA_exception_free (&ev);
  
      if (server != CORBA_OBJECT_NIL)
        {
	  g_string_free (failure_log, TRUE);
	  g_free (gconfd_dir);
	  return server;
	}
    }

  g_free (gconfd_dir);

  if (start_if_not_found)
    {
      /* Spawn server */
      if (pipe (p) < 0)
        {
          g_set_error (error,
                       GCONF_ERROR,
                       GCONF_ERROR_NO_SERVER,
                       _("Failed to create pipe for communicating with spawned gconf daemon: %s\n"),
                       g_strerror (errno));
          goto out;
        }

      argv[0] = g_strconcat (GCONF_SERVERDIR, "/" GCONFD, NULL);
      argv[1] = g_strdup_printf ("%d", p[1]);
      argv[2] = NULL;
  
      tmp_err = NULL;
      if (!g_spawn_async (NULL,
                          argv,
                          NULL,
                          G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                          close_fd_func,
                          p,
                          NULL,
                          &tmp_err))
        {
          g_free (argv[0]);
          g_free (argv[1]);
          g_set_error (error,
                       GCONF_ERROR,
                       GCONF_ERROR_NO_SERVER,
                       _("Failed to launch configuration server: %s\n"),
                       tmp_err->message);
          g_error_free (tmp_err);
          goto out;
        }
      
      g_free (argv[0]);
      g_free (argv[1]);
  
      /* Block until server starts up */
      read (p[0], buf, 1);

      g_string_append (failure_log, " 2: ");
      lock_dir = gconf_get_lock_dir ();
      server = gconf_get_current_lock_holder (lock_dir, failure_log);
      g_free (lock_dir);
    }
  
 out:
  if (server == CORBA_OBJECT_NIL &&
      error &&
      *error == NULL)
    g_set_error (error,
                 GCONF_ERROR,
                 GCONF_ERROR_NO_SERVER,
                 _("Failed to contact configuration server; some possible causes are that you need to enable TCP/IP networking for ORBit, or you have stale NFS locks due to a system crash. See http://www.gnome.org/projects/gconf/ for information. (Details - %s)"),
                 failure_log->len > 0 ? failure_log->str : _("none"));

  g_string_free (failure_log, TRUE);
  
  close (p[0]);
  close (p[1]);
  
  return server;
}


/*
 * CORBA / GConfValue glue
 */

GConfValue* 
gconf_value_from_corba_value(const ConfigValue* value)
{
  GConfValue* gval;
  GConfValueType type = GCONF_VALUE_INVALID;
  
  switch (value->_d)
    {
    case InvalidVal:
      return NULL;
      break;
    case IntVal:
      type = GCONF_VALUE_INT;
      break;
    case StringVal:
      type = GCONF_VALUE_STRING;
      break;
    case FloatVal:
      type = GCONF_VALUE_FLOAT;
      break;
    case BoolVal:
      type = GCONF_VALUE_BOOL;
      break;
    case SchemaVal:
      type = GCONF_VALUE_SCHEMA;
      break;
    case ListVal:
      type = GCONF_VALUE_LIST;
      break;
    case PairVal:
      type = GCONF_VALUE_PAIR;
      break;
    default:
      gconf_log(GCL_DEBUG, "Invalid type in %s", G_GNUC_FUNCTION);
      return NULL;
    }

  g_assert(GCONF_VALUE_TYPE_VALID(type));
  
  gval = gconf_value_new(type);

  switch (gval->type)
    {
    case GCONF_VALUE_INT:
      gconf_value_set_int(gval, value->_u.int_value);
      break;
    case GCONF_VALUE_STRING:
      if (!g_utf8_validate (value->_u.string_value, -1, NULL))
        {
          gconf_log (GCL_ERR, _("Invalid UTF-8 in string value in '%s'"),
                     value->_u.string_value); 
        }
      else
        {
          gconf_value_set_string(gval, value->_u.string_value);
        }
      break;
    case GCONF_VALUE_FLOAT:
      gconf_value_set_float(gval, value->_u.float_value);
      break;
    case GCONF_VALUE_BOOL:
      gconf_value_set_bool(gval, value->_u.bool_value);
      break;
    case GCONF_VALUE_SCHEMA:
      gconf_value_set_schema_nocopy(gval, 
                                    gconf_schema_from_corba_schema(&(value->_u.schema_value)));
      break;
    case GCONF_VALUE_LIST:
      {
        GSList* list = NULL;
        guint i = 0;
        
        switch (value->_u.list_value.list_type)
          {
          case BIntVal:
            gconf_value_set_list_type(gval, GCONF_VALUE_INT);
            break;
          case BBoolVal:
            gconf_value_set_list_type(gval, GCONF_VALUE_BOOL);
            break;
          case BFloatVal:
            gconf_value_set_list_type(gval, GCONF_VALUE_FLOAT);
            break;
          case BStringVal:
            gconf_value_set_list_type(gval, GCONF_VALUE_STRING);
            break;
          case BInvalidVal:
            break;
          default:
            g_warning("Bizarre list type in %s", G_GNUC_FUNCTION);
            break;
          }

        if (gconf_value_get_list_type(gval) != GCONF_VALUE_INVALID)
          {
            i = 0;
            while (i < value->_u.list_value.seq._length)
              {
                GConfValue* val;
                
                /* This is a bit dubious; we cast a ConfigBasicValue to ConfigValue
                   because they have the same initial members, but by the time
                   the CORBA and C specs kick in, not sure we are guaranteed
                   to be able to do this.
                */
                val = gconf_value_from_corba_value((ConfigValue*)&value->_u.list_value.seq._buffer[i]);
                
                if (val == NULL)
                  gconf_log(GCL_ERR, _("Couldn't interpret CORBA value for list element"));
                else if (val->type != gconf_value_get_list_type(gval))
                  gconf_log(GCL_ERR, _("Incorrect type for list element in %s"), G_GNUC_FUNCTION);
                else
                  list = g_slist_prepend(list, val);
                
                ++i;
              }
        
            list = g_slist_reverse(list);
            
            gconf_value_set_list_nocopy(gval, list);
          }
        else
          {
            gconf_log(GCL_ERR, _("Received list from gconfd with a bad list type"));
          }
      }
      break;
    case GCONF_VALUE_PAIR:
      {
        g_return_val_if_fail(value->_u.pair_value._length == 2, gval);
        
        gconf_value_set_car_nocopy(gval,
                                   gconf_value_from_corba_value((ConfigValue*)&value->_u.list_value.seq._buffer[0]));

        gconf_value_set_cdr_nocopy(gval,
                                   gconf_value_from_corba_value((ConfigValue*)&value->_u.list_value.seq._buffer[1]));
      }
      break;
    default:
      g_assert_not_reached();
      break;
    }
  
  return gval;
}

void          
gconf_fill_corba_value_from_gconf_value(const GConfValue *value, 
                                        ConfigValue      *cv)
{
  if (value == NULL)
    {
      cv->_d = InvalidVal;
      return;
    }

  switch (value->type)
    {
    case GCONF_VALUE_INT:
      cv->_d = IntVal;
      cv->_u.int_value = gconf_value_get_int(value);
      break;
    case GCONF_VALUE_STRING:
      cv->_d = StringVal;
      cv->_u.string_value = CORBA_string_dup((char*)gconf_value_get_string(value));
      break;
    case GCONF_VALUE_FLOAT:
      cv->_d = FloatVal;
      cv->_u.float_value = gconf_value_get_float(value);
      break;
    case GCONF_VALUE_BOOL:
      cv->_d = BoolVal;
      cv->_u.bool_value = gconf_value_get_bool(value);
      break;
    case GCONF_VALUE_SCHEMA:
      cv->_d = SchemaVal;
      gconf_fill_corba_schema_from_gconf_schema (gconf_value_get_schema(value),
                                                 &cv->_u.schema_value);
      break;
    case GCONF_VALUE_LIST:
      {
        guint n, i;
        GSList* list;
        
        cv->_d = ListVal;

        list = gconf_value_get_list(value);

        n = g_slist_length(list);

        cv->_u.list_value.seq._buffer =
          CORBA_sequence_ConfigBasicValue_allocbuf(n);
        cv->_u.list_value.seq._length = n;
        cv->_u.list_value.seq._maximum = n;
        CORBA_sequence_set_release(&cv->_u.list_value.seq, TRUE);
        
        switch (gconf_value_get_list_type(value))
          {
          case GCONF_VALUE_INT:
            cv->_u.list_value.list_type = BIntVal;
            break;

          case GCONF_VALUE_BOOL:
            cv->_u.list_value.list_type = BBoolVal;
            break;
            
          case GCONF_VALUE_STRING:
            cv->_u.list_value.list_type = BStringVal;
            break;

          case GCONF_VALUE_FLOAT:
            cv->_u.list_value.list_type = BFloatVal;
            break;

          case GCONF_VALUE_SCHEMA:
            cv->_u.list_value.list_type = BSchemaVal;
            break;
            
          default:
            cv->_u.list_value.list_type = BInvalidVal;
            gconf_log(GCL_DEBUG, "Invalid list type in %s", G_GNUC_FUNCTION);
            break;
          }
        
        i= 0;
        while (list != NULL)
          {
            /* That dubious ConfigBasicValue->ConfigValue cast again */
            gconf_fill_corba_value_from_gconf_value((GConfValue*)list->data,
                                                    (ConfigValue*)&cv->_u.list_value.seq._buffer[i]);

            list = g_slist_next(list);
            ++i;
          }
      }
      break;
    case GCONF_VALUE_PAIR:
      {
        cv->_d = PairVal;

        cv->_u.pair_value._buffer =
          CORBA_sequence_ConfigBasicValue_allocbuf(2);
        cv->_u.pair_value._length = 2;
        cv->_u.pair_value._maximum = 2;
        CORBA_sequence_set_release(&cv->_u.pair_value, TRUE);
        
        /* dubious cast */
        gconf_fill_corba_value_from_gconf_value (gconf_value_get_car(value),
                                                 (ConfigValue*)&cv->_u.pair_value._buffer[0]);
        gconf_fill_corba_value_from_gconf_value(gconf_value_get_cdr(value),
                                                (ConfigValue*)&cv->_u.pair_value._buffer[1]);
      }
      break;
      
    case GCONF_VALUE_INVALID:
      cv->_d = InvalidVal;
      break;
    default:
      cv->_d = InvalidVal;
      gconf_log(GCL_DEBUG, "Unknown type in %s", G_GNUC_FUNCTION);
      break;
    }
}

ConfigValue*  
gconf_corba_value_from_gconf_value (const GConfValue* value)
{
  ConfigValue* cv;

  cv = ConfigValue__alloc();

  gconf_fill_corba_value_from_gconf_value(value, cv);

  return cv;
}

ConfigValue*  
gconf_invalid_corba_value ()
{
  ConfigValue* cv;

  cv = ConfigValue__alloc();

  cv->_d = InvalidVal;

  return cv;
}

static ConfigValueType
corba_type_from_gconf_type(GConfValueType type)
{
  switch (type)
    {
    case GCONF_VALUE_INT:
      return IntVal;
    case GCONF_VALUE_BOOL:
      return BoolVal;
    case GCONF_VALUE_FLOAT:
      return FloatVal;
    case GCONF_VALUE_INVALID:
      return InvalidVal;
    case GCONF_VALUE_STRING:
      return StringVal;
    case GCONF_VALUE_SCHEMA:
      return SchemaVal;
    case GCONF_VALUE_LIST:
      return ListVal;
    case GCONF_VALUE_PAIR:
      return PairVal;
    default:
      g_assert_not_reached();
      return InvalidVal;
    }
}

static GConfValueType
gconf_type_from_corba_type(ConfigValueType type)
{
  switch (type)
    {
    case InvalidVal:
      return GCONF_VALUE_INVALID;
    case StringVal:
      return GCONF_VALUE_STRING;
    case IntVal:
      return GCONF_VALUE_INT;
    case FloatVal:
      return GCONF_VALUE_FLOAT;
    case SchemaVal:
      return GCONF_VALUE_SCHEMA;
    case BoolVal:
      return GCONF_VALUE_BOOL;
    case ListVal:
      return GCONF_VALUE_LIST;
    case PairVal:
      return GCONF_VALUE_PAIR;
    default:
      g_assert_not_reached();
      return GCONF_VALUE_INVALID;
    }
}

void          
gconf_fill_corba_schema_from_gconf_schema(const GConfSchema *sc, 
                                          ConfigSchema      *cs)
{
  cs->value_type = corba_type_from_gconf_type (gconf_schema_get_type (sc));
  cs->value_list_type = corba_type_from_gconf_type (gconf_schema_get_list_type (sc));
  cs->value_car_type = corba_type_from_gconf_type (gconf_schema_get_car_type (sc));
  cs->value_cdr_type = corba_type_from_gconf_type (gconf_schema_get_cdr_type (sc));

  cs->locale = CORBA_string_dup (gconf_schema_get_locale (sc) ? gconf_schema_get_locale (sc) : "");
  cs->short_desc = CORBA_string_dup (gconf_schema_get_short_desc (sc) ? gconf_schema_get_short_desc (sc) : "");
  cs->long_desc = CORBA_string_dup (gconf_schema_get_long_desc (sc) ? gconf_schema_get_long_desc (sc) : "");
  cs->owner = CORBA_string_dup (gconf_schema_get_owner (sc) ? gconf_schema_get_owner (sc) : "");

  {
    gchar* encoded;
    GConfValue* default_val;

    default_val = gconf_schema_get_default_value (sc);

    if (default_val)
      {
        encoded = gconf_value_encode (default_val);

        g_assert (encoded != NULL);

        cs->encoded_default_value = CORBA_string_dup (encoded);

        g_free (encoded);
      }
    else
      cs->encoded_default_value = CORBA_string_dup ("");
  }
}

ConfigSchema* 
gconf_corba_schema_from_gconf_schema (const GConfSchema* sc)
{
  ConfigSchema* cs;

  cs = ConfigSchema__alloc ();

  gconf_fill_corba_schema_from_gconf_schema (sc, cs);

  return cs;
}

GConfSchema*  
gconf_schema_from_corba_schema(const ConfigSchema* cs)
{
  GConfSchema* sc;
  GConfValueType type = GCONF_VALUE_INVALID;
  GConfValueType list_type = GCONF_VALUE_INVALID;
  GConfValueType car_type = GCONF_VALUE_INVALID;
  GConfValueType cdr_type = GCONF_VALUE_INVALID;

  type = gconf_type_from_corba_type(cs->value_type);
  list_type = gconf_type_from_corba_type(cs->value_list_type);
  car_type = gconf_type_from_corba_type(cs->value_car_type);
  cdr_type = gconf_type_from_corba_type(cs->value_cdr_type);

  sc = gconf_schema_new();

  gconf_schema_set_type(sc, type);
  gconf_schema_set_list_type(sc, list_type);
  gconf_schema_set_car_type(sc, car_type);
  gconf_schema_set_cdr_type(sc, cdr_type);

  if (*cs->locale != '\0')
    {
      if (!g_utf8_validate (cs->locale, -1, NULL))
        gconf_log (GCL_ERR, _("Invalid UTF-8 in locale for schema"));
      else
        gconf_schema_set_locale(sc, cs->locale);
    }

  if (*cs->short_desc != '\0')
    {
      if (!g_utf8_validate (cs->short_desc, -1, NULL))
        gconf_log (GCL_ERR, _("Invalid UTF-8 in short description for schema"));
      else
        gconf_schema_set_short_desc(sc, cs->short_desc);
    }

  if (*cs->long_desc != '\0')
    {
      if (!g_utf8_validate (cs->long_desc, -1, NULL))
        gconf_log (GCL_ERR, _("Invalid UTF-8 in long description for schema"));
      else
        gconf_schema_set_long_desc(sc, cs->long_desc);
    }

  if (*cs->owner != '\0')
    {
      if (!g_utf8_validate (cs->owner, -1, NULL))
        gconf_log (GCL_ERR, _("Invalid UTF-8 in owner for schema"));
      else
        gconf_schema_set_owner(sc, cs->owner);
    }
      
  {
    GConfValue* val;

    val = gconf_value_decode(cs->encoded_default_value);

    if (val)
      gconf_schema_set_default_value_nocopy(sc, val);
  }
  
  return sc;
}

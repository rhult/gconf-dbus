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

#include <popt.h>
#include "gconf.h"
#include "gconf-internals.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <unistd.h>


gboolean
gconf_key_check (const gchar* key, GError** err)
{
  gchar* why = NULL;
  
  if (key == NULL)
    {
      if (err)
        *err = gconf_error_new (GCONF_ERROR_BAD_KEY,
                                _("Key \"%s\" is NULL"),
                                key);
      return FALSE;
    }
  else if (!gconf_valid_key (key, &why))
    {
      if (err)
        *err = gconf_error_new (GCONF_ERROR_BAD_KEY, _("\"%s\": %s"),
                                key, why);
      g_free(why);
      return FALSE;
    }
  return TRUE;
}

void
gconf_preinit (gpointer app, gpointer mod_info)
{
  /* Deprecated */
}

void
gconf_postinit (gpointer app, gpointer mod_info)
{
  /* Deprecated */
}

/* All deprecated */
const char gconf_version[] = VERSION;

struct poptOption gconf_options[] = {
  {NULL}
};

/* Also deprecated */
gboolean     
gconf_init (int argc, char **argv, GError** err)
{
  
  return TRUE;
}

gboolean
gconf_is_initialized (void)
{
  return TRUE;
}

/* 
 * Ampersand and <> are not allowed due to the XML backend; shell
 * special characters aren't allowed; others are just in case we need
 * some magic characters someday.  hyphen, underscore, period, colon
 * are allowed as separators. % disallowed to avoid printf confusion.
 */

/* Key/dir validity is exactly the same, except that '/' must be a dir, 
   but we are sort of ignoring that for now. */

/* Also, keys can contain only ASCII */

static const gchar invalid_chars[] = " \t\r\n\"$&<>,+=#!()'|{}[]?~`;%\\";

gboolean     
gconf_valid_key      (const gchar* key, gchar** why_invalid)
{
  const gchar* s = key;
  gboolean just_saw_slash = FALSE;

  /* Key must start with the root */
  if (*key != '/')
    {
      if (why_invalid != NULL)
        *why_invalid = g_strdup(_("Must begin with a slash (/)"));
      return FALSE;
    }
  
  /* Root key is a valid dir */
  if (*key == '/' && key[1] == '\0')
    return TRUE;

  while (*s)
    {
      if (just_saw_slash)
        {
          /* Can't have two slashes in a row, since it would mean
           * an empty spot.
           * Can't have a period right after a slash,
           * because it would be a pain for filesystem-based backends.
           */
          if (*s == '/' || *s == '.')
            {
              if (why_invalid != NULL)
                {
                  if (*s == '/')
                    *why_invalid = g_strdup(_("Can't have two slashes (/) in a row"));
                  else
                    *why_invalid = g_strdup(_("Can't have a period (.) right after a slash (/)"));
                }
              return FALSE;
            }
        }

      if (*s == '/')
        {
          just_saw_slash = TRUE;
        }
      else
        {
          const gchar* inv = invalid_chars;

          just_saw_slash = FALSE;
          
          if (((unsigned char)*s) > 127)
            {
              if (why_invalid != NULL)
                *why_invalid = g_strdup_printf (_("'%c' is not an ASCII character, so isn't allowed in key names"),
                                                *s);
              return FALSE;
            }
          
          while (*inv)
            {
              if (*inv == *s)
                {
                  if (why_invalid != NULL)
                    *why_invalid = g_strdup_printf(_("`%c' is an invalid character in key/directory names"), *s);
                  return FALSE;
                }
              ++inv;
            }
        }

      ++s;
    }

  /* Can't end with slash */
  if (just_saw_slash)
    {
      if (why_invalid != NULL)
        *why_invalid = g_strdup(_("Key/directory may not end with a slash (/)"));
      return FALSE;
    }
  else
    return TRUE;
}

/**
 * gconf_escape_key:
 * @arbitrary_text: some text in any encoding or format
 * @len: length of @arbitrary_text in bytes, or -1 if @arbitrary_text is nul-terminated
 * 
 * Escape @arbitrary_text such that it's a valid key element (i.e. one
 * part of the key path). The escaped key won't pass gconf_valid_key()
 * because it isn't a whole key (i.e. it doesn't have a preceding
 * slash), but prepending a slash to the escaped text should always
 * result in a valid key.
 * 
 * Return value: a nul-terminated valid GConf key
 **/
char*
gconf_escape_key (const char *arbitrary_text,
                  int         len)
{
  const char *p;
  const char *end;
  GString *retval;

  g_return_val_if_fail (arbitrary_text != NULL, NULL);
  
  /* Nearly all characters we would normally use for escaping aren't allowed in key
   * names, so we use @ for that.
   *
   * Invalid chars and @ itself are escaped as @xxx@ where xxx is the
   * Latin-1 value in decimal
   */

  if (len < 0)
    len = strlen (arbitrary_text);

  retval = g_string_new (NULL);

  p = arbitrary_text;
  end = arbitrary_text + len;
  while (p != end)
    {
      if (*p == '/' || *p == '.' || *p == '@' || ((guchar) *p) > 127 ||
          strchr (invalid_chars, *p))
        {
          g_string_append_c (retval, '@');
          g_string_append_printf (retval, "%u", (unsigned int) *p);
          g_string_append_c (retval, '@');
        }
      else
        g_string_append_c (retval, *p);
      
      ++p;
    }

  return g_string_free (retval, FALSE);
}

/**
 * gconf_unescape_key:
 * @escaped_key: a key created with gconf_escape_key()
 * @len: length of @escaped_key in bytes, or -1 if @escaped_key is nul-terminated
 * 
 * Converts a string escaped with gconf_escape_key() back into its original
 * form.
 * 
 * Return value: the original string that was escaped to create @escaped_key
 **/
char*
gconf_unescape_key (const char *escaped_key,
                    int         len)
{
  const char *p;
  const char *end;
  const char *start_seq;
  GString *retval;

  g_return_val_if_fail (escaped_key != NULL, NULL);
  
  if (len < 0)
    len = strlen (escaped_key);

  retval = g_string_new (NULL);

  p = escaped_key;
  end = escaped_key + len;
  start_seq = NULL;
  while (p != end)
    {
      if (start_seq)
        {
          if (*p == '@')
            {
              /* *p is the @ that ends a seq */
              char *end;
              guchar val;
              
              val = strtoul (start_seq, &end, 10);
              if (start_seq != end)
                g_string_append_c (retval, val);
              
              start_seq = NULL;
            }
        }
      else
        {
          if (*p == '@')
            start_seq = p + 1;
          else
            g_string_append_c (retval, *p);
        }

      ++p;
    }

  return g_string_free (retval, FALSE);
}


gboolean
gconf_key_is_below   (const gchar* above, const gchar* below)
{
  int len;

  if (above[0] == '/' && above[1] == '\0')
    return TRUE;
  
  len = strlen (above);
  if (strncmp (below, above, len) == 0)
    {
      /* only if this is a complete key component,
       * so that /foo is not above /foofoo/bar */
      if (below[len] == '\0' || below[len] == '/')
        return TRUE;
      else
	return FALSE;
    }
  else
    return FALSE;
}

gchar*
gconf_unique_key (void)
{
  /* This function is hardly cryptographically random but should be
     "good enough" */
  
  static guint serial = 0;
  gchar* key;
  guint t, ut, p, u, r;
  struct timeval tv;
  
  gettimeofday(&tv, NULL);
  
  t = tv.tv_sec;
  ut = tv.tv_usec;

  p = getpid();
  
  u = getuid();

  /* don't bother to seed; if it's based on the time or any other
     changing info we can get, we may as well just use that changing
     info. since we don't seed we'll at least get a different number
     on every call to this function in the same executable. */
  r = rand();
  
  /* The letters may increase uniqueness by preventing "melds"
     i.e. 01t01k01 and 0101t0k1 are not the same */
  key = g_strdup_printf("%ut%uut%uu%up%ur%uk%u",
                        /* Duplicate keys must be generated
                           by two different program instances */
                        serial,
                        /* Duplicate keys must be generated
                           in the same microsecond */
                        t,
                        ut,
                        /* Duplicate keys must be generated by
                           the same user */
                        u,
                        /* Duplicate keys must be generated by
                           two programs that got the same PID */
                        p,
                        /* Duplicate keys must be generated with the
                           same random seed and the same index into
                           the series of pseudorandom values */
                        r,
                        /* Duplicate keys must result from running
                           this function at the same stack location */
                        GPOINTER_TO_UINT(&key));

  ++serial;
  
  return key;
}

/*
 * Sugar functions 
 */

gdouble      
gconf_engine_get_float (GConfEngine* conf, const gchar* key,
                 GError** err)
{
  GConfValue* val;
  static const gdouble deflt = 0.0;
  
  g_return_val_if_fail(conf != NULL, 0.0);
  g_return_val_if_fail(key != NULL, 0.0);
  
  val = gconf_engine_get (conf, key, err);

  if (val == NULL)
    return deflt;
  else
    {
      gdouble retval;
      
      if (val->type != GCONF_VALUE_FLOAT)
        {
          if (err)
            *err = gconf_error_new(GCONF_ERROR_TYPE_MISMATCH, _("Expected float, got %s"),
                                    gconf_value_type_to_string(val->type));
          gconf_value_free(val);
          return deflt;
        }

      retval = gconf_value_get_float(val);

      gconf_value_free(val);

      return retval;
    }
}

gint         
gconf_engine_get_int   (GConfEngine* conf, const gchar* key,
                 GError** err)
{
  GConfValue* val;
  static const gint deflt = 0;
  
  g_return_val_if_fail(conf != NULL, 0);
  g_return_val_if_fail(key != NULL, 0);
  
  val = gconf_engine_get (conf, key, err);

  if (val == NULL)
    return deflt;
  else
    {
      gint retval;

      if (val->type != GCONF_VALUE_INT)
        {
          if (err)
            *err = gconf_error_new(GCONF_ERROR_TYPE_MISMATCH, _("Expected int, got %s"),
                                    gconf_value_type_to_string(val->type));
          gconf_value_free(val);
          return deflt;
        }

      retval = gconf_value_get_int(val);

      gconf_value_free(val);

      return retval;
    }
}

gchar*       
gconf_engine_get_string(GConfEngine* conf, const gchar* key,
                 GError** err)
{
  GConfValue* val;
  static const gchar* deflt = NULL;
  
  g_return_val_if_fail(conf != NULL, NULL);
  g_return_val_if_fail(key != NULL, NULL);
  
  val = gconf_engine_get (conf, key, err);

  if (val == NULL)
    return deflt ? g_strdup(deflt) : NULL;
  else
    {
      gchar* retval;

      if (val->type != GCONF_VALUE_STRING)
        {
          if (err)
            *err = gconf_error_new(GCONF_ERROR_TYPE_MISMATCH, _("Expected string, got %s"),
                                    gconf_value_type_to_string(val->type));
          gconf_value_free(val);
          return deflt ? g_strdup(deflt) : NULL;
        }

      retval = gconf_value_steal_string (val);
      gconf_value_free (val);

      return retval;
    }
}

gboolean     
gconf_engine_get_bool  (GConfEngine* conf, const gchar* key,
                        GError** err)
{
  GConfValue* val;
  static const gboolean deflt = FALSE;
  
  g_return_val_if_fail(conf != NULL, FALSE);
  g_return_val_if_fail(key != NULL, FALSE);
  
  val = gconf_engine_get (conf, key, err);

  if (val == NULL)
    return deflt;
  else
    {
      gboolean retval;

      if (val->type != GCONF_VALUE_BOOL)
        {
          if (err)
            *err = gconf_error_new(GCONF_ERROR_TYPE_MISMATCH, _("Expected bool, got %s"),
                                    gconf_value_type_to_string(val->type));
          gconf_value_free(val);
          return deflt;
        }

      retval = gconf_value_get_bool(val);

      gconf_value_free(val);

      return retval;
    }
}

GConfSchema* 
gconf_engine_get_schema  (GConfEngine* conf, const gchar* key, GError** err)
{
  GConfValue* val;

  g_return_val_if_fail(conf != NULL, NULL);
  g_return_val_if_fail(key != NULL, NULL);
  
  val = gconf_engine_get_with_locale(conf, key, gconf_current_locale(), err);

  if (val == NULL)
    return NULL;
  else
    {
      GConfSchema* retval;

      if (val->type != GCONF_VALUE_SCHEMA)
        {
          if (err)
            *err = gconf_error_new(GCONF_ERROR_TYPE_MISMATCH, _("Expected schema, got %s"),
                                    gconf_value_type_to_string(val->type));
          gconf_value_free(val);
          return NULL;
        }

      retval = gconf_value_steal_schema (val);
      gconf_value_free (val);

      return retval;
    }
}

GSList*
gconf_engine_get_list    (GConfEngine* conf, const gchar* key,
                          GConfValueType list_type, GError** err)
{
  GConfValue* val;

  g_return_val_if_fail(conf != NULL, NULL);
  g_return_val_if_fail(key != NULL, NULL);
  g_return_val_if_fail(list_type != GCONF_VALUE_INVALID, NULL);
  g_return_val_if_fail(list_type != GCONF_VALUE_LIST, NULL);
  g_return_val_if_fail(list_type != GCONF_VALUE_PAIR, NULL);
  g_return_val_if_fail(err == NULL || *err == NULL, NULL);
  
  val = gconf_engine_get_with_locale(conf, key, gconf_current_locale(), err);

  if (val == NULL)
    return NULL;
  else
    {
      /* This type-checks the value */
      return gconf_value_list_to_primitive_list_destructive(val, list_type, err);
    }
}

gboolean
gconf_engine_get_pair    (GConfEngine* conf, const gchar* key,
                   GConfValueType car_type, GConfValueType cdr_type,
                   gpointer car_retloc, gpointer cdr_retloc,
                   GError** err)
{
  GConfValue* val;
  GError* error = NULL;
  
  g_return_val_if_fail(conf != NULL, FALSE);
  g_return_val_if_fail(key != NULL, FALSE);
  g_return_val_if_fail(car_type != GCONF_VALUE_INVALID, FALSE);
  g_return_val_if_fail(car_type != GCONF_VALUE_LIST, FALSE);
  g_return_val_if_fail(car_type != GCONF_VALUE_PAIR, FALSE);
  g_return_val_if_fail(cdr_type != GCONF_VALUE_INVALID, FALSE);
  g_return_val_if_fail(cdr_type != GCONF_VALUE_LIST, FALSE);
  g_return_val_if_fail(cdr_type != GCONF_VALUE_PAIR, FALSE);
  g_return_val_if_fail(car_retloc != NULL, FALSE);
  g_return_val_if_fail(cdr_retloc != NULL, FALSE);
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);  
  
  val = gconf_engine_get_with_locale(conf, key, gconf_current_locale(), &error);

  if (error != NULL)
    {
      g_assert(val == NULL);
      
      if (err)
        *err = error;
      else
        g_error_free(error);

      return FALSE;
    }
  
  if (val == NULL)
    {
      return TRUE;
    }
  else
    {
      /* Destroys val */
      return gconf_value_pair_to_primitive_pair_destructive(val,
                                                            car_type, cdr_type,
                                                            car_retloc, cdr_retloc,
                                                            err);
    }
}

/*
 * Setters
 */

static gboolean
error_checked_set(GConfEngine* conf, const gchar* key,
                  GConfValue* gval, GError** err)
{
  GError* my_err = NULL;
  
  gconf_engine_set (conf, key, gval, &my_err);

  gconf_value_free(gval);
  
  if (my_err != NULL)
    {
      if (err)
        *err = my_err;
      else
        g_error_free(my_err);
      return FALSE;
    }
  else
    return TRUE;
}

gboolean
gconf_engine_set_float   (GConfEngine* conf, const gchar* key,
                          gdouble val, GError** err)
{
  GConfValue* gval;

  g_return_val_if_fail(conf != NULL, FALSE);
  g_return_val_if_fail(key != NULL, FALSE);
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);
  
  gval = gconf_value_new(GCONF_VALUE_FLOAT);

  gconf_value_set_float(gval, val);

  return error_checked_set(conf, key, gval, err);
}

gboolean
gconf_engine_set_int     (GConfEngine* conf, const gchar* key,
                          gint val, GError** err)
{
  GConfValue* gval;

  g_return_val_if_fail(conf != NULL, FALSE);
  g_return_val_if_fail(key != NULL, FALSE);
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);
  
  gval = gconf_value_new(GCONF_VALUE_INT);

  gconf_value_set_int(gval, val);

  return error_checked_set(conf, key, gval, err);
}

gboolean
gconf_engine_set_string  (GConfEngine* conf, const gchar* key,
                          const gchar* val, GError** err)
{
  GConfValue* gval;

  g_return_val_if_fail (val != NULL, FALSE);
  g_return_val_if_fail (conf != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);
  g_return_val_if_fail (err == NULL || *err == NULL, FALSE);
  
  g_return_val_if_fail (g_utf8_validate (val, -1, NULL), FALSE);
  
  gval = gconf_value_new(GCONF_VALUE_STRING);

  gconf_value_set_string(gval, val);

  return error_checked_set(conf, key, gval, err);
}

gboolean
gconf_engine_set_bool    (GConfEngine* conf, const gchar* key,
                          gboolean val, GError** err)
{
  GConfValue* gval;

  g_return_val_if_fail(conf != NULL, FALSE);
  g_return_val_if_fail(key != NULL, FALSE);
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);
  
  gval = gconf_value_new(GCONF_VALUE_BOOL);

  gconf_value_set_bool(gval, !!val); /* canonicalize the bool */

  return error_checked_set(conf, key, gval, err);
}

gboolean
gconf_engine_set_schema  (GConfEngine* conf, const gchar* key,
                          const GConfSchema* val, GError** err)
{
  GConfValue* gval;

  g_return_val_if_fail(conf != NULL, FALSE);
  g_return_val_if_fail(key != NULL, FALSE);
  g_return_val_if_fail(val != NULL, FALSE);
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);
  
  gval = gconf_value_new(GCONF_VALUE_SCHEMA);

  gconf_value_set_schema(gval, val);

  return error_checked_set(conf, key, gval, err);
}

gboolean
gconf_engine_set_list    (GConfEngine* conf, const gchar* key,
                          GConfValueType list_type,
                          GSList* list,
                          GError** err)
{
  GConfValue* value_list;
  GError *tmp_err = NULL;
  
  g_return_val_if_fail(conf != NULL, FALSE);
  g_return_val_if_fail(key != NULL, FALSE);
  g_return_val_if_fail(list_type != GCONF_VALUE_INVALID, FALSE);
  g_return_val_if_fail(list_type != GCONF_VALUE_LIST, FALSE);
  g_return_val_if_fail(list_type != GCONF_VALUE_PAIR, FALSE);
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);

  value_list = gconf_value_list_from_primitive_list(list_type, list, &tmp_err);

  if (tmp_err)
    {
      g_propagate_error (err, tmp_err);
      return FALSE;
    }
  
  /* destroys the value_list */
  
  return error_checked_set(conf, key, value_list, err);
}

gboolean
gconf_engine_set_pair    (GConfEngine* conf, const gchar* key,
                          GConfValueType car_type, GConfValueType cdr_type,
                          gconstpointer address_of_car,
                          gconstpointer address_of_cdr,
                          GError** err)
{
  GConfValue* pair;
  GError *tmp_err = NULL;
  
  g_return_val_if_fail(conf != NULL, FALSE);
  g_return_val_if_fail(key != NULL, FALSE);
  g_return_val_if_fail(car_type != GCONF_VALUE_INVALID, FALSE);
  g_return_val_if_fail(car_type != GCONF_VALUE_LIST, FALSE);
  g_return_val_if_fail(car_type != GCONF_VALUE_PAIR, FALSE);
  g_return_val_if_fail(cdr_type != GCONF_VALUE_INVALID, FALSE);
  g_return_val_if_fail(cdr_type != GCONF_VALUE_LIST, FALSE);
  g_return_val_if_fail(cdr_type != GCONF_VALUE_PAIR, FALSE);
  g_return_val_if_fail(address_of_car != NULL, FALSE);
  g_return_val_if_fail(address_of_cdr != NULL, FALSE);
  g_return_val_if_fail(err == NULL || *err == NULL, FALSE);
  

  pair = gconf_value_pair_from_primitive_pair(car_type, cdr_type,
                                              address_of_car, address_of_cdr,
                                              &tmp_err);

  if (tmp_err)
    {
      g_propagate_error (err, tmp_err);
      return FALSE;
    }  
  
  return error_checked_set(conf, key, pair, err);
}


/*
 * Enumeration conversions
 */

gboolean
gconf_string_to_enum (GConfEnumStringPair lookup_table[],
                      const gchar* str,
                      gint* enum_value_retloc)
{
  int i = 0;
  
  while (lookup_table[i].str != NULL)
    {
      if (g_ascii_strcasecmp (lookup_table[i].str, str) == 0)
        {
          *enum_value_retloc = lookup_table[i].enum_value;
          return TRUE;
        }

      ++i;
    }

  return FALSE;
}

const gchar*
gconf_enum_to_string (GConfEnumStringPair lookup_table[],
                      gint enum_value)
{
  int i = 0;
  
  while (lookup_table[i].str != NULL)
    {
      if (lookup_table[i].enum_value == enum_value)
        return lookup_table[i].str;

      ++i;
    }

  return NULL;
}

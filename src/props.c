/* msm SmProp utils */

/* 
 * Copyright (C) 2001 Havoc Pennington
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include "props.h"

#include <stdlib.h>
#include <string.h>

/* Property functions stolen from gnome-session */

GList*
proplist_find_link_by_name (GList      *list,
                            const char *name)
{
  for (; list; list = list->next)
    {
      SmProp *prop = (SmProp *) list->data;
      if (strcmp (prop->name, name) == 0)
	return list;
    }

  return NULL;
}


SmProp*
proplist_find_by_name (GList *list, const char *name)
{
  GList *ret;

  ret = proplist_find_link_by_name (list, name);

  return ret ? ret->data : NULL;
}

gboolean
proplist_find_card8 (GList *list, const char *name,
		     int *result)
{
  SmProp *prop;

  g_return_val_if_fail (result != NULL, FALSE);

  *result = 0;
  
  prop = proplist_find_by_name (list, name);
  if (prop == NULL)
    return FALSE;
  else
    return smprop_get_card8 (prop, result);
}

gboolean
proplist_find_string (GList *list, const char *name,
		      char **result)
{
  SmProp *prop;

  g_return_val_if_fail (result != NULL, FALSE);

  *result = NULL;
  
  prop = proplist_find_by_name (list, name);
  if (prop == NULL)
    return FALSE;
  else
    return smprop_get_string (prop, result);
}

GList*
proplist_replace (GList        *list,
                  SmProp       *new_prop)
{
  GList *link;
  
  link = proplist_find_link_by_name (list, new_prop->name);
  if (link)
    {
      SmFreeProperty (link->data);
      link->data = new_prop;
    }
  else
    {
      list = g_list_prepend (list, new_prop);
    }

  return list;
}

GList*
proplist_delete (GList        *list,
                 const char   *name)
{
  GList *link;
  
  link = proplist_find_link_by_name (list, name);
  if (link)
    {
      SmFreeProperty (link->data);
      list = g_list_delete_link (list, link);
    }

  return list;
}

GList*
proplist_replace_card8 (GList        *list,
                        const char   *name,
                        int           value)
{
  SmProp *prop;

  prop = smprop_new_card8 (name, value);

  return proplist_replace (list, prop);  
}

GList*
proplist_replace_string (GList        *list,
                         const char   *name,
                         const char   *str,
                         int           len)
{
  SmProp *prop;

  prop = smprop_new_string (name, str, len);

  return proplist_replace (list, prop);
}

GList*
proplist_replace_vector (GList        *list,
                         const char   *name,
                         int           argc,
                         char        **argv)
{
  SmProp *prop;

  prop = smprop_new_vector (name, argc, argv);

  return proplist_replace (list, prop);
}

gboolean
proplist_find_vector (GList *list, const char *name,
		      int *argcp, char ***argvp)
{
  SmProp *prop;

  g_return_val_if_fail (argcp != NULL, FALSE);
  g_return_val_if_fail (argvp != NULL, FALSE);

  *argcp = 0;
  *argvp = NULL;
  
  prop = proplist_find_by_name (list, name);
  if (prop == NULL)
    return FALSE;
  else
    return smprop_get_vector (prop, argcp, argvp);
}

void
proplist_free (GList *list)
{
  GList *tmp;
  
  tmp = list;
  while (tmp != NULL)
    {
      SmProp *prop = tmp->data;

      SmFreeProperty (prop);
      
      tmp = tmp->next;
    }

  g_list_free (list);
}

void
proplist_as_array (GList    *list,
                   SmProp ***props,
                   int      *n_props)
{
  GList *tmp;
  int i;

  g_return_if_fail (props != NULL);
  g_return_if_fail (n_props != NULL);
  
  *n_props = g_list_length (list);
  *props = g_new (SmProp*, *n_props);

  i = 0;
  tmp = list;
  while (tmp != NULL)
    {
      (*props)[i] = tmp->data;

      tmp = tmp->next;
      ++i;
    }
}

GList*
proplist_copy (GList *list)
{
  GList *copy;
  GList *tmp;

  copy = g_list_copy (list);
  tmp = copy;
  while (tmp != NULL)
    {
      tmp->data = smprop_copy (tmp->data);
      
      tmp = tmp->next;
    }

  return copy;
}

gboolean
smprop_get_card8  (SmProp   *prop,
                   int      *result)
{
  g_return_val_if_fail (result != NULL, FALSE);

  if (strcmp (prop->type, SmCARD8) == 0)
    {
      /* card8 is an unsigned int */
      unsigned char *p;
      p = prop->vals[0].value;
      *result = *p;
      return TRUE;
    }
  else
    return FALSE;
}

gboolean
smprop_get_string (SmProp   *prop,
                   char    **result)
{
  g_return_val_if_fail (result != NULL, FALSE);

  if (strcmp (prop->type, SmARRAY8) == 0)
    {
      *result = g_malloc (prop->vals[0].length + 1);
      memcpy (*result, prop->vals[0].value, prop->vals[0].length);
      (*result)[prop->vals[0].length] = '\0';
      return TRUE;
    }
  else
    return FALSE;
}

gboolean
smprop_get_vector (SmProp   *prop,
                   int      *argcp,
                   char   ***argvp)
{
  g_return_val_if_fail (argcp != NULL, FALSE);
  g_return_val_if_fail (argvp != NULL, FALSE);

  if (strcmp (prop->type, SmLISTofARRAY8) == 0)
    {
      int i;
      
      *argcp = prop->num_vals;
      *argvp = g_new0 (char *, *argcp + 1);
      for (i = 0; i < *argcp; ++i)
        {
          (*argvp)[i] = g_malloc (prop->vals[i].length + 1);
          memcpy ((*argvp)[i], prop->vals[i].value, prop->vals[i].length);
          (*argvp)[i][prop->vals[i].length] = '\0';
        }

      return TRUE;
    }
  else
    return FALSE;
}

SmProp*
smprop_copy (SmProp *prop)
{
  int i;
  SmProp *copy;

  /* This all uses malloc so we can use SmFreeProperty() */
  
  copy = msm_non_glib_malloc (sizeof (SmProp));

  if (prop->name)
    copy->name = msm_non_glib_strdup (prop->name);
  else
    copy->name = NULL;

  if (prop->type)
    copy->type = msm_non_glib_strdup (prop->type);
  else
    copy->type = NULL;

  copy->num_vals = prop->num_vals;
  copy->vals = NULL;

  if (copy->num_vals > 0 && prop->vals)
    {
      copy->vals = msm_non_glib_malloc (sizeof (SmPropValue) * copy->num_vals);
      
      for (i = 0; i < copy->num_vals; i++)
        {
          if (prop->vals[i].value)
            {
              copy->vals[i].length = prop->vals[i].length;
              copy->vals[i].value = msm_non_glib_malloc (copy->vals[i].length);
              memcpy (copy->vals[i].value, prop->vals[i].value,
                      copy->vals[i].length);
            }
          else
            {
              copy->vals[i].length = 0;
              copy->vals[i].value = NULL;
            }
        }
    }

  return copy;
}

SmProp*
smprop_new_vector (const char  *name,
                   int          argc,
                   char       **argv)
{
  SmProp *prop;
  int i;
  
  prop = msm_non_glib_malloc (sizeof (SmProp));
  prop->name = msm_non_glib_strdup (name);
  prop->type = msm_non_glib_strdup (SmLISTofARRAY8);

  prop->num_vals = argc;
  prop->vals = msm_non_glib_malloc (sizeof (SmPropValue) * prop->num_vals);
  i = 0;
  while (i < argc)
    {
      prop->vals[i].length = strlen (argv[i]);
      prop->vals[i].value = msm_non_glib_strdup (argv[i]);
      
      ++i;
    }

  return prop;
}

SmProp*
smprop_new_string (const char  *name,
                   const char  *str,
                   int          len)
{
  SmProp *prop;

  if (len < 0)
    len = strlen (str);
  
  prop = msm_non_glib_malloc (sizeof (SmProp));
  prop->name = msm_non_glib_strdup (name);
  prop->type = msm_non_glib_strdup (SmARRAY8);
  
  prop->num_vals = 1;
  prop->vals = msm_non_glib_malloc (sizeof (SmPropValue) * prop->num_vals);

  prop->vals[0].length = len;
  prop->vals[0].value = msm_non_glib_malloc (len);
  memcpy (prop->vals[0].value, str, len);

  return prop;
}

SmProp*
smprop_new_card8  (const char  *name,
                   int          value)
{
  SmProp *prop;
  
  prop = msm_non_glib_malloc (sizeof (SmProp));
  prop->name = msm_non_glib_strdup (name);
  prop->type = msm_non_glib_strdup (SmCARD8);

  prop->num_vals = 1;
  prop->vals = msm_non_glib_malloc (sizeof (SmPropValue) * prop->num_vals);

  prop->vals[0].length = 1;
  prop->vals[0].value = msm_non_glib_malloc (1);
  (* (unsigned char*)  prop->vals[0].value) = (unsigned char) value;

  return prop;
}

void
smprop_set_card8 (SmProp *prop,
                  int     value)
{
  g_return_if_fail (value >= 0);
  g_return_if_fail (value < 256);
  
  (* (unsigned char*)  prop->vals[0].value) = (unsigned char) value;
}

void
smprop_set_string (SmProp      *prop,
                   const char  *str,
                   int          len)
{
  g_return_if_fail (str != NULL);
  
  if (len < 0)
    len = strlen (str);

  prop->vals[0].length = len;
  free (prop->vals[0].value);
  prop->vals[0].value = msm_non_glib_malloc (len);
  memcpy (prop->vals[0].value, str, len);  
}

void
smprop_set_vector (SmProp      *prop,
                   int          argc,
                   char       **argv)
{
  int i;  

  i = 0;
  while (i < prop->num_vals)
    {
      if (prop->vals[i].value)
        free (prop->vals[i].value);
      
      ++i;
    }

  free (prop->vals);
  
  prop->num_vals = argc;
  prop->vals = msm_non_glib_malloc (sizeof (SmPropValue) * prop->num_vals);
  i = 0;
  while (i < argc)
    {
      prop->vals[i].length = strlen (argv[i]);
      prop->vals[i].value = msm_non_glib_strdup (argv[i]);
      
      ++i;
    }
}

void
smprop_append_to_vector   (SmProp      *prop,
                           const char  *str)
{
  prop->num_vals += 1;
  prop->vals = realloc (prop->vals, prop->num_vals * sizeof (SmPropValue));
  if (prop->vals == NULL)
    g_error ("Failed to realloc vector in SmProp");

  prop->vals[prop->num_vals - 1].length = strlen (str);
  prop->vals[prop->num_vals - 1].value = msm_non_glib_strdup (str);
}

void
smprop_set_vector_element (SmProp      *prop,
                           int          i,
                           const char  *str)
{
  g_return_if_fail (i < prop->num_vals);

  if (prop->vals[i].value)
    free (prop->vals[i].value);

  prop->vals[i].length = strlen (str);
  prop->vals[i].value = msm_non_glib_strdup (str);
}

int
smprop_get_vector_length (SmProp *prop)
{
  return prop->num_vals;
}

/* These are randomly in here due to the convenience library */
char*
msm_non_glib_strdup (const char *str)
{
  char *new_str;

  if (str)
    {
      new_str = msm_non_glib_malloc (strlen (str) + 1);
      strcpy (new_str, str);
    }
  else
    new_str = NULL;

  return new_str;
}

void*
msm_non_glib_malloc (int bytes)
{
  void *ptr;

  if (bytes == 0)
    return NULL;
  
  ptr = malloc (bytes);
  if (ptr == NULL)
    g_error ("Failed to allocate %d bytes\n", bytes);

  return ptr;
}

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

#ifndef MSM_PROPS_H
#define MSM_PROPS_H

#include <glib.h>
#include <X11/ICE/ICElib.h>
#include <X11/SM/SMlib.h>

GList*   proplist_find_link_by_name (GList        *list,
                                     const char   *name);
SmProp*  proplist_find_by_name      (GList        *list,
                                     const char   *name);
gboolean proplist_find_card8        (GList        *list,
                                     const char   *name,
                                     int          *result);
gboolean proplist_find_string       (GList        *list,
                                     const char   *name,
                                     char        **result);
gboolean proplist_find_vector       (GList        *list,
                                     const char   *name,
                                     int          *argcp,
                                     char       ***argvp);

GList*   proplist_replace           (GList        *list,
                                     SmProp       *new_prop);

GList*   proplist_delete           (GList        *list,
                                    const char   *name);

GList*   proplist_replace_card8     (GList        *list,
                                     const char   *name,
                                     int           value);
GList*   proplist_replace_string     (GList        *list,
                                      const char   *name,
                                      const char   *str,
                                      int           len);
GList*   proplist_replace_vector     (GList        *list,
                                      const char   *name,
                                      int           argc,
                                      char        **argv);

void     proplist_free               (GList        *list);

void     proplist_as_array           (GList    *list,
                                      SmProp ***props,
                                      int      *n_props);
GList*   proplist_copy               (GList        *list);

gboolean smprop_get_card8  (SmProp   *prop,
                            int      *result);
gboolean smprop_get_string (SmProp   *prop,
                            char    **result);
gboolean smprop_get_vector (SmProp   *prop,
                            int      *argcp,
                            char   ***argvp);

SmProp* smprop_new_card8  (const char  *name,
                           int          value);
SmProp* smprop_new_string (const char  *name,
                           const char  *str,
                           int          len);
SmProp* smprop_new_vector (const char  *name,
                           int          argc,
                           char       **argv);

SmProp* smprop_copy (SmProp *prop);

void smprop_set_card8          (SmProp      *prop,
                                int          value);
void smprop_set_string         (SmProp      *prop,
                                const char  *str,
                                int          len);
void smprop_set_vector         (SmProp      *prop,
                                int          argc,
                                char       **argv);
void smprop_append_to_vector   (SmProp      *prop,
                                const char  *value);
void smprop_set_vector_element (SmProp      *prop,
                                int          i,
                                const char  *value);

int  smprop_get_vector_length  (SmProp      *prop);


char* msm_non_glib_strdup (const char *src);
void* msm_non_glib_malloc (int bytes);


#endif


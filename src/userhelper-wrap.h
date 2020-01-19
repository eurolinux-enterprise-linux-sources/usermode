/*
 * Copyright (C) 1997-2001, 2007 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef __USERHELPER_WRAP_H__
#define __USERHELPER_WRAP_H__

#include "config.h"
#include <glib.h>

void userhelper_run(gboolean notify_success, const char *path, ...);
int userhelper_runv(gboolean notify_success, const char *path, char **args);
void userhelper_main_quit(void);

#endif /* __USERHELPER_WRAP_H__ */

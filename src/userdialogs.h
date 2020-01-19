/*
 * Copyright (C) 1997, 2001, 2007 Red Hat, Inc.
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

#ifndef __USERDIALOGS_H__
#define __USERDIALOGS_H__

#include "config.h"
#include <gtk/gtk.h>

GtkWidget* create_message_box(const gchar* message, const gchar* title);
GtkWidget* create_error_box(const gchar* error, const gchar* title);

#endif /* __USERDIALOGS_H__ */

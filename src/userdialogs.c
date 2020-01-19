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
 *
 */

#include "config.h"
#include <locale.h>
#include <libintl.h>
#include <gtk/gtk.h>
#include "userdialogs.h"

#define DIALOG_XML_NAME "userdialog-xml"

GtkWidget *
create_message_box(const gchar *message, const gchar *title)
{
	GtkWidget *dialog;
	dialog =  gtk_message_dialog_new(NULL, 0,
					 GTK_MESSAGE_INFO,
					 GTK_BUTTONS_CLOSE,
					 "%s", message);
	if (title) {
		gtk_window_set_title(GTK_WINDOW(dialog), title);
	}
	gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ALWAYS);
	return dialog;
}

GtkWidget *
create_error_box(const gchar *error, const gchar *title)
{
	GtkWidget *dialog;
	dialog =  gtk_message_dialog_new(NULL, 0,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_CLOSE,
					 "%s", error);
	if (title) {
		gtk_window_set_title(GTK_WINDOW(dialog), title);
	}
	gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ALWAYS);
	return dialog;
}

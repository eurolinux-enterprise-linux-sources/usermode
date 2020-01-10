/*
 * Copyright (C) 1999, 2000, 2001, 2002, 2003, 2007 Red Hat, Inc.  All rights
 * reserved.
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

#include "config.h"
#include <libintl.h>
#include <locale.h>
#include <stdio.h>
#include <gtk/gtk.h>
#include "userdialogs.h"

static void
hello_world(GtkWidget *ignored, gpointer data)
{
	(void)ignored;
	(void)data;
	printf("Hello world, %s.\n", (char*) data);
}

int
main(int argc, char *argv[])
{
	GtkWidget *msg;

	bindtextdomain(PACKAGE, LOCALEDIR);
	bind_textdomain_codeset(PACKAGE, "UTF-8");
	textdomain(PACKAGE);

	gtk_init(&argc, &argv);

	msg = create_message_box("Hello world!\n"
				 "Let's make this a really big message box.",
				 "Hello");

	g_signal_connect(G_OBJECT(msg), "destroy",
			 G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(G_OBJECT(msg), "destroy",
			 G_CALLBACK(hello_world), (gpointer)"otto");

	msg = create_error_box("ERROR!\n"
			       "Let's make this a really big message box.",
			       NULL);
	gtk_dialog_run(GTK_DIALOG(msg));
	gtk_widget_destroy(msg);

	return 0;
}

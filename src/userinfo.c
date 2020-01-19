/*
 * Copyright (C) 1997 Red Hat Software, Inc.
 * Copyright (C) 2001, 2007, 2008, 2009 Red Hat, Inc.
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

/* Things to remember...
 * -- when setting values, make sure there are no colons in what users
 * want me to set.  This is just for convenient dialog boxes to tell
 * people to remove their colons.  Presumably, the suid root helper to
 * actually set values won't accept anything that has colons.  Ditto
 * for commas, as well... but the suid helper doesn't need to deal
 * with that.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libintl.h>
#include <unistd.h>
#include <pwd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include "userhelper.h"
#include "userhelper-wrap.h"

#ifdef DEBUG_USERHELPER
#define debug_msg(...) fprintf(stderr, __VA_ARGS__)
#else
#define debug_msg(...) ((void)0)
#endif

#define USERINFO_XML_NAME "userinfo-xml"
struct UserInfo {
	const char *full_name;
	const char *office;
	const char *office_phone;
	const char *home_phone;
	char *shell;
};

static void set_new_userinfo(struct UserInfo *userinfo);
static gint on_ok_clicked(GtkWidget *widget, gpointer data);

static void
shell_changed(GtkWidget *widget, gpointer data)
{
	struct UserInfo *userinfo;
	GtkTreeModel *model;
	GtkTreeIter iter;
	gchar *text;

	userinfo = data;
	if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(widget), &iter)
	    == FALSE)
		return;
	model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
	gtk_tree_model_get(model, &iter, 0, &text, -1);
	g_free(userinfo->shell);
	userinfo->shell = text;
}

static GtkWidget *
create_userinfo_window(struct UserInfo *userinfo)
{
	GtkBuilder *builder;
	GtkWidget *window = NULL;
	GError *error;
	char *shell;

	builder = gtk_builder_new();
	error = NULL;
	if (gtk_builder_add_from_file(builder, PKGDATADIR "/usermode.ui",
				      &error) == 0) {
		g_warning("Error loading usermode.ui: %s", error->message);
		g_error_free(error);
	} else {
		GtkWidget *entry, *shell_menu, *widget;
		GtkListStore *shells;
		GtkCellRenderer *cell;
		gboolean saw_shell = FALSE;

		window = GTK_WIDGET(gtk_builder_get_object(builder,
							   "userinfo"));
		g_assert(window != NULL);

		gtk_window_set_icon_from_file(GTK_WINDOW(window),
					      PIXMAPDIR "/user_icon.png", NULL);

		g_object_set_data(G_OBJECT(window),
				  USERINFO_XML_NAME, builder);
		g_signal_connect(window, "destroy",
				 G_CALLBACK(userhelper_main_quit), window);

		entry = GTK_WIDGET(gtk_builder_get_object(builder, "fullname"));
		g_assert(entry != NULL);
		if (GTK_IS_ENTRY(entry) && userinfo->full_name) {
			gtk_entry_set_text(GTK_ENTRY(entry),
					   userinfo->full_name);
		}

		entry = GTK_WIDGET(gtk_builder_get_object(builder, "office"));
		g_assert(entry != NULL);
		if (GTK_IS_ENTRY(entry) && userinfo->office) {
			gtk_entry_set_text(GTK_ENTRY(entry),
					   userinfo->office);
		}

		entry = GTK_WIDGET(gtk_builder_get_object(builder,
							  "officephone"));
		g_assert(entry != NULL);
		if (GTK_IS_ENTRY(entry) && userinfo->office_phone) {
			gtk_entry_set_text(GTK_ENTRY(entry),
					   userinfo->office_phone);
		}

		entry = GTK_WIDGET(gtk_builder_get_object(builder,
							  "homephone"));
		g_assert(entry != NULL);
		if (GTK_IS_ENTRY(entry) && userinfo->home_phone) {
			gtk_entry_set_text(GTK_ENTRY(entry),
					   userinfo->home_phone);
		}

		shells = gtk_list_store_new(1, G_TYPE_STRING);

		setusershell();
		while ((shell = getusershell()) != NULL) {
			GtkTreeIter iter;

			/* Filter out "nologin" to keep the user from shooting
			 * self in foot, or similar analogy. */
			if (strstr(shell, "/nologin") != NULL)
				continue;
			if (strcmp(shell, userinfo->shell) == 0) {
				gtk_list_store_insert(shells, &iter, 0);
				saw_shell = TRUE;
			} else
				gtk_list_store_append(shells, &iter);
			gtk_list_store_set(shells, &iter, 0, shell, -1);
		}
		endusershell();
		if (!saw_shell) {
			GtkTreeIter iter;

			gtk_list_store_insert(shells, &iter, 0);
			gtk_list_store_set(shells, &iter, 0, userinfo->shell,
					   -1);
		}

		shell_menu = GTK_WIDGET(gtk_builder_get_object(builder,
							       "shellmenu"));
		g_assert(shell_menu != NULL);
		gtk_combo_box_set_model(GTK_COMBO_BOX(shell_menu),
					GTK_TREE_MODEL(shells));
		g_object_unref(shells);
		cell = gtk_cell_renderer_text_new();
		gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(shell_menu), cell,
					   TRUE);
		gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(shell_menu),
					       cell, "text", 0, NULL);
		gtk_combo_box_set_active(GTK_COMBO_BOX(shell_menu), 0);
		g_signal_connect(shell_menu, "changed",
				 G_CALLBACK(shell_changed), userinfo);

		widget = GTK_WIDGET(gtk_builder_get_object(builder, "apply"));
		g_assert(widget != NULL);
		g_signal_connect(widget, "clicked", G_CALLBACK(on_ok_clicked),
				 userinfo);
		widget = GTK_WIDGET(gtk_builder_get_object(builder, "close"));
		g_assert(widget != NULL);
		g_signal_connect(widget, "clicked",
				 G_CALLBACK(userhelper_main_quit), window);
	}

	return window;
}

static struct UserInfo *
parse_userinfo(void)
{
	struct UserInfo *retval;
	struct passwd *pwent;

	char **vals;

	pwent = getpwuid(getuid());
	if (pwent == NULL) {
		return NULL;
	}
	retval = g_malloc0(sizeof(struct UserInfo));

	retval->shell = g_strdup(pwent->pw_shell);
	vals = g_strsplit(pwent->pw_gecos ?: "", ",", 5);
	if (vals != NULL) {
		if (vals[0]) {
			retval->full_name = g_strdup(vals[0]);
		}
		if (vals[0] && vals[1]) {
			retval->office = g_strdup(vals[1]);
		}
		if (vals[0] && vals[1] && vals[2]) {
			retval->office_phone = g_strdup(vals[2]);
		}
		if (vals[0] && vals[1] && vals[2] && vals[3]) {
			retval->home_phone = g_strdup(vals[3]);
		}
		g_strfreev(vals);
	}

	return retval;
}

static gint
on_ok_clicked(GtkWidget *widget, gpointer data)
{
	struct UserInfo *userinfo;
	GtkWidget *toplevel, *entry;
	GtkBuilder *builder;

	toplevel = gtk_widget_get_toplevel(widget);
	if (!gtk_widget_is_toplevel(toplevel)) {
		return FALSE;
	}
	userinfo = data;
	builder = g_object_get_data(G_OBJECT(toplevel), USERINFO_XML_NAME);

	entry = GTK_WIDGET(gtk_builder_get_object(builder, "fullname"));
	if (GTK_IS_ENTRY(entry)) {
		userinfo->full_name = gtk_entry_get_text(GTK_ENTRY(entry));
	}

	entry = GTK_WIDGET(gtk_builder_get_object(builder, "office"));
	if (GTK_IS_ENTRY(entry)) {
		userinfo->office = gtk_entry_get_text(GTK_ENTRY(entry));
	}

	entry = GTK_WIDGET(gtk_builder_get_object(builder, "officephone"));
	if (GTK_IS_ENTRY(entry)) {
		userinfo->office_phone = gtk_entry_get_text(GTK_ENTRY(entry));
	}

	entry = GTK_WIDGET(gtk_builder_get_object(builder, "homephone"));
	if (GTK_IS_ENTRY(entry)) {
		userinfo->home_phone = gtk_entry_get_text(GTK_ENTRY(entry));
	}

	if (GTK_IS_WIDGET(toplevel)) {
		gtk_widget_set_sensitive(GTK_WIDGET(toplevel), FALSE);
	}
	set_new_userinfo(userinfo);
	if (GTK_IS_WIDGET(toplevel)) {
		gtk_widget_set_sensitive(GTK_WIDGET(toplevel), TRUE);
	}
	return FALSE;
}

static void
set_new_userinfo(struct UserInfo *userinfo)
{
	const char *fullname;
	const char *office;
	const char *officephone;
	const char *homephone;
	const char *shell;
	char *argv[12];
	int i = 0;

	fullname = userinfo->full_name;
	office = userinfo->office;
	officephone = userinfo->office_phone;
	homephone = userinfo->home_phone;
	shell = userinfo->shell;

	argv[i++] = (char *)UH_PATH;

	if (fullname) {
		argv[i++] = (char *)UH_FULLNAME_OPT;
		argv[i++] = (char *)fullname;
	}

	if (office) {
		argv[i++] = (char *)UH_OFFICE_OPT;
		argv[i++] = (char *)office;
	}

	if (officephone) {
		argv[i++] = (char *)UH_OFFICEPHONE_OPT;
		argv[i++] = (char *)officephone;
	}

	if (homephone) {
		argv[i++] = (char *)UH_HOMEPHONE_OPT;
		argv[i++] = (char *)homephone;
	}

	if (shell) {
		argv[i++] = (char *)UH_SHELL_OPT;
		argv[i++] = (char *)shell;
	}

	argv[i++] = NULL;

	userhelper_runv(TRUE, UH_PATH, argv);
}

static int
safe_strcmp (const char *s1, const char *s2)
{
        return strcmp (s1 ? s1 : "", s2 ? s2 : "");
}

static void
parse_args (struct UserInfo *userinfo, int argc, char *argv[])
{
	int changed;
	int x_flag;
	int arg;

        changed = 0;
        x_flag = 0;

   	while ((arg = getopt(argc, argv, "f:o:p:h:s:x")) != -1) {
                switch (arg) {
                        case 'f':
                                /* Full name. */
				if (safe_strcmp (userinfo->full_name, optarg) != 0) {
	                                changed = 1;
                                	userinfo->full_name = optarg;
				}
                                break;
                        case 'o':
                                /* Office. */
				if (safe_strcmp (userinfo->office, optarg) != 0) {
	                                changed = 1;
                                	userinfo->office = optarg;
				}
                                break;
                        case 'h':
                                /* Home phone. */
				if (safe_strcmp (userinfo->home_phone, optarg) != 0) {
	                                changed = 1;
                                	userinfo->home_phone = optarg;
				}
                                break;
                        case 'p':
                                /* Office phone. */
				if (safe_strcmp (userinfo->office_phone, optarg) != 0) {
	                                changed = 1;
                                	userinfo->office_phone = optarg;
				}
                                break;
                        case 's':
                                /* Shell. */
				if (safe_strcmp (userinfo->shell, optarg) != 0) {
	                                changed = 1;
                                	userinfo->shell = optarg;
				}
                                break;
                        case 'x':
				x_flag = 1;
				break;
			default:
				exit(1);
		}
	}
	if (optind != argc) {
		fprintf(stderr, _("Unexpected command-line arguments\n"));
		exit(1);
	}

	if (x_flag) {
		if (changed)
			set_new_userinfo(userinfo);

		exit(0);
	}
}

int
main(int argc, char *argv[])
{
	struct UserInfo *userinfo;
	GtkWidget *window;

	bindtextdomain(PACKAGE, LOCALEDIR);
	bind_textdomain_codeset(PACKAGE, "UTF-8");
	textdomain(PACKAGE);

	userinfo = parse_userinfo();
	if (userinfo == NULL) {
		fprintf(stderr, _("You don't exist.  Go away.\n"));
		exit(1);
	}

	gtk_init(&argc, &argv);

	parse_args (userinfo, argc, argv);

	window = create_userinfo_window(userinfo);
	gtk_widget_show_all(window);

	debug_msg("Running.\n");

	gtk_main();

	debug_msg("Exiting.\n");

	return 0;
}

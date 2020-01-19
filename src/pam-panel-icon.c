/*
 * Copyright (C) 2002, 2003, 2007 Red Hat, Inc.  All rights reserved.
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
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <libintl.h>
#include <locale.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include "gsmclient.h"

#define PAM_TIMESTAMP_CHECK_PATH "/sbin/pam_timestamp_check"

enum {
	RESPONSE_DROP = 0,
	RESPONSE_DO_NOTHING = 1
};

enum {
	STATUS_UNKNOWN = -1,
	STATUS_AUTHENTICATED = 0,
	STATUS_BINARY_NOT_SUID = 2,
	STATUS_NO_TTY = 3,
	STATUS_USER_UNKNOWN = 4,
	STATUS_PERMISSIONS_ERROR = 5,
	STATUS_INVALID_TTY = 6,
	STATUS_OTHER_ERROR = 7
};

static int current_status = STATUS_UNKNOWN;
static GIOChannel *child_io_channel = NULL;
static guint child_io_source; /* = 0; */
static guint child_watch_source; /* = 0; */
static void *tray_icon; /* = NULL; actually a GtkStatusIcon * */
static void *drop_dialog; /* = NULL; actually a GtkWidget * */
static GtkWidget *drop_menu = NULL;

static void launch_checker(void);

 /* Tray icon and authorization dropping */

/* Respond to an user decision about dropping the authorization */
static void
handle_drop_response(int response_id)
{
	if (response_id == RESPONSE_DROP) {
		static char *command[] = {
			(char *)PAM_TIMESTAMP_CHECK_PATH, (char *)"-k",
			(char *)"root", NULL
		};

		GError *err;
		GtkWidget *dialog;
		int exit_status;

		exit_status = 0;
		err = NULL;
		dialog = NULL;
		if (!g_spawn_sync("/", command, NULL,
				  G_SPAWN_CHILD_INHERITS_STDIN, NULL, NULL,
				  NULL, NULL, &exit_status, &err)) {
			/* There was an error running the command. */
			dialog = gtk_message_dialog_new(NULL,
							GTK_DIALOG_DESTROY_WITH_PARENT,
							GTK_MESSAGE_ERROR,
							GTK_BUTTONS_CLOSE,
							_("Failed to drop administrator privileges: %s"),
							err->message);
			g_error_free(err);
		} else if (WIFEXITED(exit_status)
			   && WEXITSTATUS(exit_status) != 0) {
			dialog = gtk_message_dialog_new(NULL,
							GTK_DIALOG_DESTROY_WITH_PARENT,
							GTK_MESSAGE_ERROR,
							GTK_BUTTONS_CLOSE,
							_("Failed to drop administrator privileges: "
							  "pam_timestamp_check returned failure code %d"),
							WEXITSTATUS(exit_status));
		}
		if (dialog != NULL) {
			g_signal_connect(G_OBJECT(dialog), "response",
					 G_CALLBACK(gtk_widget_destroy),
					 NULL);
			gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
			gtk_window_present(GTK_WINDOW(dialog));
		}
	}
}

/* Respond to a selectdion in the popup menu. */
static void
drop_menu_response_cb(GtkMenuItem *item, gpointer data)
{
	(void)item;
	handle_drop_response((int)(intptr_t)data);
}

/* Respond to a button press in the drop dialog. */
static void
drop_dialog_response_cb(GtkWidget *dialog, gint response_id, gpointer data)
{
	(void)data;
	gtk_widget_destroy(dialog);

	handle_drop_response(response_id);
}

static void
handle_activate(GtkStatusIcon *icon, gpointer data)
{
	(void)icon;
	(void)data;
	/* If we're already authenticated, give the user the option of removing
	   the timestamp. */
	if (current_status != STATUS_AUTHENTICATED)
		return;

	/* If there's not already a dialog up, create one. */
	if (drop_dialog == NULL) {
		drop_dialog = gtk_message_dialog_new(NULL,
						     GTK_DIALOG_DESTROY_WITH_PARENT,
						     GTK_MESSAGE_QUESTION,
						     GTK_BUTTONS_NONE,
						     _("You're currently authorized to configure system-wide settings (that affect all users) without typing the administrator password again. You can give up this authorization."));

		g_object_add_weak_pointer(G_OBJECT(drop_dialog), &drop_dialog);

		gtk_dialog_add_button(GTK_DIALOG(drop_dialog),
				      _("Keep Authorization"),
				      RESPONSE_DO_NOTHING);
		gtk_dialog_add_button(GTK_DIALOG(drop_dialog),
				      _("Forget Authorization"), RESPONSE_DROP);

		g_signal_connect(G_OBJECT(drop_dialog), "response",
				 G_CALLBACK(drop_dialog_response_cb), NULL);

		gtk_window_set_resizable(GTK_WINDOW(drop_dialog), FALSE);
	}
	gtk_window_present(GTK_WINDOW(drop_dialog));
}

static void
handle_popup(GtkStatusIcon *icon, guint button, guint activate_time,
	     gpointer data)
{
	(void)data;
	/* If we're already authenticated, give the user the option of removing
	   the timestamp. */
	if (current_status != STATUS_AUTHENTICATED)
		return;
	if (drop_menu == NULL) {
		GtkWidget *item;

		drop_menu = gtk_menu_new();

		item = gtk_menu_item_new_with_label(_("Keep Authorization"));
		gtk_menu_shell_append(GTK_MENU_SHELL(drop_menu), item);
		g_signal_connect(G_OBJECT(item), "activate",
				 G_CALLBACK(drop_menu_response_cb),
				 (gpointer)RESPONSE_DO_NOTHING);
		gtk_widget_show(item);

		item = gtk_menu_item_new_with_label(_("Forget Authorization"));
		gtk_menu_shell_append(GTK_MENU_SHELL(drop_menu), item);
		g_signal_connect(G_OBJECT(item), "activate",
				 G_CALLBACK(drop_menu_response_cb),
				 (gpointer)RESPONSE_DROP);
		gtk_widget_show(item);
	}
	gtk_menu_popup(GTK_MENU(drop_menu), NULL, NULL,
		       gtk_status_icon_position_menu, icon, button,
		       activate_time);
}

static void
refresh_tray_icon(void)
{
	gboolean visible;

	if (current_status == STATUS_AUTHENTICATED
	    && (getuid() != 0 || geteuid() != 0 || getgid() != 0
		|| getegid() != 0))
		visible = TRUE;
	else
		visible = FALSE;
	if (visible && tray_icon == NULL) {
		tray_icon = gtk_status_icon_new_from_file(PIXMAPDIR
							  "/badge-small.png");

		/* If the system tray goes away, our icon will get destroyed,
		 * and we don't want to be left with a dangling pointer to it
		 * if that happens.  */
		g_object_add_weak_pointer(G_OBJECT(tray_icon), &tray_icon);

		g_signal_connect(G_OBJECT(tray_icon), "activate",
				 G_CALLBACK(handle_activate), NULL);
		g_signal_connect(G_OBJECT(tray_icon), "popup-menu",
				 G_CALLBACK(handle_popup), NULL);
	}
	if (tray_icon != NULL)
		gtk_status_icon_set_visible(GTK_STATUS_ICON(tray_icon), visible);
}

 /* Interaction with pam_timestamp_check */

static gboolean
child_io_func(GIOChannel *source, GIOCondition condition, void *data)
{
	gboolean respawn_child;
	int output;
	const char *message;
	int old_status;

	(void)data;
	output = 0;
	respawn_child = FALSE;
	old_status = current_status;

	if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
		/* Error conditions mean we need to launch a new child. */
		respawn_child = TRUE;
		current_status = STATUS_UNKNOWN;
	} else
	if (condition & G_IO_IN) {
		char buf[10];
		GError *err;
		gsize i;
		gsize bytes_read;

		err = NULL;
		g_io_channel_read_chars(source, buf, sizeof(buf), &bytes_read,
					&err);

		if (err != NULL) {
			g_printerr("Error reading from pam_timestamp_check: "
				   "%s\n", err->message);
			g_error_free(err);

			respawn_child = TRUE;
			current_status = STATUS_UNKNOWN;
		}

		for (i = 0; i < bytes_read; i++) {
			if (g_ascii_isdigit(buf[i]))
				output = atoi(buf + i);
			else if (buf[i] != '\n')
				g_printerr("Unknown byte '%d' from "
					   "pam_timestamp_check.\n",
					   (int)buf[i]);
		}
	}

	message = NULL;
	switch (output) {
	case 0:
		current_status = STATUS_AUTHENTICATED;
		break;
	case 1:
		current_status = STATUS_UNKNOWN;
		message = "bad args to pam_timestamp_check";
		break;
	case 2:
		message = _("pam_timestamp_check is not setuid root");
		current_status = STATUS_BINARY_NOT_SUID;
		break;
	case 3:
		message = _("no controlling tty for pam_timestamp_check");
		current_status = STATUS_NO_TTY;
		break;
	case 4:
		message = _("user unknown to pam_timestamp_check");
		current_status = STATUS_USER_UNKNOWN;
		break;
	case 5:
		message = _("permissions error in pam_timestamp_check");
		current_status = STATUS_PERMISSIONS_ERROR;
		break;
	case 6:
		message = _("invalid controlling tty in pam_timestamp_check");
		current_status = STATUS_INVALID_TTY;
		break;

	case 7:
		/* timestamp just isn't held - user hasn't authenticated */
		current_status = STATUS_OTHER_ERROR;
		break;

	default:
		message = "got unknown code from pam_timestamp_check";
		current_status = STATUS_UNKNOWN;
		break;
	}

	if (message) {
		/*  FIXME, dialog? */
		if (old_status != current_status)
			g_printerr(_("Error: %s\n"), message);
	}

	refresh_tray_icon();

	if (respawn_child) {
		/* Respawn the child */
		launch_checker();
		return FALSE;
	} else
		return TRUE;
}

static void
child_watch_func(GPid pid, gint status, gpointer data)
{
	(void)pid;
	(void)data;
	if (WIFSIGNALED(status))
		g_printerr("pam_timestamp_check died on signal %d\n",
			   WTERMSIG(status));

	current_status = STATUS_UNKNOWN;
	refresh_tray_icon();

	/* Respawn the child */
	launch_checker();
}

/* Launch the child which checks for timestamps. */
static void
launch_checker(void)
{
	static char *command[] = {
		(char *)PAM_TIMESTAMP_CHECK_PATH, (char *)"-d", (char *)"root",
		NULL
	};

	GPid pid;
	GError *err;
	int out_fd;

	/* If we are launching a new checker, destroy all state associated
	   with the old one. */
	if (child_io_source != 0) {
		g_source_remove(child_io_source);
		child_io_source = 0;
	}
	if (child_watch_source != 0) {
		g_source_remove(child_watch_source);
		child_watch_source = 0;
	}
	if (child_io_channel != NULL) {
		g_io_channel_unref(child_io_channel);
		child_io_channel = NULL;
	}

	/* Let the child inherit stdin so that pam_timestamp_check can get at
	 * the panel's controlling tty, if there is one. */
	out_fd = -1;
	err = NULL;
	if (!g_spawn_async_with_pipes("/", command, NULL,
				      G_SPAWN_CHILD_INHERITS_STDIN
				      | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
				      &pid, NULL, &out_fd, NULL, &err)) {
		g_printerr(_("Failed to run command \"%s\": %s\n"),
			   command[0], err->message);
		g_error_free(err);
		return;
	}
	/* We're watching for output from the child. */
	child_io_channel = g_io_channel_unix_new(out_fd);

	/* Try to set the channel non-blocking. */
	err = NULL;
	g_io_channel_set_flags(child_io_channel, G_IO_FLAG_NONBLOCK, &err);
	if (err != NULL) {
		g_printerr(_("Failed to set IO channel nonblocking: %s\n"),
			   err->message);
		g_error_free(err);

		g_io_channel_unref(child_io_channel);
		child_io_channel = NULL;
		return;
	}

	/* Set up a callback for when the child tells us something. */
	child_watch_source = g_child_watch_add(pid, child_watch_func, NULL);
	child_io_source = g_io_add_watch(child_io_channel,
					 G_IO_IN | G_IO_HUP | G_IO_ERR
					 | G_IO_NVAL, child_io_func, NULL);
}

 /* Main program */

/* Save ourselves. */
static void
session_save_callback(GsmClient *client, gboolean is_phase2, gpointer data)
{
	const char *argv[4];

	(void)is_phase2;
	(void)data;
	argv[0] = BINDIR "/pam-panel-icon";
	argv[1] = "--sm-client-id";
	argv[2] = gsm_client_get_id(client);
	argv[3] = NULL;

	/* The restart command will restart us with this same session ID. */
	gsm_client_set_restart_command(client, G_N_ELEMENTS(argv) - 1,
				       (char**) argv);

	/* The clone command will start up another copy. */
	gsm_client_set_clone_command(client, 1, (char**) argv);
}

/* Handle the "session closing" notification. */
static void
session_die_callback(GsmClient *client, gpointer data)
{
	(void)client;
	(void)data;
	gtk_main_quit();
}

int
main(int argc, char **argv)
{
	GsmClient *client = NULL;
	const char *previous_id = NULL;

	gtk_init(&argc, &argv);

	/* Check if a session management ID was passed on the command line. */
	if (argc > 1) {
		if ((argc != 3) || (strcmp(argv[1], "--sm-client-id") != 0)) {
			g_printerr("pam-panel-icon: invalid args\n");
			return 1;
		}
		previous_id = argv[2];
	}

	/* Start up locales */
        setlocale(LC_ALL, "");
        bindtextdomain(PACKAGE, LOCALEDIR);
        bind_textdomain_codeset(PACKAGE, "UTF-8");
        textdomain(PACKAGE);

	client = gsm_client_new();

	gsm_client_set_restart_style(client, GSM_RESTART_IMMEDIATELY);
	/* start up last */
	gsm_client_set_priority(client, GSM_CLIENT_PRIORITY_NORMAL + 10);

	gsm_client_connect(client, previous_id);

	if (!gsm_client_get_connected(client)) {
		g_printerr(_("pam-panel-icon: failed to connect to session manager\n"));
	}

	g_signal_connect(G_OBJECT(client), "save",
			 G_CALLBACK(session_save_callback), NULL);

	g_signal_connect(G_OBJECT(client), "die",
			 G_CALLBACK(session_die_callback), NULL);

	launch_checker();

	gtk_main();

	return 0;
}

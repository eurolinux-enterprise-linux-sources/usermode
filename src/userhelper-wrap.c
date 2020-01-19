/*
 * Copyright (C) 1997, 2001-2003, 2007, 2008 Red Hat, Inc.
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
#include <assert.h>
#include <sys/types.h>
#include <ctype.h>
#include <errno.h>
#include <libintl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#ifdef USE_STARTUP_NOTIFICATION
#include <libsn/sn.h>
#endif
#include "userdialogs.h"
#include "userhelper.h"
#include "userhelper-messages.h"
#include "userhelper-wrap.h"

#ifdef DEBUG_USERHELPER
#define debug_msg(...) fprintf(stderr, __VA_ARGS__)
#else
#define debug_msg(...) ((void)0)
#endif

#define  PAD 8
#define  RESPONSE_FALLBACK 100
static int childin[2];
static int childpid;
static int childout_tag;
static int child_exit_status;
static gboolean child_success_dialog = TRUE;
static gboolean child_was_execed = FALSE;

struct message {
	int type;
	GtkWidget *entry;
	GtkWidget *label;
};

struct response {
	int responses, rows;
	gboolean fallback_allowed;
	char *service, *suggestion, *banner, *title;
	GList *message_list; /* contains pointers to messages */
	void *dialog; /* Actually a GtkWidget * */
	GtkWidget *user_label, *first, *last, *table;
	gulong dialog_response_handler;
};

static char *sn_id = NULL;
static char *sn_name = NULL;
static char *sn_description = NULL;
static int sn_workspace = -1;
static char *sn_wmclass = NULL;
static char *sn_binary_name = NULL;
static char *sn_icon_name = NULL;

#ifdef USE_STARTUP_NOTIFICATION
/* Push errors for the specified display. */
static void
trap_push(SnDisplay *display, Display *xdisplay)
{
	(void)xdisplay;
	sn_display_error_trap_push(display);
	gdk_error_trap_push();
}

/* Pop errors for the specified display. */
static void
trap_pop(SnDisplay *display, Display *xdisplay)
{
	(void)xdisplay;
	gdk_error_trap_pop();
	sn_display_error_trap_pop(display);
}
#endif

/* Complete startup notification for consolehelper. */
static void
userhelper_startup_notification_launchee(const char *id)
{
	(void)id;
#ifdef USE_STARTUP_NOTIFICATION
	GdkDisplay *gdisp;
	GdkScreen *gscreen;
	SnDisplay *disp;
	SnLauncheeContext *ctx;
	int screen;

	gdisp = gdk_display_get_default();
	gscreen = gdk_display_get_default_screen(gdisp);
	disp = sn_display_new(GDK_DISPLAY(), trap_push, trap_pop);
	screen = gdk_screen_get_number(gscreen);
	if (id == NULL) {
		ctx = sn_launchee_context_new_from_environment(disp, screen);
	} else {
		ctx = sn_launchee_context_new(disp, screen, id);
	}
	if (ctx != NULL) {
		debug_msg("Completing startup notification for \"%s\".\n",
			  sn_launchee_context_get_startup_id(ctx) ?
			  sn_launchee_context_get_startup_id(ctx) : "?");
		sn_launchee_context_complete(ctx);
		sn_launchee_context_unref(ctx);
	}
	sn_display_unref(disp);
#endif
}

/* Setup startup notification for our child. */
static void
userhelper_startup_notification_launcher(void)
{
#ifdef USE_STARTUP_NOTIFICATION
	GdkDisplay *gdisp;
	GdkScreen *gscreen;
	SnDisplay *disp;
	SnLauncherContext *ctx;
	int screen;

	if (sn_name == NULL) {
		return;
	}

	gdisp = gdk_display_get_default();
	gscreen = gdk_display_get_default_screen(gdisp);
	disp = sn_display_new(GDK_DISPLAY(), trap_push, trap_pop);
	screen = gdk_screen_get_number(gscreen);
	ctx = sn_launcher_context_new(disp, screen);

	if (sn_name) {
		sn_launcher_context_set_name(ctx, sn_name);
	}
	if (sn_description) {
		sn_launcher_context_set_description(ctx, sn_description);
	}
	if (sn_workspace != -1) {
		sn_launcher_context_set_workspace(ctx, sn_workspace);
	}
	if (sn_wmclass) {
		sn_launcher_context_set_wmclass(ctx, sn_wmclass);
	}
	if (sn_binary_name) {
		sn_launcher_context_set_binary_name(ctx, sn_binary_name);
	}
	if (sn_icon_name) {
		sn_launcher_context_set_binary_name(ctx, sn_icon_name);
	}
	debug_msg("Starting launch of \"%s\", id=\"%s\".\n",
		  sn_description ? sn_description : sn_name, sn_id);
	sn_launcher_context_initiate(ctx, "userhelper", sn_name, CurrentTime);
	if (sn_launcher_context_get_startup_id(ctx) != NULL) {
		sn_id = g_strdup(sn_launcher_context_get_startup_id(ctx));
	}

	sn_launcher_context_unref(ctx);
	sn_display_unref(disp);
#endif
}

/* Call gtk_main_quit. */
void
userhelper_main_quit(void)
{
	debug_msg("Quitting main loop %d.\n", gtk_main_level());
	gtk_main_quit();
}

static void
userhelper_fatal_error(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
	(void)widget;
	(void)event;
	(void)user_data;
	userhelper_main_quit();
}


/* Display a dialog explaining a child's exit status, and exit ourselves. */
static void
userhelper_parse_exitstatus(int exitstatus)
{
	const char *message;
	enum uh_message_type type;
	GtkWidget *message_box;

	if (child_was_execed)
		debug_msg("Wrapped application returned exit status %d.\n",
			  exitstatus);
	else
		debug_msg("Child returned exit status %d.\n", exitstatus);

	/* If the exit status came from what the child execed, then we don't
	 * care about reporting it to the user. */
	if (child_was_execed) {
		child_exit_status = exitstatus;
		return;
	}

	/* Create a dialog suitable for displaying this code. */
	uh_exitstatus_message(exitstatus, &message, &type);
	debug_msg("Status is \"%s\".\n", message);

	/* If we recognize this code, create the error dialog for it if we
	   need to display one. */
	switch (type) {
	case UHM_MESSAGE:
		message_box = create_message_box(message, NULL);
		break;
	case UHM_ERROR:
		message_box = create_error_box(message, NULL);
		break;
	case UHM_SILENT:
		message_box = NULL;
		break;
	default:
		g_assert_not_reached();
	}

	/* Run the dialog box. */
	if (message_box != NULL) {
		if (child_success_dialog || (exitstatus != 0)) {
			gtk_dialog_run(GTK_DIALOG(message_box));
		}
		gtk_widget_destroy(message_box);
	}
}

/* Attempt to grab focus for the toplevel of this widget, so that peers can
 * get events too. */
static void
userhelper_grab_keyboard(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
	(void)event;
	(void)user_data;
#ifndef DEBUG_USERHELPER
	GdkGrabStatus ret;

	ret = gdk_keyboard_grab(gtk_widget_get_window (widget), TRUE,
				GDK_CURRENT_TIME);
	if (ret != GDK_GRAB_SUCCESS) {
		switch (ret) {
		case GDK_GRAB_ALREADY_GRABBED:
			g_warning("keyboard grab failed: keyboard already grabbed by another window");
			break;
		case GDK_GRAB_INVALID_TIME:
			g_warning("keyboard grab failed: requested grab time was invalid (shouldn't happen)");
			break;
		case GDK_GRAB_NOT_VIEWABLE:
			g_warning("keyboard grab failed: window requesting grab is not viewable");
			break;
		case GDK_GRAB_FROZEN:
			g_warning("keyboard grab failed: keyboard is already grabbed");
			break;
		default:
			g_warning("keyboard grab failed: unknown error %d", (int) ret);
			break;
		}
	}
#endif
}

/* Try to send a string encoded in UTF-8 to the child. */
static void
write_childin_string(const char *s)
{
	char *converted;
	gsize s_len, converted_len;
	GError *err;

	s_len = strlen(s);

	err = NULL;
	converted = g_locale_from_utf8(s, s_len, NULL, &converted_len, &err);
	if (err == NULL) {
		write(childin[1], converted, converted_len);
		g_free(converted);
	} else {
		g_error_free(err);
		/* Oh well, some data is better than no data... */
		write(childin[1], s, s_len);
	}
}

/* Handle the executed dialog, writing responses back to the child when
 * possible. */
static void
userhelper_write_childin(GtkResponseType response, struct response *resp)
{
	static const unsigned char sync_point[] = { UH_SYNC_POINT, '\n' };
	static const unsigned char eol = '\n';

	gboolean startup = FALSE;

	switch (response) {
	case RESPONSE_FALLBACK: {
		/* The user wants to run unprivileged. */
		static const unsigned char cmd[] = { UH_FALLBACK, '\n' };

		debug_msg("Responding FALLBACK.\n");
		write(childin[1], cmd, sizeof(cmd));
		startup = TRUE;
		break;
	}
	case GTK_RESPONSE_CANCEL: {
		/* The user doesn't want to run this after all. */
		static const unsigned char cmd[] = { UH_CANCEL, '\n' };

		debug_msg("Responding CANCEL.\n");
		write(childin[1], cmd, sizeof(cmd));
		startup = FALSE;
		break;
	}
	case GTK_RESPONSE_OK: {
		GList *message_list;

		/* The user answered the questions. */
		for (message_list = resp->message_list;
		     message_list != NULL && message_list->data != NULL;
		     message_list = g_list_next(message_list)) {
			struct message *m = ((struct message *)
					     message_list->data);
			debug_msg("message %d\n", m->type);
			if (GTK_IS_ENTRY(m->entry))
				debug_msg("Responding `%s'.\n",
					  gtk_entry_get_text(GTK_ENTRY
							     (m->entry)));
			if (GTK_IS_ENTRY(m->entry)) {
				static const unsigned char cmd = UH_TEXT;

				const char *s;

				s = gtk_entry_get_text(GTK_ENTRY (m->entry));
				write(childin[1], &cmd, sizeof(cmd));
				write_childin_string(s);
				write(childin[1], &eol, sizeof(eol));
			}
		}
		startup = TRUE;
		break;
	}
	default:
		/* We were closed, deleted, canceled, or something else which
		   we can treat as a cancellation. */
		startup = FALSE;
		_exit(1);
		break;
	}
	if (startup && sn_name != NULL) {
		/* If we haven't set up notification yet, do so. */
		if (sn_id == NULL)
			userhelper_startup_notification_launcher();
#ifdef USE_STARTUP_NOTIFICATION
		/* Tell the child what its ID is. */
		if (sn_id != NULL) {
			static const unsigned char cmd = UH_SN_ID;

			debug_msg("Sending new window startup ID \"%s\".\n",
				  sn_id);
			write(childin[1], &cmd, sizeof(cmd));
			write_childin_string(sn_id);
			write(childin[1], &eol, sizeof(eol));
		}
#endif
	}
	/* Tell the child we have no more to say. */
	debug_msg("Sending synchronization point.\n");
	write(childin[1], sync_point, sizeof(sync_point));
}

/* Glue. */
static void
fake_respond_ok(GtkWidget *widget, gpointer user_data)
{
	(void)widget;
	gtk_dialog_response(GTK_DIALOG(user_data), GTK_RESPONSE_OK);
}

/* Used only when resp->dialog is displayed non-modally. */
static void
userhelper_dialog_response(GtkDialog *dialog, gint response, gpointer user_data)
{
	(void)dialog;
	(void)user_data;
	if (response == GTK_RESPONSE_CANCEL)
		/* This is the best we can do because the child is not waiting
		   for a reponse. */ {
		g_print("cancel from response!\n");
		userhelper_main_quit();
	}
}

/* Handle a request from the child userhelper process and display it in a
   message box. */
static void
userhelper_handle_childout(char prompt_type, char *prompt)
{
	static struct response *resp = NULL;

	if (resp == NULL) {
		/* Allocate the response structure. */
		resp = g_malloc0(sizeof(struct response));

		/* Create a table to hold the entry fields and labels. */
		resp->table = gtk_table_new(2, 1, FALSE);
		/* Hold to the table after gtk_box_pack_start(). */
		g_object_ref_sink(resp->table);
		/* The First row is used for the "Authenticating as \"%s\""
		   label. */
		resp->rows = 1;
	}


	debug_msg("Child message: (%d)/\"%s\"\n", prompt_type, prompt);

	switch (prompt_type) {
	case UH_PROMPT_SUGGESTION:
		/* A suggestion for the next input. */
		g_free(resp->suggestion);
		resp->suggestion = g_strdup(prompt);
		debug_msg("Suggested response \"%s\".\n", resp->suggestion);
		break;
	case UH_ECHO_OFF_PROMPT: case UH_ECHO_ON_PROMPT: {
		struct message *msg;

		/* Prompts.  Create a label and entry field. */
		msg = g_malloc(sizeof(*msg));
		msg->type = prompt_type;
		/* Only set the title to "Query" if it isn't already set to
		  "Error" or something else more meaningful. */
		if (resp->title == NULL)
			resp->title = _("Query");
		/* Create a label to hold the prompt */
		msg->label = gtk_label_new(_(prompt));
		gtk_label_set_line_wrap(GTK_LABEL(msg->label), TRUE);
		gtk_misc_set_alignment(GTK_MISC(msg->label), 1.0, 0.5);

		/* Create an entry field to hold the answer. */
		msg->entry = gtk_entry_new();
		/* Make a feeble gesture at being accessible. */
		gtk_label_set_mnemonic_widget(GTK_LABEL(msg->label),
					      GTK_WIDGET(msg->entry));
		gtk_entry_set_visibility(GTK_ENTRY(msg->entry),
					 prompt_type == UH_ECHO_ON_PROMPT);

		/* If we had a suggestion, use it up. */
		if (resp->suggestion) {
			gtk_entry_set_text(GTK_ENTRY(msg->entry),
					   resp->suggestion);
			g_free(resp->suggestion);
			resp->suggestion = NULL;
		}

		/* Keep track of the first entry field in the dialog box. */
		if (resp->first == NULL)
			resp->first = msg->entry;

		/* Keep track of the last entry field in the dialog box. */
		resp->last = msg->entry;

		/* Insert them. */
		gtk_table_attach(GTK_TABLE(resp->table), msg->label, 0, 1,
				 resp->rows, resp->rows + 1,
				 GTK_EXPAND | GTK_FILL, 0, PAD, PAD);
		gtk_table_attach(GTK_TABLE(resp->table), msg->entry, 1, 2,
				 resp->rows, resp->rows + 1,
				 GTK_EXPAND | GTK_FILL, 0, PAD, PAD);

		/* Add this message to the list of messages. */
		resp->message_list = g_list_append(resp->message_list, msg);

		/* Note that this one needs a response. */
		resp->responses++;
		resp->rows++;
		debug_msg("Now we need %d responses.\n", resp->responses);
		break;
	}
	case UH_FALLBACK_ALLOW:
		/* Fallback flag.  Read it and save it for later. */
		resp->fallback_allowed = atoi(prompt) != 0;
		debug_msg("Fallback %sallowed.\n",
			  resp->fallback_allowed ? "" : "not ");
		break;
	case UH_USER: {
		char *text;

		/* Tell the user which user's passwords to enter */
		if (resp->user_label != NULL)
			gtk_widget_destroy(resp->user_label);
		debug_msg("User is \"%s\".\n", prompt);
		text = g_strdup_printf(_("Authenticating as \"%s\""), prompt);
		resp->user_label = gtk_label_new(text);
		g_free(text);
		gtk_misc_set_alignment(GTK_MISC(resp->user_label), 0.0, 0.5);
		gtk_table_attach(GTK_TABLE(resp->table), resp->user_label, 0, 2,
				 0, 1, GTK_EXPAND | GTK_FILL, 0, PAD, PAD);
		break;
	}
	case UH_SERVICE_NAME:
		/* Service name. Read it and save it for later. */
		g_free(resp->service);
		resp->service = g_strdup(prompt);
		debug_msg("Service is \"%s\".\n", resp->service);
		break;
	case UH_ERROR_MSG: case UH_INFO_MSG: {
		struct message *msg;

		/* An error/informational message. */
		resp->title = (prompt_type == UH_ERROR_MSG
			       ? _("Error") : _("Information"));
		msg = g_malloc(sizeof(*msg));
		msg->type = prompt_type;
		msg->entry = NULL;
		msg->label = gtk_label_new(_(prompt));
		gtk_misc_set_alignment(GTK_MISC(msg->label), 0.0, 0.5);
		gtk_table_attach(GTK_TABLE(resp->table), msg->label, 0, 2,
				 resp->rows, resp->rows + 1,
				 GTK_EXPAND | GTK_FILL, 0, PAD, PAD);
		resp->message_list = g_list_append(resp->message_list, msg);
		resp->rows++;
		break;
	}
	case UH_BANNER:
		/* An informative banner. */
		g_free(resp->banner);
		resp->banner = g_strdup(prompt);
		debug_msg("Banner is \"%s\".\n", resp->banner);
		break;
	case UH_EXEC_START:
		/* Userhelper is trying to exec. */
		child_was_execed = TRUE;
		debug_msg("Child started.\n");
		break;
	case UH_EXEC_FAILED:
		/* Userhelper failed to exec. */
		child_was_execed = FALSE;
		debug_msg("Child failed.\n");
		break;
	case UH_SN_NAME:
		/* Startup notification name. */
		g_free(sn_name);
		sn_name = g_strdup(prompt);
		debug_msg("SN Name is \"%s\".\n", sn_name);
		break;
	case UH_SN_DESCRIPTION:
		/* Startup notification description. */
		g_free(sn_description);
		sn_description = g_strdup(prompt);
		debug_msg("SN Description is \"%s\".\n", sn_description);
		break;
	case UH_SN_WORKSPACE:
		/* Startup notification workspace. */
		sn_workspace = atoi(prompt);
		debug_msg("SN Workspace is %d.\n", sn_workspace);
		break;
	case UH_SN_WMCLASS:
		/* Startup notification wmclass. */
		g_free(sn_wmclass);
		sn_wmclass = g_strdup(prompt);
		debug_msg("SN WMClass is \"%s\".\n", sn_wmclass);
		break;
	case UH_SN_BINARY_NAME:
		/* Startup notification binary name. */
		g_free(sn_binary_name);
		sn_binary_name = g_strdup(prompt);
		debug_msg("SN Binary name is \"%s\".\n", sn_binary_name);
		break;
	case UH_SN_ICON_NAME:
		/* Startup notification icon name. */
		g_free(sn_icon_name);
		sn_icon_name = g_strdup(prompt);
		debug_msg("SN Icon name is \"%s\".\n", sn_icon_name);
		break;
	case UH_EXPECT_RESP:
		/* Sanity-check for the number of expected responses. */
		if (resp->responses != atoi(prompt)) {
			fprintf(stderr, "Protocol error (%d responses expected "
				"from %d prompts)!\n", atoi(prompt),
				resp->responses);
			_exit(1);
		}
		break;
		/* Synchronization point -- no more prompts. */
	case UH_SYNC_POINT:
		break;
	default:
		break;
	}

	/* Complete startup notification for consolehelper. */
	userhelper_startup_notification_launchee(NULL);

	if (prompt_type != UH_SYNC_POINT)
		return;

	/* Destroy the dialog if a child is executing because userhelper
	   already has all required information.  Destroy the dialog also if
	   resp->responses != 0 because the dialog has a slightly different
	   layout in that case, and if resp->dialog != NULL, it must have been
	   created when resp->responses was 0. */
	if (resp->dialog != NULL
	    && (child_was_execed || resp->responses != 0)) {
		GtkWidget *vbox;

		/* Don't destroy anything inside resp->table. */
		vbox = gtk_dialog_get_content_area (GTK_DIALOG(resp->dialog));
		gtk_container_remove(GTK_CONTAINER(vbox), resp->table);
		gtk_widget_destroy(resp->dialog);
		resp->dialog = NULL;
	}

	/* Don't show any diaogs if a child is executing or there is nothing
	   to show. */
	if (child_was_execed || (resp->responses == 0 && resp->rows == 1)) {
		/* This can happens when the child needs to wait for
		   UH_EXEC_START or UH_EXEC_FAILED response to make sure the
		   exit status is processed correctly. */
		userhelper_write_childin(GTK_RESPONSE_OK, resp);
		return;
	}

	/* Create and show the dialog if there is a message, even if no
	   reponses are required.  The dialog might ask the user to do
	   something else (e.g. insert a hardware token). */
	if (resp->dialog == NULL) {
		char *text;
		GtkWidget *label, *vbox;

		/* If we already set up startup notification for the child,
		   something must have gone wrong (e.g. wrong password).  Clean
		   it up, which will allow setting it up again. */
		if (sn_id != NULL) {
			userhelper_startup_notification_launchee(sn_id);
			g_free(sn_id);
			sn_id = NULL;
		}
		resp->dialog = gtk_message_dialog_new(NULL, 0,
						      resp->responses > 0 ?
						      GTK_MESSAGE_QUESTION :
						      GTK_MESSAGE_INFO,
						      resp->responses > 0 ?
						      GTK_BUTTONS_OK_CANCEL :
						      GTK_BUTTONS_CANCEL,
						      "Placeholder text.");
		/* Ensure that we don't get dangling crap widget pointers. */
		g_object_add_weak_pointer(G_OBJECT(resp->dialog),
					  &resp->dialog);
		/* If we didn't get a title from userhelper, assume badness. */
		gtk_window_set_title(GTK_WINDOW(resp->dialog),
				     resp->title ? resp->title : _("Error"));
		gtk_window_set_position(GTK_WINDOW(resp->dialog),
					GTK_WIN_POS_CENTER_ALWAYS);
		gtk_window_set_keep_above(GTK_WINDOW(resp->dialog), TRUE);
		gtk_window_set_icon_from_file(GTK_WINDOW(resp->dialog),
					      PIXMAPDIR "/password.png", NULL);
		vbox = gtk_dialog_get_content_area (GTK_DIALOG(resp->dialog));
		gtk_box_pack_start(GTK_BOX(vbox), resp->table, TRUE, TRUE, 0);

		if (resp->responses > 0)
			g_signal_connect(G_OBJECT(resp->dialog), "map_event",
					 G_CALLBACK(userhelper_grab_keyboard),
					 NULL);
		g_signal_connect(G_OBJECT(resp->dialog), "delete_event",
				 G_CALLBACK(userhelper_fatal_error),
				 NULL);
		resp->dialog_response_handler
			= g_signal_connect(G_OBJECT(resp->dialog), "response",
					   G_CALLBACK
					   (userhelper_dialog_response), NULL);

		if (resp->responses == 0)
			text = g_strdup("");
		else if (resp->service != NULL) {
			if (strcmp(resp->service, "passwd") == 0)
				text = g_strdup(_("Changing password."));
			else if (strcmp(resp->service, "chfn") == 0)
				text = g_strdup(_("Changing personal "
						  "information."));
			else if (strcmp(resp->service, "chsh") == 0)
				text = g_strdup(_("Changing login shell."));
			else if (resp->banner != NULL)
				text = g_strdup(resp->banner);
			else if (resp->fallback_allowed)
				text = g_strdup_printf(_("You are attempting to run \"%s\" which may benefit from administrative privileges, but more information is needed in order to do so."), resp->service);
			else
				text = g_strdup_printf(_("You are attempting to run \"%s\" which requires administrative privileges, but more information is needed in order to do so."), resp->service);
		} else if (resp->banner != NULL)
			text = g_strdup(resp->banner);
		else if (resp->fallback_allowed)
			text = g_strdup(_("You are attempting to run a command which may benefit from administrative privileges, but more information is needed in order to do so."));
		else
			text = g_strdup(_("You are attempting to run a command which requires administrative privileges, but more information is needed in order to do so."));
		label = (GTK_MESSAGE_DIALOG(resp->dialog))->label;
		gtk_label_set_text(GTK_LABEL(label), text);
		g_free(text);
	}
	/* Do this every time to show widgets newly added to resp->table. */
	gtk_widget_show_all(resp->dialog);

	if (resp->responses == 0)
		/* This might be just a module being stupid and calling the
		   conversation callback once. for. every. chunk. of. output
		   and we'll get an actual prompt later. */
		userhelper_write_childin(GTK_RESPONSE_OK, resp);
	else {
		/* A non-zero number of queries demands an answer. */
		GtkWidget *image;
		GtkResponseType response;

#ifdef DEBUG_USERHELPER
		{
		int timeout = 2;
		debug_msg("Ready to ask %d questions.\n", resp->responses);
		debug_msg("Pausing for %d seconds for debugging.\n", timeout);
		sleep(timeout);
		}
#endif

		/* We're asking questions, change the dialog's icon. */
		image = gtk_message_dialog_get_image (GTK_MESSAGE_DIALOG
						      (resp->dialog));
		gtk_image_set_from_file(GTK_IMAGE(image),
					PIXMAPDIR "/keyring.png");

		/* Add an "unprivileged" button if we're allowed to offer
		 * unprivileged execution as an option. */
		if (resp->fallback_allowed) {
			gtk_dialog_add_button(GTK_DIALOG(resp->dialog),
					      _("_Run Unprivileged"),
					      RESPONSE_FALLBACK);
		}

		/* Have the activation signal for the last entry field be
		 * equivalent to hitting the default button. */
		if (resp->last) {
			g_signal_connect(G_OBJECT(resp->last), "activate",
					 G_CALLBACK(fake_respond_ok),
					 resp->dialog);
		}

		/* Show the dialog and grab focus. */
		gtk_widget_show_all(resp->dialog);
		if (GTK_IS_ENTRY(resp->first)) {
			gtk_widget_grab_focus(resp->first);
		}

		/* Run the dialog. */
		g_signal_handler_disconnect(resp->dialog,
					    resp->dialog_response_handler);
		response = gtk_dialog_run(GTK_DIALOG(resp->dialog));

		/* Release the keyboard. */
		gdk_keyboard_ungrab(GDK_CURRENT_TIME);

		/* Answer the child's questions. */
		userhelper_write_childin(response, resp);

		/* Destroy the dialog box. */
		gtk_widget_destroy(resp->dialog);
		g_object_unref(resp->table);
		g_free(resp->banner);
		g_free(resp->suggestion);
		g_free(resp->service);
		if (resp->message_list) {
			GList *e;

			for (e = g_list_first(resp->message_list); e != NULL;
			     e = g_list_next(e))
				g_free(e->data);
			g_list_free(resp->message_list);
		}
		g_free(resp);
		resp = NULL;
	}
}

/* Handle a child-exited signal by disconnecting from its stdout. */
static void
userhelper_child_exited(GPid pid, int status, gpointer data)
{
	(void)data;
	debug_msg("Child %d exited (looking for %d).\n", pid, childpid);

	if (pid == childpid) {
		/* If we're doing startup notification, clean it up just in
		 * case the child didn't complete startup. */
		if (sn_id != NULL) {
			userhelper_startup_notification_launchee(sn_id);
		}
		/* If we haven't lost the connection with the child, it's
		 * gone now. */
		if (childout_tag != 0) {
			g_source_remove(childout_tag);
			childout_tag = 0;
		}
		if (WIFEXITED(status)) {
			debug_msg("Child %d exited normally, ret = %d.\n", pid,
				  WEXITSTATUS(status));
			userhelper_parse_exitstatus(WEXITSTATUS(status));
		} else {
			debug_msg("Child %d exited abnormally.\n", pid);
			if (WIFSIGNALED(status)) {
				debug_msg("Child %d died on signal %d.\n", pid,
					  WTERMSIG(status));
				userhelper_parse_exitstatus(ERR_UNK_ERROR);
			}
		}
		userhelper_main_quit();
	}
}

/* Read data sent from the child userhelper process and pass it on to
 * userhelper_parse_childout(). */
static gboolean
userhelper_read_childout(GIOChannel *source, GIOCondition condition,
			 gpointer data)
{
	(void)data;
	if ((condition & (G_IO_ERR | G_IO_NVAL)) != 0) {
		/* Serious error, this is.  Panic, we must. */
		_exit(1);
	}
	if ((condition & G_IO_HUP) != 0) {
		/* EOF from the child. */
		debug_msg("EOF from child.\n");
		childout_tag = 0;
		return FALSE;
	}
	while ((condition & G_IO_IN) != 0) {
		char command, eol, buf[BUFSIZ], *end, *converted;
		unsigned long request_size;
		gsize bytes_read;
		GError *err;

		err = NULL;
		g_io_channel_read_chars(source, &command, 1, &bytes_read, &err);
		if (err != NULL || bytes_read != 1)
			_exit(0); /* EOF, or error of some kind. */
		assert(UH_REQUEST_SIZE_DIGITS + 1 <= sizeof(buf));
		g_io_channel_read_chars(source, buf, UH_REQUEST_SIZE_DIGITS,
					&bytes_read, &err);
		if (err != NULL || bytes_read != UH_REQUEST_SIZE_DIGITS
		    || !g_ascii_isdigit(*buf))
			_exit(1); /* Error of some kind. */
		buf[bytes_read] = '\0';
		errno = 0;
		request_size = strtoul(buf, &end, 10);
		if (errno != 0 || *end != 0 || end == buf ||
		    request_size + 1 > sizeof(buf))
			_exit(1); /* Error of some kind. */
		g_io_channel_read_chars(source, buf, request_size, &bytes_read,
					&err);
		if (err != NULL || bytes_read != request_size)
			_exit(1); /* Error of some kind. */
		converted = g_locale_to_utf8(buf, request_size, &bytes_read,
					     NULL, &err);
		if (err != NULL || bytes_read != request_size)
			_exit(1);
		g_io_channel_read_chars(source, &eol, 1, &bytes_read, &err);
		if (err != NULL || bytes_read != 1 || eol != '\n')
			_exit(0); /* EOF, or error of some kind. */
		userhelper_handle_childout(command, converted);
		g_free(converted);
		condition = g_io_channel_get_buffer_condition(source);
	}
	return TRUE;
}

int
userhelper_runv(gboolean dialog_success, const char *path, char **args)
{
	int childout[2];

	/* Create pipes with which to interact with the userhelper child. */
	if ((pipe(childout) == -1) || (pipe(childin) == -1)) {
		fprintf(stderr, _("Pipe error.\n"));
		_exit(1);
	}

	/* Start up a new process. */
	childpid = fork();
	if (childpid == -1) {
		fprintf(stderr, _("Cannot fork().\n"));
		_exit(0);
	}

	if (childpid > 0) {
		GIOChannel *childout_channel;
		GError *err;

		/* We're the parent; close the write-end of the reading pipe,
		 * and the read-end of the writing pipe. */
		close(childout[1]);
		close(childin[0]);

		/* Keep track of whether or not we need to display a dialog
		 * box on successful termination. */
		child_success_dialog = dialog_success;

		/* Watch the reading end of the reading pipe for data from the
		   child. */
		childout_channel = g_io_channel_unix_new(childout[0]);

		err = NULL;
		g_io_channel_set_encoding(childout_channel, NULL, &err);
		if (err != NULL) {
			fprintf(stderr, _("Can't set binary encoding on an IO "
					  "channel: %s\n"), err->message);
			_exit(1);
		}
		g_io_channel_set_line_term(childout_channel, "\n", -1);

		childout_tag = g_io_add_watch(childout_channel,
					      G_IO_IN | G_IO_HUP | G_IO_ERR
					      | G_IO_NVAL,
					      userhelper_read_childout, NULL);

		/* Watch for child exits. */
		child_exit_status = 0;
		g_child_watch_add(childpid, userhelper_child_exited, NULL);
		debug_msg("Running child pid=%ld.\n", (long) childpid);

		/* Tell the child we're ready for it to run. */
		write(childin[1], "Go", 1);
		gtk_main();

		/* If we're doing startup notification, clean it up just in
		 * case the child didn't complete startup. */
		userhelper_startup_notification_launchee(sn_id);

		debug_msg("Child exited, continuing.\n");
	} else {
		int i, fd[4];
		unsigned char byte;
		long open_max;

		/* We're the child; close the read-end of the parent's reading
		 * pipe, and the write-end of the parent's writing pipe. */
		close(childout[0]);
		close(childin[1]);

		/* Read one byte from the parent so that we know it's ready
		 * to go. */
		read(childin[0], &byte, 1);

		/* Close all of descriptors which aren't stdio or the two
		 * pipe descriptors we'll be using. */
		open_max = sysconf(_SC_OPEN_MAX);
		for (i = 3; i < open_max; i++) {
			if ((i != childout[1]) && (i != childin[0])) {
				close(i);
			}
		}

		/* First create two copies of stdin, in case 3 and 4 aren't
		   currently in use.  STDIN_FILENO is surely valid even if the
		   user has started us with <&- because the X11 socket uses
		   a file descriptor. */
		fd[0] = dup(STDIN_FILENO);
		fd[1] = dup(STDIN_FILENO);

		/* Now create temporary copies of the pipe descriptors, which
		 * aren't goint to be 3 or 4 because they are surely in use
		 * by now. */
		fd[2] = dup(childin[0]);
		fd[3] = dup(childout[1]);

		/* Now get rid of the temporary descriptors, */
		close(fd[0]);
		close(fd[1]);
		close(childin[0]);
		close(childout[1]);

		/* and move the pipe descriptors to their new homes. */
		if (dup2(fd[2], UH_INFILENO) == -1) {
			fprintf(stderr, _("dup2() error.\n"));
			_exit(2);
		}
		if (dup2(fd[3], UH_OUTFILENO) == -1) {
			fprintf(stderr, _("dup2() error.\n"));
			_exit(2);
		}
		close(fd[2]);
		close(fd[3]);

#ifdef DEBUG_USERHELPER
		for (i = 0; args[i] != NULL; i++)
			debug_msg("Exec arg %d = \"%s\".\n", i, args[i]);
#endif
		execv(path, args);
		fprintf(stderr, _("execl() error, errno=%d\n"), errno);
		_exit(0);
	}
	return child_exit_status;
}

void
userhelper_run(gboolean dialog_success, const char *path, ...)
{
	va_list ap;
	char **argv;
	int argc = 0;
	int i = 0;

	/* Count the number of arguments. */
	va_start(ap, path);
	while (va_arg(ap, char *) != NULL) {
		argc++;
	}
	va_end(ap);

	/* Copy the arguments into a normal array. */
	argv = g_malloc_n(argc + 1, sizeof(*argv));
	va_start(ap, path);
	for (i = 0; i < argc; i++)
		argv[i] = va_arg(ap, char *);
	argv[i] = NULL;
	va_end(ap);

	/* Pass the array into userhelper_runv() to actually run it. */
	userhelper_runv(dialog_success, path, argv);

	g_free(argv);
}

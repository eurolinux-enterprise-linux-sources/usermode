/*
 * Copyright (C) 1997-2003, 2007, 2008 Red Hat, Inc.  All rights reserved.
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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <libintl.h>
#include <locale.h>
#include <math.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib-object.h>

#include <security/pam_appl.h>
#include <security/pam_misc.h>

#include <libuser/user.h>

#ifdef WITH_SELINUX
#include <selinux/selinux.h>
#include <selinux/flask.h>
#include <selinux/av_permissions.h>
#endif

#include "shvar.h"
#include "userhelper.h"

#ifdef DEBUG_USERHELPER
#define debug_msg(...) g_print(__VA_ARGS__)
#else
#define debug_msg(...) ((void)0)
#endif

/* A maximum GECOS field length.  There's no hard limit, so we guess. */
#define GECOS_LENGTH			127

/* A structure to hold broken-out GECOS data.  The number and names of the
 * fields are dictated entirely by the flavor of finger we use.  Seriously. */
struct gecos_data {
	char *full_name;	/* full user name */
	char *office;		/* office */
	char *office_phone;	/* office phone */
	char *home_phone;	/* home phone */
	char *site_info;	/* other stuff */
};

/* We manipulate the environment directly, so we have to declare (but not
 * define) the right variable here. */
extern char **environ;

/* A structure type which we use to carry psuedo-global data around with us. */
struct app_data {
	pam_handle_t *pamh;
	gboolean fallback_allowed, fallback_chosen, canceled;
	FILE *input, *output;
	const char *banner, *domain;
#ifdef USE_STARTUP_NOTIFICATION
	const char *sn_name, *sn_description, *sn_wmclass;
	const char *sn_binary_name, *sn_icon_name;
	char *sn_id;
	int sn_workspace;
#endif
};

#ifdef WITH_SELINUX
static int checkAccess(unsigned int selaccess) {
  int status=-1;
  security_context_t user_context;
  if( getprevcon(&user_context)==0 ) {
    struct av_decision avd;
    int retval = security_compute_av(user_context,
				     user_context,
				     SECCLASS_PASSWD,
				     selaccess,
				     &avd);
	  
    if ((retval == 0) && 
	((selaccess & avd.allowed) == selaccess)) {
      status=0;
    } 
    freecon(user_context);
  }

  if (status != 0 && security_getenforce()==0) {
      status=0;
  }
  return status;
}
#endif /* WITH_SELINUX */

/* Exit, returning the proper status code based on a PAM error code. */
static int G_GNUC_NORETURN
fail_exit(struct app_data *data, int pam_retval)
{
	int status;

	/* This is a local error.  Bail. */
	if (pam_retval == ERR_SHELL_INVALID) {
		exit(ERR_SHELL_INVALID);
	}

	if (pam_retval == PAM_SUCCESS)
		/* Just exit. */
		status = 0;
	/* Map the PAM error code to a local error code and return it to the
	   parent process.  Trust the canceled flag before any PAM error
	   codes. */
	else if (data->canceled)
		status = ERR_CANCELED;
	else {
		debug_msg("userhelper: got PAM error %d.\n", pam_retval);
		switch (pam_retval) {
			case PAM_OPEN_ERR:
			case PAM_SYMBOL_ERR:
			case PAM_SERVICE_ERR:
			case PAM_SYSTEM_ERR:
			case PAM_BUF_ERR:
				status = ERR_PAM_INT_ERROR;
				break;
			case PAM_AUTH_ERR:
			case PAM_AUTHTOK_ERR:
			case PAM_PERM_DENIED:
				status = ERR_PASSWD_INVALID;
				break;
			case PAM_AUTHTOK_LOCK_BUSY:
				status = ERR_LOCKS;
				break;
			case PAM_CRED_INSUFFICIENT:
			case PAM_AUTHINFO_UNAVAIL:
			case PAM_CRED_UNAVAIL:
			case PAM_CRED_EXPIRED:
			case PAM_AUTHTOK_EXPIRED:
				status = ERR_NO_RIGHTS;
				break;
			case PAM_USER_UNKNOWN:
				status = ERR_NO_USER;
				break;
			case PAM_ABORT:
				/* fall through */
			default:
				status = ERR_UNK_ERROR;
				break;
		}
	}
	debug_msg("userhelper: exiting with status %d.\n", status);
	exit(status);
}

/* Read a string from stdin, and return a freshly-allocated copy, without
 * the end-of-line terminator if there was one, and with an optional
 * consolehelper message header removed. */
static char *
read_reply(FILE *fp)
{
	char buffer[BUFSIZ];
	size_t slen;

	if (feof(fp))
		return NULL;
	if (fgets(buffer, sizeof(buffer), fp) == NULL)
		return NULL;

	slen = strlen(buffer);
	while (slen > 1
	       && (buffer[slen - 1] == '\n' || buffer[slen - 1] == '\r')) {
		buffer[slen - 1] = '\0';
		slen--;
	}

	return g_strdup(buffer);
}

/* Send a request. */
static void
send_request(FILE *fp, char request_type, const char *data)
{
	size_t len;

	if (data == NULL)
		data = "";
	len = strlen(data);
	assert(len < powl(10.0, UH_REQUEST_SIZE_DIGITS));
	fprintf(fp, "%c%0*zu%s\n", request_type, UH_REQUEST_SIZE_DIGITS, len,
		data);
}

/* Send a request with an integer payload. */
static void
send_request_int(FILE *fp, char request_type, int data)
{
	/* log2(10) > 3, so sizeof(int) * CHAR_BIT / 3 digits are necessary
	   to represent all values, + 1 to avoid rounding down "partial
	   digits". */
	char buf[sizeof(int) * CHAR_BIT / 3 + 1 + 1];

	sprintf(buf, "%d", data);
	send_request(fp, request_type, buf);
}

/* A text-mode conversation function suitable for use when there is no
 * controlling terminal. */
static int
silent_converse(int num_msg, const struct pam_message **msg,
		struct pam_response **resp, void *appdata_ptr)
{
	(void)num_msg;
	(void)msg;
	(void)resp;
	(void)appdata_ptr;
	return PAM_CONV_ERR;
}

static int
get_pam_string_item(pam_handle_t *pamh, int item, const char **out)
{
	const void *s;
	int ret;

	ret = pam_get_item(pamh, item, &s);
	*out = s;
	return ret;
}

/* Free the first COUNT entries of RESP, and RESP itself */
static void
free_reply(struct pam_response *resp, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		free(resp[i].resp);
	free(resp);
}

/* A mixed-mode conversation function suitable for use with X. */
static int
converse_pipe(int num_msg, const struct pam_message **msg,
	      struct pam_response **resp, void *appdata_ptr)
{
	int count, expected_responses, received_responses;
	struct pam_response *reply;
	char *string;
	const char *user, *service;
	struct app_data *data = appdata_ptr;

	/* Pass on any hints we have to the consolehelper. */

	/* Since PAM does not handle our cancel request we'll we have to do it
	   ourselves.  Don't bother user with messages if already canceled. */
	if (data->canceled) {
		debug_msg("userhelper (cp): we were already canceled\n");
		return PAM_ABORT;
	}


	/* User. */
	if ((get_pam_string_item(data->pamh, PAM_USER, &user) != PAM_SUCCESS) ||
	    (user == NULL) ||
	    (strlen(user) == 0)) {
		user = "root";
	}
	debug_msg("userhelper (cp): converse_pipe_called(num_msg=%d, "
		  "canceled=%d)\n", num_msg, data->canceled);
	debug_msg("userhelper (cp): sending user `%s'\n", user);
	send_request(data->output, UH_USER, user);

	/* Service. */
	if (get_pam_string_item(data->pamh, PAM_SERVICE,
				&service) == PAM_SUCCESS) {
		debug_msg("userhelper (cp): sending service `%s'\n", service);
		send_request(data->output, UH_SERVICE_NAME, service);
	}

	/* Fallback allowed? */
	debug_msg("userhelper (cp): sending fallback = %d.\n",
		  data->fallback_allowed ? 1 : 0);
	send_request_int(data->output, UH_FALLBACK_ALLOW,
			 data->fallback_allowed ? 1 : 0);

	/* Banner. */
	if ((data->domain != NULL) && (data->banner != NULL)) {
		debug_msg("userhelper (cp): sending banner `%s'\n",
			  data->banner);
		send_request(data->output, UH_BANNER,
			     dgettext(data->domain, data->banner));
	}

#ifdef USE_STARTUP_NOTIFICATION
	/* SN Name. */
	if ((data->domain != NULL) && (data->sn_name != NULL)) {
		debug_msg("userhelper (cp): sending sn name `%s'\n",
			  data->sn_name);
		send_request(data->output, UH_SN_NAME,
			     dgettext(data->domain, data->sn_name));
	}

	/* SN Description. */
	if ((data->domain != NULL) && (data->sn_description != NULL)) {
		debug_msg("userhelper (cp): sending sn description `%s'\n",
			  data->sn_description);
		send_request(data->output, UH_SN_DESCRIPTION,
			     dgettext(data->domain, data->sn_description));
	}

	/* SN WM Class. */
	if ((data->domain != NULL) && (data->sn_wmclass != NULL)) {
		debug_msg("userhelper (cp): sending sn wm_class `%s'\n",
			  data->sn_wmclass);
		send_request(data->output, UH_SN_WMCLASS,
			     dgettext(data->domain, data->sn_wmclass));
	}

	/* SN BinaryName. */
	if ((data->domain != NULL) && (data->sn_binary_name != NULL)) {
		debug_msg("userhelper (cp): sending sn binary name `%s'\n",
			  data->sn_binary_name);
		send_request(data->output, UH_SN_BINARY_NAME,
			     dgettext(data->domain, data->sn_binary_name));
	}

	/* SN IconName. */
	if ((data->domain != NULL) && (data->sn_icon_name != NULL)) {
		debug_msg("userhelper (cp): sending sn icon name `%s'\n",
			  data->sn_icon_name);
		send_request(data->output, UH_SN_ICON_NAME,
			     dgettext(data->domain, data->sn_icon_name));
	}

	/* SN Workspace. */
	if ((data->domain != NULL) && (data->sn_workspace != -1)) {
		debug_msg("userhelper (cp): sending sn workspace %d.\n",
			  data->sn_workspace);
		send_request_int(data->output, UH_SN_WORKSPACE,
				 data->sn_workspace);
	}
#endif

	/* We do a first pass on all items and output them, and then a second
	 * pass to read responses from the helper. */
	for (count = expected_responses = 0; count < num_msg; count++) {
		switch (msg[count]->msg_style) {
			case PAM_PROMPT_ECHO_ON:
				/* Spit out the prompt. */
				debug_msg("userhelper (cp): sending prompt "
					  "(echo on) = \"%s\".\n",
					  msg[count]->msg);
				send_request(data->output, UH_ECHO_ON_PROMPT,
					     msg[count]->msg);
				expected_responses++;
				break;
			case PAM_PROMPT_ECHO_OFF:
				debug_msg("userhelper (cp): sending prompt (no "
					  "echo) = \"%s\".\n", msg[count]->msg);
				send_request(data->output, UH_ECHO_OFF_PROMPT,
					     msg[count]->msg);
				expected_responses++;
				break;
			case PAM_TEXT_INFO:
				/* Text information strings are output
				 * verbatim. */
				debug_msg("userhelper (cp): sending text = "
					  "\"%s\".\n", msg[count]->msg);
				send_request(data->output, UH_INFO_MSG,
					     msg[count]->msg);
				break;
			case PAM_ERROR_MSG:
				/* Error message strings are output verbatim. */
				debug_msg("userhelper (cp): sending error = "
					  "\"%s\".\n", msg[count]->msg);
				send_request(data->output, UH_ERROR_MSG,
					     msg[count]->msg);
				break;
			default:
				/* Maybe the consolehelper can figure out what
				 * to do with this, because we sure can't. */
				debug_msg("userhelper (cp): sending ??? = "
					  "\"%s\".\n", msg[count]->msg);
				send_request(data->output, UH_UNKNOWN_PROMPT,
					     msg[count]->msg);
				break;
		}
	}

	/* Tell the consolehelper how many messages for which we expect to
	 * receive responses. */
	debug_msg("userhelper (cp): sending expected response count = %d.\n",
		  expected_responses);
	send_request_int(data->output, UH_EXPECT_RESP, expected_responses);

	/* Tell the consolehelper that we're ready for it to do its thing. */
	debug_msg("userhelper (cp): sending sync point.\n");
	send_request(data->output, UH_SYNC_POINT, NULL);
	fflush(NULL);

	/* Now, for the second pass, allocate space for the responses and read
	 * the answers back. */
	reply = calloc(num_msg, sizeof(*reply));
	data->fallback_chosen = FALSE;

	/* First, handle the items which don't require answers. */
	for (count = 0; count < num_msg; count++) {
		switch (msg[count]->msg_style) {
		case PAM_TEXT_INFO:
		case PAM_ERROR_MSG:
			/* Ignore it... */
			/* reply[count].resp = NULL; set by the calloc ()
			   above */
			reply[count].resp_retcode = PAM_SUCCESS;
			break;
		default:
			break;
		}
	}

	/* Now read responses until we hit a sync point or an EOF. */
	count = received_responses = 0;
	for (;;) {
		string = read_reply(data->input);

		/* If we got nothing, and we expected data, then we're done. */
		if ((string == NULL) &&
		    (received_responses < expected_responses)) {
			debug_msg("userhelper (cp): got %d responses, expected "
				  "%d\n", received_responses,
				  expected_responses);
			data->canceled = TRUE;
			free_reply(reply, count);
			return PAM_ABORT;
		}

		debug_msg("userhelper (cp): received string type %d, text "
			  "\"%s\".\n", string[0], string[0] ? string + 1 : "");

		/* If we hit a sync point, we're done. */
		if (string[0] == UH_SYNC_POINT) {
			debug_msg("userhelper (cp): received sync point\n");
			g_free(string);
			if (data->fallback_chosen) {
				debug_msg("userhelper (cp): falling back\n");
				free_reply(reply, count);
				return PAM_ABORT;
			}
			if (received_responses != expected_responses) {
				/* Whoa, not done yet! */
				debug_msg("userhelper (cp): got %d responses, "
					  "expected %d\n", received_responses,
					  expected_responses);
				free_reply(reply, count);
				return PAM_CONV_ERR;
			}
			/* Okay, we're done. */
			break;
		}

#ifdef USE_STARTUP_NOTIFICATION
		/* If we got a desktop startup ID, set it. */
		if (string[0] == UH_SN_ID) {
			const char *p;

			g_free(data->sn_id);
			for (p = string + 1; *p != '\0' || g_ascii_isspace(*p);
			     p++)
				;
			data->sn_id = g_strdup(p);
			g_free(string);
			debug_msg("userhelper (cp): startup id \"%s\"\n",
				  data->sn_id);
			continue;
		}
#endif

		/* If the user chose to abort, do so. */
		if (string[0] == UH_CANCEL) {
			data->canceled = TRUE;
			g_free(string);
			free_reply(reply, count);
			debug_msg("userhelper (cp): canceling with PAM_ABORT "
				  "(%d)\n", PAM_ABORT);
			return PAM_ABORT;
		}

		/* If the user chose to fallback, do so. */
		if (string[0] == UH_FALLBACK) {
			data->fallback_chosen = TRUE;
			g_free(string);
			debug_msg("userhelper (cp): will fall back\n");
			continue;
		}

		/* Find the first unanswered prompt. */
		while ((count < num_msg) &&
		       (msg[count]->msg_style != PAM_PROMPT_ECHO_ON) &&
		       (msg[count]->msg_style != PAM_PROMPT_ECHO_OFF)) {
			count++;
		}
		if (count >= num_msg) {
			/* Whoa, TMI! */
			debug_msg("userhelper (cp): got %d responses, expected "
				  "< %d\n", received_responses, num_msg);
			g_free(string);
			free_reply(reply, count);
			return PAM_CONV_ERR;
		}

		/* Save this response. */
		reply[count].resp = strdup(string + 1);
		g_free(string);
		if (reply[count].resp == NULL) {
			free_reply(reply, count);
			return PAM_BUF_ERR;
		}
		reply[count].resp_retcode = PAM_SUCCESS;
		debug_msg("userhelper (cp): got `%s'\n", reply[count].resp);
		count++;
		received_responses++;
	}

	/* Check that we got exactly the number of responses we were
	 * expecting. */
	if (received_responses != expected_responses) {
		/* Must be an error of some sort... */
		debug_msg("userhelper (cp): got %d responses, expected %d\n",
			  received_responses, expected_responses);
		free_reply(reply, count);
		return PAM_CONV_ERR;
	}

	/* Return successfully. */
	if (resp != NULL)
		*resp = reply;
	else
		free_reply(reply, count);
	return PAM_SUCCESS;
}

/* A conversation function which wraps the one provided by libpam_misc. */
static int
converse_console(int num_msg, const struct pam_message **msg,
		 struct pam_response **resp, void *appdata_ptr)
{
	static int banner = 0;
	const char *service = NULL, *user;
	char *text;
	struct app_data *data = appdata_ptr;
	struct pam_message **messages;
	int i, ret;

	get_pam_string_item(data->pamh, PAM_SERVICE, &service);
	user = NULL;
	get_pam_string_item(data->pamh, PAM_USER, &user);

	if (banner == 0) {
		if ((data->banner != NULL) && (data->domain != NULL)) {
			text = g_strdup_printf(dgettext(data->domain, data->banner));
		} else {
			if ((service != NULL) && (strlen(service) > 0)) {
				if (data->fallback_allowed) {
					text = g_strdup_printf(_("You are attempting to run \"%s\" which may benefit from administrative\nprivileges, but more information is needed in order to do so."), service);
				} else {
					text = g_strdup_printf(_("You are attempting to run \"%s\" which requires administrative\nprivileges, but more information is needed in order to do so."), service);
				}
			} else {
				if (data->fallback_allowed) {
					text = g_strdup_printf(_("You are attempting to run a command which may benefit from\nadministrative privileges, but more information is needed in order to do so."));
				} else {
					text = g_strdup_printf(_("You are attempting to run a command which requires administrative\nprivileges, but more information is needed in order to do so."));
				}
			}
		}
		if (text != NULL) {
			fprintf(stdout, "%s\n", text);
			fflush(stdout);
			g_free(text);
		}
		if (user != NULL && strlen(user) != 0) {
			fprintf(stdout, _("Authenticating as \"%s\""), user);
			putchar('\n');
			fflush(stdout);
		}
		banner++;
	}

	messages = g_malloc(num_msg * sizeof(*messages));
	for (i = 0; i < num_msg; i++) {
		messages[i] = g_malloc(sizeof(*(messages[i])));
		*(messages[i]) = *(msg[i]);
		if (msg[i]->msg != NULL)
			messages[i]->msg = _(msg[i]->msg);
	}

	ret = misc_conv(num_msg, (const struct pam_message **)messages,
			resp, appdata_ptr);

	for (i = 0; i < num_msg; i++)
		g_free(messages[i]);
	g_free(messages);

	return ret;
}

/* A mixed-mode libuser prompter callback. */
static gboolean
prompt_pipe(struct lu_prompt *prompts, int prompts_count,
	    gpointer callback_data, struct lu_error **error)
{
	int i;
	char *string;
	const char *user, *service;
	struct app_data *data = callback_data;

	/* Pass on any hints we have to the consolehelper. */
	send_request_int(data->output, UH_FALLBACK_ALLOW,
			 data->fallback_allowed ? 1 : 0);

	/* User. */
	if ((get_pam_string_item(data->pamh, PAM_USER, &user) != PAM_SUCCESS) ||
	    (user == NULL) ||
	    (strlen(user) == 0)) {
		user = "root";
	}
	debug_msg("userhelper: sending user `%s'\n", user);
	send_request(data->output, UH_USER, user);

	/* Service. */
	if (get_pam_string_item(data->pamh, PAM_SERVICE,
				&service) == PAM_SUCCESS) {
		send_request(data->output, UH_SERVICE_NAME, service);
	}

	/* We do first a pass on all items and output them, and then a second
	 * pass to read responses from the helper. */
	for (i = 0; i < prompts_count; i++) {
		/* Spit out the prompt. */
		if (prompts[i].default_value) {
			send_request(data->output, UH_PROMPT_SUGGESTION,
				     prompts[i].default_value);
		}
		send_request(data->output, prompts[i].visible
			     ? UH_ECHO_ON_PROMPT : UH_ECHO_OFF_PROMPT,
			     prompts[i].prompt);
	}

	/* Tell the consolehelper how many messages we expect to get
	 * responses to. */
	send_request_int(data->output, UH_EXPECT_RESP, prompts_count);
	send_request(data->output, UH_SYNC_POINT, NULL);
	fflush(NULL);

	/* Now, for the second pass, allocate space for the responses and read
	 * the answers back. */
	i = 0;
	for (;;) {
		string = read_reply(data->input);

		if (string == NULL) {
			/* EOF: the child isn't going to give us any more
			 * information. */
			data->canceled = TRUE;
			lu_error_new(error, lu_error_generic,
				     "Operation canceled by user");
			goto err_prompts;
		}

		/* If we finished, we're done. */
		if (string[0] == UH_SYNC_POINT) {
			g_free(string);
			if (i < prompts_count) {
				/* Not enough information. */
				debug_msg("userhelper: not enough responses\n");
				lu_error_new(error, lu_error_generic,
					     "Not enough responses returned by "
					     "parent");
				goto err_prompts;
			}
			return TRUE;
		}

		/* If the user chose to abort, do so. */
		if (string[0] == UH_CANCEL) {
			debug_msg("userhelper: user canceled\n");
			g_free(string);
			data->canceled = TRUE;
			lu_error_new(error, lu_error_generic,
				     "Operation canceled by user");
			goto err_prompts;
		}

		/* If the user chose to fallback, do so. */
		if (string[0] == UH_FALLBACK) {
			debug_msg("userhelper: user fell back\n");
			g_free(string);
			data->fallback_chosen = TRUE;
			lu_error_new(error, lu_error_generic,
				     "User has decided to use unprivileged "
				     "mode");
			goto err_prompts;
		}

		/* Save this response. */
		prompts[i].free_value = g_free;
		prompts[i].value = g_strdup(string + 1);
		debug_msg("userhelper: got `%s'\n", prompts[i].value);
		g_free(string);
		i++;
	}

err_prompts:
	while (i != 0) {
		prompts[i].free_value(prompts[i].value);
		i--;
	}
	return FALSE;
}

/* A sync point is expected on the input pipe.  Wait until it arrives. */
static int
pipe_conv_wait_for_sync(struct app_data *data)
{
	char *reply;
	int err;

	reply = read_reply(data->input);
	if (reply == NULL) {
		err = ERR_UNK_ERROR;
		goto err;
	}
	if (reply[0] != UH_SYNC_POINT) {
		err = ERR_UNK_ERROR;
		goto err_reply;
	}
	err = 0;
	/* Fall through */
err_reply:
	g_free(reply);
err:
	return err;
}

static int
pipe_conv_exec_start(const struct pam_conv *conv)
{
	if (conv->conv == converse_pipe) {
		struct app_data *data;

		data = conv->appdata_ptr;
		send_request(data->output, UH_EXEC_START, NULL);
		send_request(data->output, UH_SYNC_POINT, NULL);
		fflush(data->output);
#ifdef DEBUG_USERHELPER
		{
			int timeout = 5;
			debug_msg("userhelper: exec start\nuserhelper: pausing "
				  "for %d seconds for debugging\n", timeout);
			sleep(timeout);
		}
#endif
		return pipe_conv_wait_for_sync(data);
	}
	return 0;
}

static void
pipe_conv_exec_fail(const struct pam_conv *conv)
{
	if (conv->conv == converse_pipe) {
		struct app_data *data;

		data = conv->appdata_ptr;
		debug_msg("userhelper: exec failed\n");
		send_request(data->output, UH_EXEC_FAILED, NULL);
		send_request(data->output, UH_SYNC_POINT, NULL);
		fflush(data->output);
		/* It is important to keep the parent in sync with our state,
		   even though there is no reliable way to inform it if this
		   fails. */
		(void)pipe_conv_wait_for_sync(data);
	}
}

/* Parse the passed-in GECOS string and set PARSED to its broken-down contents.
   Note that the parsing is performed using the convention obeyed by BSDish
   finger(1) under Linux. */
static void
gecos_parse(const char *gecos, struct gecos_data *parsed)
{
	char **exploded, **dest;
	int i;

	if (gecos == NULL) {
		return;
	}
	exploded = g_strsplit(gecos, ",", 5);

	if (exploded == NULL) {
		return;
	}

	for (i = 0; exploded[i] != NULL; i++) {
		dest = NULL;
		switch (i) {
			case 0:
				dest = &parsed->full_name;
				break;
			case 1:
				dest = &parsed->office;
				break;
			case 2:
				dest = &parsed->office_phone;
				break;
			case 3:
				dest = &parsed->home_phone;
				break;
			case 4:
				dest = &parsed->site_info;
				break;
			default:
				g_assert_not_reached();
				break;
		}
		if (dest != NULL) {
			*dest = g_strdup(exploded[i]);
		}
	}

	g_strfreev(exploded);
}

/* A simple function to compute the size of a gecos string containing the
 * data we have. */
static size_t
gecos_size(struct gecos_data *parsed)
{
	size_t len;

	len = 4; /* commas! */
	if (parsed->full_name != NULL) {
		len += strlen(parsed->full_name);
	}
	if (parsed->office != NULL) {
		len += strlen(parsed->office);
	}
	if (parsed->office_phone != NULL) {
		len += strlen(parsed->office_phone);
	}
	if (parsed->home_phone != NULL) {
		len += strlen(parsed->home_phone);
	}
	if (parsed->site_info != NULL) {
		len += strlen(parsed->site_info);
	}
	len++;

	return len;
}

/* Assemble a new gecos string. */
static char *
gecos_assemble(struct gecos_data *parsed)
{
	char *ret;
	size_t i;

	/* Construct the basic version of the string. */
	ret = g_strdup_printf("%s,%s,%s,%s,%s",
			      parsed->full_name ?: "",
			      parsed->office ?: "",
			      parsed->office_phone ?: "",
			      parsed->home_phone ?: "",
			      parsed->site_info ?: "");
	/* Strip off terminal commas. */
	i = strlen(ret);
	while ((i > 0) && (ret[i - 1] == ',')) {
		ret[i - 1] = '\0';
		i--;
	}
	return ret;
}

/* Free GECOS */
static void
gecos_free(struct gecos_data *gecos)
{
	g_free(gecos->full_name);
	g_free(gecos->office);
	g_free(gecos->office_phone);
	g_free(gecos->home_phone);
	g_free(gecos->site_info);
}

/* Check if the passed-in shell is a valid shell according to getusershell(),
 * which is usually back-ended by /etc/shells.  Treat NULL or the empty string
 * as "/bin/sh", as is traditional. */
static gboolean
shell_valid(const char *shell_name)
{
	gboolean found;

	found = FALSE;
	if (shell_name != NULL) {
		char *shell;

		if (strlen(shell_name) == 0)
			shell_name = "/bin/sh";
		setusershell();
		while ((shell = getusershell()) != NULL) {
			debug_msg("userhelper: got shell \"%s\"\n", shell);
			if (strcmp(shell_name, shell) == 0) {
				found = TRUE;
				break;
			}
		}
		endusershell();
	}
	return found;
}

/* checks if username is a member of groupname */
static gboolean
is_group_member(const char *username, const char * groupname)
{
	char **mem;
	struct group *gr = getgrnam(groupname);
	struct passwd *pw = getpwnam(username);
	
	if (pw !=NULL && gr != NULL) {
		if (gr->gr_gid == pw->pw_gid) return TRUE;
		for (mem = gr->gr_mem; *mem != NULL; mem++) {
			if (strcmp(*mem, username) == 0) return TRUE;
		}
	}
	return FALSE;
} 

/* checks if username is a member of any of a comma-separated list of groups */
static gboolean
is_grouplist_member(const char *username, const char * grouplist)
{
	char **grouparray;
	gboolean retval = FALSE;
	
	if (grouplist != NULL) {
		grouparray = g_strsplit(grouplist, ",", -1);
		int i;
		for (i = 0; grouparray[i] != NULL; i++) {
			g_strstrip(grouparray[i]);
			if (is_group_member(username, grouparray[i])) {
				retval = TRUE;
				break;
			}
		}
		g_strfreev(grouparray);
	}
	
	return retval;
}

static void
become_super(void)
{
	/* Become the superuser. */
	setgroups(0, NULL);
	setregid(0, 0);
	setreuid(0, 0);
	/* Yes, setuid() and friends can fail, even for superusers. */
	if ((geteuid() != 0) ||
	    (getuid() != 0) ||
	    (getegid() != 0) ||
	    (getgid() != 0)) {
		debug_msg("userhelper: set*id() failure: %s\n",
			  strerror(errno));
		exit(ERR_EXEC_FAILED);
	}
}

static void
become_normal(const char *user)
{
	/* Join the groups of the user who invoked us. */
	initgroups(user, getgid());
	/* Verify that we're back to normal. */
	if (getegid() != getgid()) {
		debug_msg("userhelper: still setgid()\n");
		exit(ERR_EXEC_FAILED);
	}
	/* Become the user who invoked us. */
	setreuid(getuid(), getuid());
	/* Yes, setuid() can fail. */
	if (geteuid() != getuid()) {
		debug_msg("userhelper: still setuid()\n");
		exit(ERR_EXEC_FAILED);
	}
}

/* Determine the name of the user who ran userhelper. */
static char *
get_invoking_user(void)
{
	struct passwd *pwd;
	char *ret=NULL;

	/* Now try to figure out who called us. */
	pwd = getpwuid(getuid());
	if ((pwd != NULL) && (pwd->pw_name != NULL)) {
		ret = g_strdup(pwd->pw_name);
	} else {
		/* I have no name and I must have one. */
		debug_msg("userhelper: i have no name\n");
		exit(ERR_UNK_ERROR);
	}

	debug_msg("userhelper: ruid user = '%s'\n", ret);

	return ret;
}

/* Determine the name of the user as whom we must authenticate. */
static char *
get_user_for_auth(shvarFile *s)
{
	char *ret;
	char *invoking_user, *configured_user, *configured_asusergroups;

	invoking_user = get_invoking_user();

	ret = NULL;

	if (ret == NULL) {
		/* Determine who we should authenticate as.  If not specified,
		 * or if "<user>" is specified, or if UGROUPS is set and the
		 * invoking user is a member, we authenticate as the invoking
		 * user, otherwise we authenticate as the specified user (which
		 * is usually root, but could conceivably be someone else). */
		configured_user = svGetValue(s, "USER");
		configured_asusergroups = svGetValue(s, "UGROUPS");
		if (configured_user == NULL) {
			ret = invoking_user;
		} else
		if (strcmp(configured_user, "<user>") == 0) {
			g_free(configured_user);
			ret = invoking_user;
		} else if (configured_asusergroups != NULL) {
			if (is_grouplist_member(invoking_user, configured_asusergroups)) {
				g_free(configured_user);
				ret = invoking_user;
			} else
				ret = configured_user;
		} else if (strcmp(configured_user, "<none>") == 0) {
			exit(ERR_NO_RIGHTS);
		} else
			ret = configured_user;
		g_free(configured_asusergroups);
	}

	if (ret != NULL) {
		if (invoking_user != ret)
			g_free(invoking_user);
		debug_msg("userhelper: user for auth = '%s'\n", ret);
		return ret;
	}

	debug_msg("userhelper: user for auth not known\n");
	return NULL;
}

/* Change the user's password using the indicated conversation function and
 * application data (which includes the ability to cancel if the user requests
 * it.  For this task, we don't retry on failure. */
static void G_GNUC_NORETURN
passwd(const char *user, struct pam_conv *conv)
{
	int retval;
	struct app_data *data;

	data = conv->appdata_ptr;
	retval = pam_start("passwd", user, conv, &data->pamh);
	if (retval != PAM_SUCCESS) {
		debug_msg("userhelper: pam_start() failed\n");
		fail_exit(conv->appdata_ptr, retval);
	}

	retval = pam_set_item(data->pamh, PAM_RUSER, user);
	if (retval != PAM_SUCCESS) {
		debug_msg("userhelper: pam_set_item(PAM_RUSER) failed\n");
		fail_exit(conv->appdata_ptr, retval);
	}

	debug_msg("userhelper: changing password for \"%s\"\n", user);
	retval = pam_chauthtok(data->pamh, 0);
	debug_msg("userhelper: PAM retval = %d (%s)\n", retval,
		  pam_strerror(data->pamh, retval));

	if (retval != PAM_SUCCESS) {
		debug_msg("userhelper: pam_chauthtok() failed\n");
		fail_exit(conv->appdata_ptr, retval);
	}

	retval = pam_end(data->pamh, PAM_SUCCESS);
	if (retval != PAM_SUCCESS) {
		debug_msg("userhelper: pam_end() failed\n");
		fail_exit(conv->appdata_ptr, retval);
	}
	exit(0);
}

/* We're here to change the user's non-security information.  PAM doesn't
 * provide an interface to do this, because it's not PAM's job to manage this
 * stuff, so farm it out to a different library. */
static void G_GNUC_NORETURN
chfn(const char *user, struct pam_conv *conv, lu_prompt_fn *prompt,
     const char *new_full_name, const char *new_office,
     const char *new_office_phone, const char *new_home_phone,
     const char *new_shell)
{
	char *new_gecos, *old_gecos, *old_shell;
	struct gecos_data parsed_gecos;
	const char *authed_user;
	struct lu_context *context;
	struct lu_ent *ent;
	struct lu_error *error;
	GValueArray *values;
	GValue *value, val;
	int tryagain = 3, retval;
	struct app_data *data;
	gboolean ret;

	debug_msg("userhelper: chfn(\"%s\", \"%s\", \"%s\", \"%s\", "
		  "\"%s\", \"%s\")\n", user,
		  new_full_name ? new_full_name : "(null)",
		  new_office ? new_office : "(null)",
		  new_office_phone ? new_office_phone : "(null)",
		  new_home_phone ? new_home_phone : "(null)",
		  new_shell ? new_shell : "(null)");

	/* Verify that the fields we were given on the command-line
	 * are sane (i.e., contain no forbidden characters). */
	if (new_full_name && strpbrk(new_full_name, ":,=")) {
		exit(ERR_FIELDS_INVALID);
	}
	if (new_office && strpbrk(new_office, ":,=")) {
		exit(ERR_FIELDS_INVALID);
	}
	if (new_office_phone && strpbrk(new_office_phone, ":,=")) {
		exit(ERR_FIELDS_INVALID);
	}
	if (new_home_phone && strpbrk(new_home_phone, ":,=")) {
		exit(ERR_FIELDS_INVALID);
	}

	/* Start up PAM to authenticate the user, this time pretending
	 * we're "chfn". */
	data = conv->appdata_ptr;
	retval = pam_start("chfn", user, conv, &data->pamh);
	if (retval != PAM_SUCCESS) {
		debug_msg("userhelper: pam_start() failed\n");
		fail_exit(conv->appdata_ptr, retval);
	}

	/* Set the requesting user. */
	retval = pam_set_item(data->pamh, PAM_RUSER, user);
	if (retval != PAM_SUCCESS) {
		debug_msg("userhelper: pam_set_item(PAM_RUSER) failed\n");
		fail_exit(conv->appdata_ptr, retval);
	}

	/* Try to authenticate the user. */
	do {
		debug_msg("userhelper: about to authenticate \"%s\"\n", user);
		retval = pam_authenticate(data->pamh, 0);
		debug_msg("userhelper: PAM retval = %d (%s)\n", retval,
			  pam_strerror(data->pamh, retval));
		tryagain--;
	} while ((retval != PAM_SUCCESS) &&
		 (retval != PAM_CONV_ERR) &&
		 !data->canceled &&
		 (tryagain > 0));
	/* If we didn't succeed, bail. */
	if (retval != PAM_SUCCESS) {
		debug_msg("userhelper: pam authentication failed\n");
		pam_end(data->pamh, retval);
		fail_exit(conv->appdata_ptr, retval);
	}

	/* Verify that the authenticated user is the user we started
	 * out trying to authenticate. */
	retval = get_pam_string_item(data->pamh, PAM_USER, &authed_user);
	if (retval != PAM_SUCCESS) {
		debug_msg("userhelper: no pam user set\n");
		pam_end(data->pamh, retval);
		fail_exit(conv->appdata_ptr, retval);
	}

	/* At some point this check will go away. */
	if (strcmp(user, authed_user) != 0) {
		debug_msg("userhelper: username(%s) != authuser(%s)\n",
			  user, authed_user);
		exit(ERR_UNK_ERROR);
	}

	/* Check if the user is allowed to change her information at
	 * this time, on this machine, yadda, yadda, yadda.... */
	retval = pam_acct_mgmt(data->pamh, 0);
	if (retval != PAM_SUCCESS) {
		debug_msg("userhelper: pam_acct_mgmt() failed\n");
		pam_end(data->pamh, retval);
		fail_exit(conv->appdata_ptr, retval);
	}

	/* Let's get to it.  Start up libuser. */
	error = NULL;
	context = lu_start(user, lu_user, NULL, NULL, prompt,
			   conv->appdata_ptr, &error);
	if (context == NULL) {
		debug_msg("userhelper: libuser startup error\n");
		pam_end(data->pamh, PAM_ABORT);
		fail_exit(conv->appdata_ptr, PAM_ABORT);
	}
	if (error != NULL) {
		debug_msg("userhelper: libuser startup error: %s\n",
			  lu_strerror(error));
		pam_end(data->pamh, PAM_ABORT);
		fail_exit(conv->appdata_ptr, PAM_ABORT);
	}

	/* Look up the user's record. */
	ent = lu_ent_new();
	ret = lu_user_lookup_name(context, user, ent, &error);
	if (ret != TRUE) {
		debug_msg("userhelper: libuser doesn't know the user \"%s\"\n",
			  user);
		pam_end(data->pamh, PAM_ABORT);
		fail_exit(conv->appdata_ptr, PAM_ABORT);
	}
	if (error != NULL) {
		debug_msg("userhelper: libuser doesn't know the user: %s\n",
			  lu_strerror(error));
		pam_end(data->pamh, PAM_ABORT);
		fail_exit(conv->appdata_ptr, PAM_ABORT);
	}

	/* Pull up the user's GECOS data, and split it up. */
	memset(&parsed_gecos, 0, sizeof(parsed_gecos));
	values = lu_ent_get(ent, LU_GECOS);
	if (values != NULL) {
		value = g_value_array_get_nth(values, 0);
		old_gecos = lu_value_strdup(value);
		gecos_parse(old_gecos, &parsed_gecos);
		debug_msg("userhelper: old gecos string \"%s\"\n", old_gecos);
		debug_msg("userhelper: old gecos \"'%s','%s','%s','%s','%s'\""
			  "\n", parsed_gecos.full_name, parsed_gecos.office,
			  parsed_gecos.office_phone, parsed_gecos.home_phone,
			  parsed_gecos.site_info);
		g_free(old_gecos);
	}

	/* Override any new values we have. */
	if (new_full_name != NULL) {
		g_free(parsed_gecos.full_name);
		parsed_gecos.full_name = g_strdup(new_full_name);
	}
	if (new_office != NULL) {
		g_free(parsed_gecos.office);
		parsed_gecos.office = g_strdup(new_office);
	}
	if (new_office_phone != NULL) {
		g_free(parsed_gecos.office_phone);
		parsed_gecos.office_phone = g_strdup(new_office_phone);
	}
	if (new_home_phone != NULL) {
		g_free(parsed_gecos.home_phone);
		parsed_gecos.home_phone = g_strdup(new_home_phone);
	}
	debug_msg("userhelper: new gecos \"'%s','%s','%s','%s','%s'\"\n",
		  parsed_gecos.full_name ? parsed_gecos.full_name : "(null)",
		  parsed_gecos.office ? parsed_gecos.office : "(null)",
		  parsed_gecos.office_phone ? parsed_gecos.office_phone
		  : "(null)",
		  parsed_gecos.home_phone ? parsed_gecos.home_phone : "(null)",
		  parsed_gecos.site_info ? parsed_gecos.site_info : "(null)");

	/* Verify that the strings we got passed are not too long. */
	if (gecos_size(&parsed_gecos) > GECOS_LENGTH) {
		debug_msg("userhelper: user gecos too long %d > %d\n",
			  gecos_size(&parsed_gecos), GECOS_LENGTH);
		lu_ent_free(ent);
		lu_end(context);
		pam_end(data->pamh, PAM_ABORT);
		exit(ERR_FIELDS_INVALID);
	}

	/* Build a new value for the GECOS data. */
	new_gecos = gecos_assemble(&parsed_gecos);
	debug_msg("userhelper: new gecos string \"%s\"\n", new_gecos);

	/* We don't need the user's current GECOS anymore, so clear
	 * out the value and set our own in the in-memory structure. */
	memset(&val, 0, sizeof(val));
	g_value_init(&val, G_TYPE_STRING);

	lu_ent_clear(ent, LU_GECOS);
	g_value_set_string(&val, new_gecos);
	lu_ent_add(ent, LU_GECOS, &val);
	g_free(new_gecos);

	/* While we're at it, set the individual data items as well. */
	lu_ent_clear(ent, LU_COMMONNAME);
	g_value_set_string(&val, parsed_gecos.full_name);
	lu_ent_add(ent, LU_COMMONNAME, &val);

	lu_ent_clear(ent, LU_ROOMNUMBER);
	g_value_set_string(&val, parsed_gecos.office);
	lu_ent_add(ent, LU_ROOMNUMBER, &val);

	lu_ent_clear(ent, LU_TELEPHONENUMBER);
	g_value_set_string(&val, parsed_gecos.office_phone);
	lu_ent_add(ent, LU_TELEPHONENUMBER, &val);

	lu_ent_clear(ent, LU_HOMEPHONE);
	g_value_set_string(&val, parsed_gecos.home_phone);
	lu_ent_add(ent, LU_HOMEPHONE, &val);

	gecos_free(&parsed_gecos);

	/* If we're here to change the user's shell, too, do that while we're
	 * in here, assuming that chsh and chfn have identical PAM
	 * configurations. */
	if (new_shell != NULL) {
		/* Check that the user's current shell is valid, and that she
		 * is not attempting to change to an invalid shell. */
		values = lu_ent_get(ent, LU_LOGINSHELL);
		if (values != NULL) {
			value = g_value_array_get_nth(values, 0);
			old_shell = lu_value_strdup(value);
		} else {
			old_shell = g_strdup("/bin/sh");
		}

		debug_msg("userhelper: current shell \"%s\"\n", old_shell);
		debug_msg("userhelper: new shell \"%s\"\n", new_shell);
		/* If the old or new shell are invalid, then
		 * the user doesn't get to make the change. */
		if (!shell_valid(new_shell) || !shell_valid(old_shell)) {
			debug_msg("userhelper: bad shell value\n");
			lu_ent_free(ent);
			lu_end(context);
			pam_end(data->pamh, PAM_ABORT);
			fail_exit(conv->appdata_ptr, ERR_SHELL_INVALID);
		}

		/* Set the shell to the new value. */
		lu_ent_clear(ent, LU_LOGINSHELL);
		g_value_set_string(&val, new_shell);
		lu_ent_add(ent, LU_LOGINSHELL, &val);
	}

	/* Save the changes to the user's account to the password
	 * database, whereever that is. */
	ret = lu_user_modify(context, ent, &error);
	if (ret != TRUE) {
		lu_ent_free(ent);
		lu_end(context);
		pam_end(data->pamh, PAM_ABORT);
		debug_msg("userhelper: libuser save failed\n");
		fail_exit(conv->appdata_ptr, PAM_ABORT);
	}
	if (error != NULL) {
		debug_msg("userhelper: libuser save error: %s\n",
			  lu_strerror(error));
		lu_ent_free(ent);
		lu_end(context);
		pam_end(data->pamh, PAM_ABORT);
		fail_exit(conv->appdata_ptr, PAM_ABORT);
	}

	lu_ent_free(ent);
	lu_end(context);
	_exit(0);
}

static char *
construct_cmdline(const char *argv0, char **argv)
{
	char *ret, *tmp;

	if (argv == NULL || argv[0] == NULL)
		return NULL;
	tmp = g_strjoinv(" ", argv + 1);
	ret = g_strconcat(argv0, " ", tmp, NULL);
	g_free(tmp);
	return ret;
}

static void G_GNUC_NORETURN
wrap(const char *user, const char *program,
     struct pam_conv *conv, struct pam_conv *text_conv, lu_prompt_fn *prompt,
     int argc, char **argv)
{
	/* We're here to wrap the named program.  After authenticating as the
	 * user given in the console.apps configuration file, execute the
	 * command given in the console.apps file. */
	char *constructed_path;
	char *apps_filename;
	char *user_pam;
	const char *auth_user;
	char *val;
	char **environ_save, **keep_env_names, **keep_env_values;
	const char *env_home, *env_term, *env_desktop_startup_id;
	const char *env_display, *env_shell;
	const char *env_lang, *env_language, *env_lcall, *env_lcmsgs;
	const char *env_xauthority;
	int session, tryagain, gui, retval;
	struct stat sbuf;
	struct passwd *pwd;
	struct app_data *data;
	shvarFile *s;

	(void)prompt;
	/* Find the basename of the command we're wrapping. */
	if (strrchr(program, '/')) {
		program = strrchr(program, '/') + 1;
	}

	/* Open the console.apps configuration file for this wrapped program,
	 * and read settings from it. */
	apps_filename = g_strconcat(SYSCONFDIR "/security/console.apps/",
				    program, NULL);
	s = svNewFile(apps_filename);

	/* If the file is world-writable, or isn't a regular file, or couldn't
	 * be opened, just exit.  We don't want to alert an attacker that the
	 * service name is invalid. */
	if ((s == NULL) ||
	    (fstat(s->fd, &sbuf) == -1) ||
	    !S_ISREG(sbuf.st_mode) ||
	    (sbuf.st_mode & S_IWOTH)) {
		debug_msg("userhelper: bad file permissions: %s \n",
			  apps_filename);
		exit(ERR_UNK_ERROR);
	}
	g_free(apps_filename);

	/* Save some of the current environment variables, because the
	 * environment is going to be nuked shortly. */
	env_desktop_startup_id = getenv("DESKTOP_STARTUP_ID");
	env_display = getenv("DISPLAY");
	env_home = getenv("HOME");
	env_lang = getenv("LANG");
	env_language = getenv("LANGUAGE");
	env_lcall = getenv("LC_ALL");
	env_lcmsgs = getenv("LC_MESSAGES");
	env_shell = getenv("SHELL");
	env_term = getenv("TERM");
	env_xauthority = getenv("XAUTHORITY");

	/* Sanity-check the environment variables as best we can: those
	 * which aren't path names shouldn't contain "/", and none of
	 * them should contain ".." or "%". */
	if (env_display &&
	    (strstr(env_display, "..") ||
	     strchr(env_display, '%')))
		env_display = NULL;
	if (env_home &&
	    (strstr(env_home, "..") ||
	     strchr(env_home, '%')))
		env_home = NULL;
	if (env_lang &&
	    (strstr(env_lang, "/") ||
	     strstr(env_lang, "..") ||
	     strchr(env_lang, '%')))
		env_lang = NULL;
	if (env_language &&
	    (strstr(env_language, "/") ||
	     strstr(env_language, "..") ||
	     strchr(env_language, '%')))
		env_language = NULL;
	if (env_lcall &&
	    (strstr(env_lcall, "/") ||
	     strstr(env_lcall, "..") ||
	     strchr(env_lcall, '%')))
		env_lcall = NULL;
	if (env_lcmsgs &&
	    (strstr(env_lcmsgs, "/") ||
	     strstr(env_lcmsgs, "..") ||
	     strchr(env_lcmsgs, '%')))
		env_lcmsgs = NULL;
	if (env_shell &&
	    (strstr(env_shell, "..") ||
	     strchr(env_shell, '%')))
		env_shell = NULL;
	if (env_term &&
	    (strstr(env_term, "..") ||
	     strchr(env_term, '%')))
		env_term = "dumb";
	if (env_xauthority &&
	    (strstr(env_xauthority , "..") ||
	     strchr(env_xauthority , '%')))
		env_xauthority = NULL;

	val = svGetValue(s, "KEEP_ENV_VARS");
	if (val != NULL) {
		size_t i, num_names;

		keep_env_names = g_strsplit(val, ",", -1);
		g_free(val);
		num_names = g_strv_length(keep_env_names);
		keep_env_values = g_malloc0(num_names
					    * sizeof (*keep_env_values));
		for (i = 0; i < num_names; i++)
			/* g_strdup(NULL) is defined to be NULL. */
			keep_env_values[i]
				= g_strdup(getenv(keep_env_names[i]));
	} else {
		keep_env_names = NULL;
		keep_env_values = NULL;
	}

	/* Wipe out the current environment. */
	environ_save = environ;
	environ = g_malloc0(2 * sizeof(char *));

	/* Set just the environment variables we can trust.  Note that
	 * XAUTHORITY is not initially copied -- we don't want to let attackers
	 * get at others' X authority records -- we restore XAUTHORITY below
	 * *after* successfully authenticating, or abandoning authentication in
	 * order to run the wrapped program as the invoking user. */
	if (env_display) setenv("DISPLAY", env_display, 1);

	/* The rest of the environment variables are simpler. */
	if (env_desktop_startup_id) setenv("DESKTOP_STARTUP_ID",
					   env_desktop_startup_id, 1);
	if (env_lang) setenv("LANG", env_lang, 1);
	if (env_language) setenv("LANGUAGE", env_language, 1);
	if (env_lcall) setenv("LC_ALL", env_lcall, 1);
	if (env_lcmsgs) setenv("LC_MESSAGES", env_lcmsgs, 1);
	if (env_shell) setenv("SHELL", env_shell, 1);
	if (env_term) setenv("TERM", env_term, 1);

	/* Set the PATH to a reasonaly safe list of directories. */
	setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin:/root/bin", 1);

	/* Set the LOGNAME and USER variables to the executing name. */
	setenv("LOGNAME", "root", 1);
	setenv("USER", "root", 1);

	/* Handle KEEP_ENV_VARS only after setting most of the variables above.
	   This lets the config file to request keeping the value of an
	   environment variable even if it would be otherwise overridden (e.g.
	   PATH). */
	if (keep_env_names != NULL) {
		size_t i;

		for (i = 0; keep_env_names[i] != NULL; i++) {
			if (keep_env_values[i] != NULL) {
				setenv(keep_env_names[i], keep_env_values[i],
				       1);
				g_free(keep_env_values[i]);
			}
		}
		g_strfreev(keep_env_names);
		g_free(keep_env_values);
	}

	/* Pass the original UID to the new program */
	setenv("USERHELPER_UID", g_strdup_printf("%jd", (intmax_t)getuid()), 1);

	data = conv->appdata_ptr;
	user_pam = get_user_for_auth(s);

	/* Read the path to the program to run. */
	constructed_path = svGetValue(s, "PROGRAM");
	if (!constructed_path || constructed_path[0] != '/') {
		g_free(constructed_path);
		/* Criminy....  The system administrator didn't give us an
		 * absolute path to the program!  Guess either /usr/sbin or
		 * /sbin, and then give up if we don't find anything by that
		 * name in either of those directories.  FIXME: we're a setuid
		 * app, so access() may not be correct here, as it may give
		 * false negatives.  But then, it wasn't an absolute path. */
		constructed_path = g_strconcat("/usr/sbin/", program, NULL);
		if (access(constructed_path, X_OK) != 0) {
			/* Try the second directory. */
			strcpy(constructed_path, "/sbin/");
			strcat(constructed_path, program);
			if (access(constructed_path, X_OK)) {
				/* Nope, not there, either. */
				debug_msg("userhelper: couldn't find wrapped "
					  "binary\n");
				exit(ERR_NO_PROGRAM);
			}
		}
	}


	/* We can forcefully disable the GUI from the configuration
	 * file (a la blah-nox applications). */
	gui = svTrueValue(s, "GUI", TRUE);
	/* We can use a magic configuration file option to disable
	 * the GUI, too. */
	if (gui) {
		val = svGetValue(s, "NOXOPTION");
		if (val != NULL && strlen(val) > 1) {
			int i;

			for (i = optind; i < argc; i++) {
				if (strcmp(argv[i], val) == 0) {
					gui = FALSE;
					break;
				}
			}
		}
		g_free(val);
	}

	if (!gui) {
		/* We are not really executing anything yet, but this switches
		   off the parent to a "pass exit code through" mode without
		   displaying any unwanted GUI dialogs. */
		retval = pipe_conv_exec_start(conv);
		if (retval != 0)
			exit(retval);
		conv = text_conv;
	}

	/* Verify that the user we need to authenticate as has a home
	 * directory. */
	pwd = getpwnam(user_pam);
	if (pwd == NULL) {
		debug_msg("userhelper: no user named %s exists\n", user_pam);
		exit(ERR_NO_USER);
	}

	/* If the user we're authenticating as has root's UID, then it's
	 * safe to let them use HOME=~root. */
	if (pwd->pw_uid == 0)
		setenv("HOME", pwd->pw_dir, 1);
	else {
		/* Otherwise, if they had a reasonable value for HOME, let them
		 * use it. */
		if (env_home != NULL)
			setenv("HOME", env_home, 1);
		else {
			/* Otherwise, set HOME to the user's home directory. */
			pwd = getpwuid(getuid());
			if ((pwd != NULL) && (pwd->pw_dir != NULL))
				setenv("HOME", pwd->pw_dir, 1);
		}
	}

	/* Read other settings. */
	session = svTrueValue(s, "SESSION", FALSE);
	data->fallback_allowed = svTrueValue(s, "FALLBACK", FALSE);
	val = svGetValue(s, "RETRY"); /* default value is "2" */
	tryagain = val ? atoi(val) + 1 : 3;
	g_free(val);

	/* Read any custom messages we might want to use. */
	val = svGetValue(s, "BANNER");
	if (val != NULL && strlen(val) > 0)
		data->banner = val;
	val = svGetValue(s, "DOMAIN");
	if (val != NULL && strlen(val) > 0) {
		bindtextdomain(val, LOCALEDIR);
		data->domain = val;
	}
	if (data->domain == NULL) {
		val = svGetValue(s, "BANNER_DOMAIN");
		if (val != NULL && strlen(val) > 0) {
			bindtextdomain(val, LOCALEDIR);
			data->domain = val;
		}
	}
	if (data->domain == NULL) {
		data->domain = program;
	}
#ifdef USE_STARTUP_NOTIFICATION
	val = svGetValue(s, "STARTUP_NOTIFICATION_NAME");
	if (val != NULL && strlen(val) > 0)
		data->sn_name = val;
	val = svGetValue(s, "STARTUP_NOTIFICATION_DESCRIPTION");
	if (val != NULL && strlen(val) > 0)
		data->sn_description = val;
	val = svGetValue(s, "STARTUP_NOTIFICATION_WMCLASS");
	if (val != NULL && strlen(val) > 0)
		data->sn_wmclass = val;
	val = svGetValue(s, "STARTUP_NOTIFICATION_BINARY_NAME");
	if (val != NULL && strlen(val) > 0)
		data->sn_binary_name = val;
	val = svGetValue(s, "STARTUP_NOTIFICATION_ICON_NAME");
	if (val != NULL && strlen(val) > 0)
		data->sn_icon_name = val;
	val = svGetValue(s, "STARTUP_NOTIFICATION_WORKSPACE");
	if (val != NULL && strlen(val) > 0)
		data->sn_workspace = atoi(val);
#endif

	/* Now we're done reading the file. Close it. */
	svCloseFile(s);

	/* Start up PAM to authenticate the specified user. */
	retval = pam_start(program, user_pam, conv, &data->pamh);
	if (retval != PAM_SUCCESS) {
		debug_msg("userhelper: pam_start() failed\n");
		fail_exit(conv->appdata_ptr, retval);
	}

	/* Set the requesting user. */
	retval = pam_set_item(data->pamh, PAM_RUSER, user);
	if (retval != PAM_SUCCESS) {
		debug_msg("userhelper: pam_set_item(PAM_RUSER) failed\n");
		fail_exit(conv->appdata_ptr, retval);
	}

	/* Try to authenticate the user. */
	do {
		debug_msg("userhelper: authenticating \"%s\"\n", user_pam);
		retval = pam_authenticate(data->pamh, 0);
		debug_msg("userhelper: PAM retval = %d (%s)\n", retval,
			  pam_strerror(data->pamh, retval));
		tryagain--;
	} while ((retval != PAM_SUCCESS) && tryagain &&
		 !data->fallback_chosen && !data->canceled);

	if (retval != PAM_SUCCESS) {
		pam_end(data->pamh, retval);
		if (data->canceled) {
			fail_exit(conv->appdata_ptr, retval);
		} else
		if (data->fallback_allowed) {
			/* Reset the user's environment so that the
			 * application can run normally. */
			argv[optind - 1] = strdup(program);
			environ = environ_save;
			become_normal(user);
			if (data->input != NULL) {
				fflush(data->input);
				fcntl(UH_INFILENO, F_SETFD, FD_CLOEXEC);
			}
			if (data->output != NULL) {
				fflush(data->output);
				fcntl(UH_OUTFILENO, F_SETFD, FD_CLOEXEC);
			}
			retval = pipe_conv_exec_start(conv);
			if (retval != 0)
				exit(retval);
#ifdef USE_STARTUP_NOTIFICATION
			if (data->sn_id) {
				debug_msg("userhelper: setting "
					  "DESKTOP_STARTUP_ID =\"%s\"\n",
					  data->sn_id);
				setenv("DESKTOP_STARTUP_ID",
				       data->sn_id, 1);
			}
#endif
			execv(constructed_path, argv + optind - 1);
			pipe_conv_exec_fail(conv);
			exit(ERR_EXEC_FAILED);
		} else {
			/* Well, we tried. */
			fail_exit(conv->appdata_ptr, retval);
		}
	}

	/* Verify that the authenticated user is the user we started
	 * out trying to authenticate. */
	retval = get_pam_string_item(data->pamh, PAM_USER, &auth_user);
	if (retval != PAM_SUCCESS) {
		pam_end(data->pamh, retval);
		fail_exit(conv->appdata_ptr, retval);
	}
	if (strcmp(user_pam, auth_user) != 0) {
		exit(ERR_UNK_ERROR);
	}

	/* Verify that the authenticated user is allowed to run this
	 * service now. */
	retval = pam_acct_mgmt(data->pamh, 0);
	if (retval != PAM_SUCCESS) {
		pam_end(data->pamh, retval);
		fail_exit(conv->appdata_ptr, retval);
	}

	/* We need to re-read the user's information -- libpam doesn't
	 * guarantee that these won't be nuked. */
	pwd = getpwnam(user_pam);
	if (pwd == NULL) {
		debug_msg("userhelper: no user named %s exists\n", user_pam);
		exit(ERR_NO_USER);
	}

	/* What we do now depends on whether or not we need to open
	 * a session for the user. */
	if (session) {
		int child, status;

		/* We're opening a session, and that may included
		 * running graphical apps, so restore the XAUTHORITY
		 * environment variable. */
		if (env_xauthority) {
			setenv("XAUTHORITY", env_xauthority, 1);
		}

		/* Open a session. */
		retval = pam_open_session(data->pamh, 0);
		if (retval != PAM_SUCCESS) {
			pam_end(data->pamh, retval);
			fail_exit(conv->appdata_ptr, retval);
		}

		/* Start up a child process we can wait on. */
		child = fork();
		if (child == -1) {
			exit(ERR_EXEC_FAILED);
		}
		if (child == 0) {
			/* We're in the child.  Make a few last-minute
			 * preparations and exec the program. */
			char **env_pam;
			const char *cmdline;

			env_pam = pam_getenvlist(data->pamh);
			while (env_pam && *env_pam) {
				debug_msg("userhelper: setting %s\n", *env_pam);
				putenv(g_strdup(*env_pam));
				env_pam++;
			}

			argv[optind - 1] = strdup(program);
			debug_msg("userhelper: about to exec \"%s\"\n",
				  constructed_path);
			become_super();
			if (data->input != NULL) {
				fflush(data->input);
				fcntl(UH_INFILENO, F_SETFD, FD_CLOEXEC);
			}
			if (data->output != NULL) {
				fflush(data->output);
				fcntl(UH_OUTFILENO, F_SETFD, FD_CLOEXEC);
			}
			retval = pipe_conv_exec_start(conv);
			if (retval != 0)
				exit(retval);
#ifdef USE_STARTUP_NOTIFICATION
			if (data->sn_id) {
				debug_msg("userhelper: setting "
					  "DESKTOP_STARTUP_ID =\"%s\"\n",
					  data->sn_id);
				setenv("DESKTOP_STARTUP_ID",
				       data->sn_id, 1);
			}
#endif
			cmdline = construct_cmdline(constructed_path,
						    argv + optind - 1);
			debug_msg("userhelper: running '%s' with root "
				  "privileges on behalf of '%s'.\n", cmdline,
				  user);
			syslog(LOG_NOTICE, "running '%s' with "
			       "root privileges on behalf of '%s'",
			       cmdline, user);
			execv(constructed_path, argv + optind - 1);
			syslog(LOG_ERR, "could not run '%s' with "
			       "root privileges on behalf of '%s': %s",
			       cmdline, user, strerror(errno));
			pipe_conv_exec_fail(conv);
			exit(ERR_EXEC_FAILED);
		}
		/* We're in the parent.  Wait for the child to exit.  The child
		   is calling pipe_conv_exec_{start,fail} () to define the
		   semantics of its exit code. */
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);

		waitpid(child, &status, 0);

		/* Close the session. */
		retval = pam_close_session(data->pamh, 0);
		if (retval != PAM_SUCCESS) {
			pipe_conv_exec_fail(conv);
			pam_end(data->pamh, retval);
			fail_exit(conv->appdata_ptr, retval);
		}

		pam_end(data->pamh, PAM_SUCCESS);
		if (WIFEXITED(status))
			exit(WEXITSTATUS(status));
		if (WIFSIGNALED(status))
			exit(WTERMSIG(status) + 128);
		exit(255);
	} else {
		const char *cmdline;

		/* We're not opening a session, so we can just exec()
		 * the program we're wrapping. */
		pam_end(data->pamh, PAM_SUCCESS);

		argv[optind - 1] = strdup(program);
		debug_msg("userhelper: about to exec \"%s\"\n",
			  constructed_path);
		become_super();
		if (data->input != NULL) {
			fflush(data->input);
			fcntl(UH_INFILENO, F_SETFD, FD_CLOEXEC);
		}
		if (data->output != NULL) {
			fflush(data->output);
			fcntl(UH_OUTFILENO, F_SETFD, FD_CLOEXEC);
		}
		retval = pipe_conv_exec_start(conv);
		if (retval != 0)
			exit(retval);
#ifdef USE_STARTUP_NOTIFICATION
		if (data->sn_id) {
			debug_msg("userhelper: setting "
				  "DESKTOP_STARTUP_ID =\"%s\"\n", data->sn_id);
			setenv("DESKTOP_STARTUP_ID", data->sn_id, 1);
		}
#endif
		cmdline = construct_cmdline(constructed_path,
					    argv + optind - 1);
		debug_msg("userhelper: running '%s' with root privileges "
			  "on behalf of '%s'\n", cmdline, user);
		syslog(LOG_NOTICE, "running '%s' with "
		       "root privileges on behalf of '%s'",
		       cmdline, user);

		execv(constructed_path, argv + optind - 1);
		syslog(LOG_ERR, "could not run '%s' with "
		       "root privileges on behalf of '%s': %s",
		       cmdline, user, strerror(errno));
		pipe_conv_exec_fail(conv);
		exit(ERR_EXEC_FAILED);
	}
}

/*
 * ------- the application itself --------
 */
int
main(int argc, char **argv)
{
	int arg;
	char *wrapped_program = NULL;
	struct passwd *pwd;
	lu_prompt_fn *prompt;
	char *user_name; /* current user, as determined by real uid */
	int f_flag;	 /* -f flag = change full name */
	int o_flag;	 /* -o flag = change office name */
	int p_flag;	 /* -p flag = change office phone */
	int h_flag;	 /* -h flag = change home phone number */
	int c_flag;	 /* -c flag = change password */
	int s_flag;	 /* -s flag = change shell */
	int t_flag;	 /* -t flag = direct interactive text-mode -- exec'ed */
	int w_flag;	 /* -w flag = act as a wrapper for next * args */
	const char *new_full_name;
	const char *new_office;
	const char *new_office_phone;
	const char *new_home_phone;
	const char *new_shell;
#ifdef WITH_SELINUX
	unsigned perm;
#endif

	/* State variable we pass around. */
	struct app_data app_data = {
		NULL,
		FALSE, FALSE, FALSE,
		NULL, NULL,
		NULL, NULL,
#ifdef USE_STARTUP_NOTIFICATION
		NULL, NULL, NULL,
		NULL, NULL, NULL,
		-1,
#endif
	};

	/* PAM conversation structures containing the addresses of the
	 * various conversation functions and our state data. */
	struct pam_conv silent_conv = {
		silent_converse,
		&app_data,
	};
	struct pam_conv pipe_conv = {
		converse_pipe,
		&app_data,
	};
	struct pam_conv text_conv = {
		converse_console,
		&app_data,
	};
	struct pam_conv *conv;

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	openlog("userhelper", LOG_PID, LOG_AUTHPRIV);

	if (geteuid() != 0) {
		fprintf(stderr, _("userhelper must be setuid root\n"));
		debug_msg("userhelper: not setuid\n");
		exit(ERR_NO_RIGHTS);
	}

	f_flag = 0;
	o_flag = 0;
	p_flag = 0;
	h_flag = 0;
	c_flag = 0;
	s_flag = 0;
	t_flag = 0;
	w_flag = 0;
	new_full_name = NULL;
	new_office = NULL;
	new_office_phone = NULL;
	new_home_phone = NULL;
	new_shell = NULL;

	while ((w_flag == 0) &&
	       (arg = getopt(argc, argv, "f:o:p:h:s:ctw:")) != -1) {
		/* We process no arguments after -w program; those are passed
		 * on to a wrapped program. */
		switch (arg) {
			case 'f':
				/* Full name. */
				f_flag++;
				new_full_name = optarg;
				break;
			case 'o':
				/* Office. */
				o_flag++;
				new_office = optarg;
				break;
			case 'h':
				/* Home phone. */
				h_flag++;
				new_home_phone = optarg;
				break;
			case 'p':
				/* Office phone. */
				p_flag++;
				new_office_phone = optarg;
				break;
			case 's':
				/* Change shell flag. */
				s_flag++;
				new_shell = optarg;
				break;
			case 'c':
				/* Change password flag. */
				c_flag++;
				break;
			case 't':
				/* Text-mode flag. */
				t_flag++;
				break;
			case 'w':
				/* Wrap flag. */
				w_flag++;
				wrapped_program = optarg;
				break;
			default:
				debug_msg("userhelper: invalid call: "
					  "unknown option\n");
				exit(ERR_INVALID_CALL);
		}
	}

	/* Sanity-check the arguments a bit. */
#define SHELL_FLAGS (f_flag || o_flag || h_flag || p_flag || s_flag)
	if ((c_flag && SHELL_FLAGS) ||
	    (c_flag && w_flag) ||
	    (w_flag && SHELL_FLAGS)) {
		debug_msg("userhelper: invalid call: invalid combination of "
			  "options\n");
		exit(ERR_INVALID_CALL);
	}

	/* Determine which conversation function to use. */
	if (t_flag) {
		/* We were told to use text mode. */
		if (isatty(STDIN_FILENO)) {
			/* We have a controlling tty on which we can disable
			 * echoing, so use the text conversation method. */
			conv = &text_conv;
		} else {
			/* We have no controlling terminal -- being run from
			 * cron or some other mechanism? */
			conv = &silent_conv;
#if 0
			/* FIXME: print a warning here? */
			fprintf(stderr, _("Unable to open graphical window, "
				"and unable to find controlling terminal.\n"));
			_exit(0);
#endif
		}
		prompt = &lu_prompt_console;
	} else {
		/* Set up to use the GTK+ helper. */
		app_data.input = fdopen(UH_INFILENO, "r");
		app_data.output = fdopen(UH_OUTFILENO, "w");
		if ((app_data.input == NULL) || (app_data.output == NULL)) {
			debug_msg("userhelper: invalid call\n");
			exit(ERR_INVALID_CALL);
		}
		conv = &pipe_conv;
		prompt = &prompt_pipe;
	}

	user_name = get_invoking_user();
	debug_msg("userhelper: current user is %s\n", user_name);

	/* If we didn't get the -w flag, the last argument can be a user's
	 * name. */
	if (w_flag == 0) {
		if ((getuid() == 0) && (argc == optind + 1)) {
			/* We were started by the superuser, so accept the
			 * user's name. */
			g_free(user_name);
			user_name = g_strdup(argv[optind]);

#ifdef WITH_SELINUX
			if (c_flag) 
			  perm = PASSWD__PASSWD;
			else if (s_flag)
			  perm = PASSWD__CHSH;
			else
			  perm = PASSWD__CHFN;

			if (is_selinux_enabled() > 0 &&
			    checkAccess(perm)!= 0) {
				security_context_t context = NULL;
				getprevcon(&context);
				syslog(LOG_NOTICE, 
				       "SELinux context %s is not allowed to change information for user \"%s\"\n",
				       context, user_name);
				g_free(user_name);
				exit(ERR_NO_USER);
			}
#endif
			debug_msg("userhelper: modifying account data for %s\n",
				  user_name);
		}

		/* Verify that the user exists. */
		pwd = getpwnam(user_name);
		if ((pwd == NULL) || (pwd->pw_name == NULL)) {
			debug_msg("userhelper: user %s doesn't exist\n",
				  user_name);
			exit(ERR_NO_USER);
		}
	}

	/* Change password? */
	if (c_flag) {
		passwd(user_name, conv);
		g_assert_not_reached();
	}

	/* Change GECOS data or shell? */
	if (SHELL_FLAGS) {
		chfn(user_name, conv, prompt,
		     new_full_name, new_office,
		     new_office_phone, new_home_phone,
		     new_shell);
		g_assert_not_reached();
	}

	/* Wrap some other program? */
	if (w_flag) {
		wrap(user_name, wrapped_program, conv, &text_conv, prompt,
		     argc, argv);
		g_assert_not_reached();
	}

	/* Not reached. */
	g_assert_not_reached();
	exit(0);
}

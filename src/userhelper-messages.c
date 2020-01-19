/*
 * Copyright (C) 2011 Red Hat, Inc.
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
#include <glib/gi18n.h>
#include "userhelper.h"
#include "userhelper-messages.h"

static const struct {
	int status;		/* -1 = default, last entry */
	enum uh_message_type type;
	const char *message;
} msgs[] = {
	{ 0, UHM_MESSAGE, N_("Information updated.") },
	{
		ERR_PASSWD_INVALID, UHM_ERROR,
		N_("The password you typed is invalid.\n"
		   "Please try again.")
	},
	{
		ERR_FIELDS_INVALID, UHM_ERROR,
		N_("One or more of the changed fields is invalid.\n"
		   "This is probably due to either colons or commas in one of "
		   "the fields.\n"
		   "Please remove those and try again.")
	},
	{ ERR_SET_PASSWORD, UHM_ERROR, N_("Password resetting error.") },
	{
		ERR_LOCKS, UHM_ERROR,
		N_("Some systems files are locked.\n"
		   "Please try again in a few moments.")
	},
	{ ERR_NO_USER, UHM_ERROR, N_("Unknown user.") },
	{ ERR_NO_RIGHTS, UHM_ERROR, N_("Insufficient rights.") },
	{ ERR_INVALID_CALL, UHM_ERROR, N_("Invalid call to subprocess.") },
	{
		ERR_SHELL_INVALID, UHM_ERROR,
		N_("Your current shell is not listed in /etc/shells.\n"
		   "You are not allowed to change your shell.\n"
		   "Consult your system administrator.")
	},
	/* well, this is unlikely to work, but at least we tried... */
	{ ERR_NO_MEMORY, UHM_ERROR, N_("Out of memory.") },
	{ ERR_EXEC_FAILED, UHM_ERROR, N_("The exec() call failed.") },
	{ ERR_NO_PROGRAM, UHM_ERROR, N_("Failed to find selected program.") },
	/* special no-display dialog */
	{ ERR_CANCELED, UHM_SILENT, N_("Request canceled.") },
	{ ERR_PAM_INT_ERROR, UHM_ERROR, N_("Internal PAM error occured.") },
	{ ERR_MAX_TRIES, UHM_ERROR, N_("No more retries allowed") },
	{ ERR_UNK_ERROR, UHM_ERROR, N_("Unknown error.") },
	/* First entry is default */
	{ -1, UHM_ERROR, N_("Unknown exit code.") },
};

/* Convert ERR_* exit STATUS to a localized *MESSAGE and its *TYPE.
   Cannot fail. */
void
uh_exitstatus_message(int exit_status, const char **message,
		      enum uh_message_type *type)
{
	size_t i;

	for (i = 0; msgs[i].status != -1; i++) {
		if (msgs[i].status == exit_status)
			break;
	}

	*message = _(msgs[i].message);
	*type = msgs[i].type;
}

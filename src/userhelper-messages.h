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

#ifndef USERHELPER_MESSAGES_H__
#define USERHELPER_MESSAGES_H__

#include "config.h"

/* What kind of message is this? */
enum uh_message_type {
	UHM_MESSAGE,
	UHM_ERROR,
	UHM_SILENT
};

/* Convert ERR_* exit STATUS to a localized *MESSAGE and its *TYPE.
   Cannot fail. */
extern void
uh_exitstatus_message(int exit_status, const char **message,
		      enum uh_message_type *type);

#endif /* USERHELPER_MESSAGES_H__ */

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

#ifndef __USERHELPER_H__
#define __USERHELPER_H__

#include "config.h"

/* Descriptors used to communicate between userhelper and consolhelper. */
#define UH_INFILENO 3
#define UH_OUTFILENO 4

/* Userhelper request format:
   request code as a single character,
   request data size as UH_REQUEST_SIZE_DIGITS decimal digits
   request data
   '\n' */
#define UH_REQUEST_SIZE_DIGITS 8

/* Synchronization point code. */
#define UH_SYNC_POINT 32

/* Valid userhelper request codes. */
#define UH_UNKNOWN_PROMPT 33
#define UH_ECHO_ON_PROMPT 34
#define UH_ECHO_OFF_PROMPT 35
#define UH_PROMPT_SUGGESTION 36
#define UH_INFO_MSG 37
#define UH_ERROR_MSG 38
#define UH_EXPECT_RESP 39
#define UH_SERVICE_NAME 40
#define UH_FALLBACK_ALLOW 41
#define UH_USER 42
#define UH_BANNER 43
#define UH_EXEC_START 44
#define UH_EXEC_FAILED 45

#ifdef USE_STARTUP_NOTIFICATION
#define UH_SN_NAME 46
#define UH_SN_DESCRIPTION 47
#define UH_SN_WORKSPACE 48
#define UH_SN_WMCLASS 49
#define UH_SN_BINARY_NAME 50
#define UH_SN_ICON_NAME 51
#endif

/* Consolehelper response format:
   response code as a single character,
   response data
   '\n' */

/* Consolehelper response codes. */
#define UH_TEXT 33
#define UH_CANCEL 34
#define UH_FALLBACK 35
#ifdef USE_STARTUP_NOTIFICATION
#define UH_SN_ID 36
#endif

/* Valid userhelper error codes. */
#define ERR_PASSWD_INVALID      1       /* password is not right */
#define ERR_FIELDS_INVALID      2       /* gecos fields invalid or
                                         * sum(lengths) too big */
#define ERR_SET_PASSWORD        3       /* password resetting error */
#define ERR_LOCKS               4       /* some files are locked */
#define ERR_NO_USER             5       /* user unknown ... */
#define ERR_NO_RIGHTS           6       /* insufficient rights  */
#define ERR_INVALID_CALL        7       /* invalid call to this program */
#define ERR_SHELL_INVALID       8       /* invalid call to this program */
#define ERR_NO_MEMORY		9	/* out of memory */
#define ERR_NO_PROGRAM		10	/* -w progname not found */
#define ERR_EXEC_FAILED		11	/* exec failed for some reason */
#define ERR_CANCELED		12	/* user cancelled operation */
#define ERR_PAM_INT_ERROR	13	/* PAM internal error */
#define ERR_UNK_ERROR		255	/* unknown error */

/* Paths, flag names, and other stuff. */
#define UH_PATH SBINDIR "/userhelper"
#define UH_CONSOLEHELPER_PATH BINDIR "/consolehelper"
#define UH_CONSOLEHELPER_X11_PATH BINDIR "/consolehelper-gtk"
#define UH_PASSWD_OPT "-c"
#define UH_FULLNAME_OPT "-f"
#define UH_OFFICE_OPT "-o"
#define UH_OFFICEPHONE_OPT "-p"
#define UH_HOMEPHONE_OPT "-h"
#define UH_SHELL_OPT "-s"
#define UH_TEXT_OPT "-t"
#define UH_WRAP_OPT "-w"

#endif /* __USERHELPER_H__ */

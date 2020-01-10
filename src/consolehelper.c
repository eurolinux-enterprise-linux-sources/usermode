/*
 * Copyright (C) 1999-2003, 2007 Red Hat, Inc.  All rights reserved.
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
#include <sys/types.h>
#include <fcntl.h>
#include <libintl.h>
#include <locale.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef DISABLE_X11
#include <gtk/gtk.h>
#endif
#include "userhelper.h"
#include "userhelper-wrap.h"

int
main(int argc, char *argv[])
{
	char **constructed_argv;
	int offset, i;
	char *progname;
	gboolean graphics_available = FALSE;
#ifndef DISABLE_X11
	char *display;
#endif

#ifdef DISABLE_X11
	/* We're in the non-X11 version.  If we have the X11-capable version
	 * installed, try to let it worry about all of this. */
	execv(UH_CONSOLEHELPER_X11_PATH, argv);
#endif

	/* Set up locales. */
	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	bind_textdomain_codeset(PACKAGE, "UTF-8");
	textdomain(PACKAGE);

	/* Find the basename of the program we were invoked as. */
	progname = strrchr(argv[0], '/');
	if(progname) {
		progname++;	/* Skip over the '/' character. */
	} else {
		progname = argv[0];
	}

#ifndef DISABLE_X11
	/* If DISPLAY is set, or if stdin isn't a TTY, we have to check if we
	 * can display in a window.  Otherwise, all is probably lost.  */
	display = getenv("DISPLAY");
	if (((display != NULL) && (strlen(display) > 0)) ||
	   !isatty(STDIN_FILENO)) {
		int fake_argc, stderrfd, fd;
		char **fake_argv;
#ifdef USE_STARTUP_NOTIFICATION
		/* GTK+ clears the environment variable at init-time, which
		 * simply won't do. */
		char *sn_id = NULL;
		sn_id = g_strdup(getenv("DESKTOP_STARTUP_ID"));
#endif
		fake_argc = 1;
		fake_argv = g_malloc0((fake_argc + 1) * sizeof(*fake_argv));
		fake_argv[0] = argv[0];
		/* Redirect stderr to silence Xlib's "can't open display"
		 * warning, which we don't mind. */
		stderrfd = dup(STDERR_FILENO);
		if (stderrfd != -1) {
			close(STDERR_FILENO);
			do {
				fd = open("/dev/null", O_WRONLY | O_APPEND);
			} while((fd != -1) && (fd != STDERR_FILENO));
		}
		if (gtk_init_check(&fake_argc, &fake_argv))
			graphics_available = TRUE;
		/* Restore stderr. */
		if (stderrfd != -1) {
			dup2(stderrfd, STDERR_FILENO);
			close(stderrfd);
		}
#ifdef USE_STARTUP_NOTIFICATION
		/* Restore DESKTOP_STARTUP_ID.  I'll think up something mean
		 * to say about having to do this eventually. */
		setenv("DESKTOP_STARTUP_ID", sn_id, 1);
#endif
	}
#endif

	/* Allocate space for a new argv array, with room for up to 3 more
	 * items than we have in argv, plus the NULL-terminator. */
	constructed_argv = g_malloc0((argc + 3 + 1) * sizeof(char *));
	if (graphics_available) {
		/* Set up args to tell userhelper to wrap the named program
		 * using a consolehelper window to interact with the user. */
		constructed_argv[0] = (char *)UH_PATH;
		constructed_argv[1] = (char *)UH_WRAP_OPT;
		constructed_argv[2] = progname;
		offset = 2;
	} else {
		/* Set up args to tell userhelper to wrap the named program
		 * using a text-only interface. */
		constructed_argv[0] = (char *)UH_PATH;
		constructed_argv[1] = (char *)UH_TEXT_OPT;
		constructed_argv[2] = (char *)UH_WRAP_OPT;
		constructed_argv[3] = progname;
		offset = 3;
	}

	/* Copy the command-line arguments, except for the program name. */
	for (i = 1; i < argc; i++) {
		constructed_argv[i + offset] = argv[i];
	}

	/* If we can open a window, use the graphical wrapper routine. */
#ifndef DISABLE_X11
	if (graphics_available)
		return userhelper_runv(FALSE, UH_PATH, constructed_argv);
#endif
	/* Text mode doesn't need the whole pipe thing. */
	execv(UH_PATH, constructed_argv);
	return 1;
}

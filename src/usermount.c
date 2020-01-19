/*
 * Copyright (C) 1997 Red Hat Software, Inc.
 * Copyright (C) 2001, 2003, 2007 Red Hat, Inc.
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

/* TODO notes.
 * This code is probably not the cleanest code ever written right
 * now.  In fact, I'm sure it's not.  When I have a breather, I'll
 * clean it up a bit... which includes moving some of the tedious gui
 * stuff out into a library of sorts.  I'm sick of coding info and
 * error boxes by hand. :)
 * 
 * Things that would be nifty features, that I'll add one of these
 * days...
 * - swap(on/off) for swap partitions.  Right now the tool ignores
 *   anything with a fstype of swap.  That's pretty much the right
 *   thing to do, for now.
 * - eject button.  Something I'd like to see... I'm not quite clear
 *   on how I could check which devices are ejectable, though.
 *   Clearly, it would require write permissions, but how do I tell if
 *   I'm on a machine with an ejectable floppy or not... interesting
 *   question. 
 */

#include "config.h"
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <errno.h>
#include <glob.h>
#include <libintl.h>
#include <limits.h>
#include <locale.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wait.h>

#include <blkid/blkid.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include "userdialogs.h"

#define MOUNT_TEXT	_("_Mount")
#define UNMOUNT_TEXT	_("Un_mount")
#define PREFERRED_FS	"ext2"
enum {
	ACTION_FORMAT = 1,
	ACTION_MOUNT = 2,
};
#define PAD		8

/* The structure which holds all of the information needed to process a
 * mount/umount/format request. */
struct mountinfo {
	char *dir;		/* mount point */
	char *dev;		/* device name */
	char *fstype;		/* filesystem type */
	gboolean mounted;	/* TRUE=mounted, FALSE=unmounted */
	gboolean writable;	/* TRUE=+w, FALSE=-w */
	gboolean fdformat;	/* TRUE=should run fdformat, FALSE=can't */
	struct mountinfo *next;	/* linked list... */
};

/* The tree view widget which contains the needed mountinfo struct in its
 * fourth column. */
static GtkTreeSelection *tree_selection = NULL;

/* Static pointer to the dialog's buttons. */
static GtkWidget *mount_button = NULL, *format_button = NULL;

/* Check whether PATH is mounted
   Return -1 if PATH can't ever possibly be mounted, 0 or 1 otherwise */
static int
is_mounted(const char *path)
{
	char *parent_dir;
	int res;
	struct stat st1, st2;

	if (stat(path, &st1) != 0)
		goto err;
	/* Get the name of the mountpoint's parent directory. */
	if (strrchr(path, '/') == path)
		/* The mountpoint is a top-level directory. */
		parent_dir = g_strdup("/");
	else
		/* The parent is not the root directory. */
		parent_dir = g_strconcat(path, "/..", NULL);

	/* Verify that the mountpoint and its parent exist. */
	res = stat(parent_dir, &st2);
	g_free(parent_dir);
	if (res != 0)
		goto err;

	/* Check if the filesystem is mounted. */
	return st1.st_dev != st2.st_dev;

err:
	return -1;
}

/* This function parses /etc/fstab using getmntent() and uses other tricks
 * to fill in the info for all filesystems with the 'pamconsole' option...
 * unless, of course getuid() == 0, then we get them all.  No reason root
 * shouldn't be allowed to use this, right?.
 *
 * Returns a linked list, on unrecoverable error shows a dialog and exits.
 */
static struct mountinfo *
build_mountinfo_list(void)
{
	gboolean superuser;
	blkid_cache cache;
	struct mountinfo *ret;
	struct mntent *fstab_entry;
	FILE *fstab;

	superuser = (geteuid() == 0);

	cache = NULL;
	(void)blkid_get_cache(&cache, NULL);

	/* Open the /etc/fstab file (yes, the preprocessor define is named
	 * with an "m", don't let that throw you. */
	fstab = setmntent(_PATH_MNTTAB, "r");
	if (fstab == NULL) {
		GtkWidget *dialog;
		int err;

		err = errno;
		dialog = create_error_box(_("Error loading list of file "
					    "systems"), _("User Mount Tool"));
		gtk_message_dialog_format_secondary_text
			(GTK_MESSAGE_DIALOG(dialog), "%s", strerror(err));

		gtk_dialog_run(GTK_DIALOG(dialog));
		gtk_widget_destroy(dialog);
		exit(EXIT_FAILURE);
	}

	ret = NULL;
	/* Iterate over all of the entries. */
	while((fstab_entry = getmntent(fstab)) != NULL) {
		struct stat dev_st;
		struct mountinfo *tmp;
		const char *mountpoint;
		char *dev, *mkfs, *sbindir;
		gboolean known_device, owner, usable;
		int res;

		dev = blkid_get_devname(cache, fstab_entry->mnt_fsname, NULL);
		if (dev == NULL)
			dev = fstab_entry->mnt_fsname;
		if (strncmp(dev, "/dev/", 5) == 0 && stat (dev, &dev_st) == 0)
			known_device = TRUE;
		else
			known_device = FALSE;
		if (dev != fstab_entry->mnt_fsname)
			free(dev);

		owner = (hasmntopt(fstab_entry, "owner") != NULL
			 && known_device && dev_st.st_uid == getuid());

		/* We only process this entry if we are allowed to mount it
		   and it's not a swap partition. */
		usable = (superuser
			  || hasmntopt(fstab_entry, "pamconsole") != NULL
			  || hasmntopt(fstab_entry, "user") != NULL
			  || hasmntopt(fstab_entry, "users") != NULL
			  || owner);
		if (!usable)
			continue;

		if (strcmp(fstab_entry->mnt_type, "swap") == 0)
			continue;

		mountpoint = fstab_entry->mnt_dir;
		/* Screen out bogus mount points.  I consider "/" bogus, too. */
		if (mountpoint[0] != '/' || strlen(mountpoint) < 2)
			continue;

		res = is_mounted(mountpoint);
		if (res == -1)
			continue;

		/* Create a new list item and place it at the list's head. */
		tmp = g_malloc0(sizeof(struct mountinfo));
		tmp->next = ret;
		ret = tmp;

		/* Populate the structure. */
		ret->dir = g_strdup(fstab_entry->mnt_dir);
		ret->dev = g_strdup(fstab_entry->mnt_fsname);
		ret->fstype = g_strdup(fstab_entry->mnt_type);

		/* Check if the filesystem is mounted. */
		ret->mounted = res == 1;

		/* Check if we can write to the device.  This indicates that
		 * we can format it and create a filesystem on it. */
		ret->writable = ((access(ret->dev, W_OK) == 0) || superuser);

		/* Now double-check that we have the software to format it. */
		if(strcmp(ret->fstype, "auto") != 0) {
			sbindir = g_path_get_dirname(PATH_MKFS);
			mkfs = g_strconcat(sbindir, "/mkfs.%s", ret->fstype,
					   NULL);
			g_free(sbindir);
			if(access(mkfs, X_OK) != 0) {
				ret->writable = FALSE;
			}
			g_free(mkfs);
		}

		/* FIXME: we make a kernel assumption which is NOT PORTABLE to
		 * determine if it's a floppy-disk-type device. */
		ret->fdformat = (known_device && S_ISBLK(dev_st.st_mode)
				 && (major(dev_st.st_rdev) == 2 ||  /* fdc */
				     major(dev_st.st_rdev) == 47)); /* usb */
	}

	endmntent(fstab);

	blkid_put_cache(cache);

	return ret;
}

/* Return the mountinfo struct associated with the currently-selected row in
 * the TreeView, which is stored in the fourth column in its model. */
static struct mountinfo *
selected_info(void)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	struct mountinfo *info = NULL;
	if (gtk_tree_selection_get_selected(GTK_TREE_SELECTION(tree_selection),
					    &model, &iter)) {
		gtk_tree_model_get(GTK_TREE_MODEL(model), &iter,
				   3, &info,
				   -1);
	}
	return info;
}

/* If the selection changed, we need to enable/disable specific buttons
 * depending on whether or not the selected filesystem is mounted or not. */
static void
changed_callback(GtkTreeSelection *ignored, gpointer also_ignored)
{
	struct mountinfo *info;

	(void)ignored;
	(void)also_ignored;
	info = selected_info();
	if (info != NULL) {
		/* Enable/disable buttons based on whether the filesystem is
		 * mounted or not. */
		info->mounted = is_mounted(info->dir) == 1;
		gtk_widget_set_sensitive(format_button,
					 info->writable && !info->mounted);
		gtk_button_set_label(GTK_BUTTON(mount_button), info->mounted
				     ? UNMOUNT_TEXT : MOUNT_TEXT);
		gtk_widget_set_sensitive(mount_button, TRUE);
	} else {
		gtk_widget_set_sensitive(format_button, FALSE);
		gtk_widget_set_sensitive(mount_button, FALSE);
	}
}

static void
format(struct mountinfo *info)
{
	GtkWidget *dialog, *vbox, *fdformat;
	GtkWidget *label, *options, *table;
	GtkResponseType response;
	char *command[6];
	GError *error = NULL;
	char *mkfs = NULL;
	glob_t results;
	gboolean results_valid = FALSE;

	/* Verify that the user really wants to do this, because it's
	 * a destructive process. */
	dialog = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_QUESTION,
					GTK_BUTTONS_YES_NO,
					_("Are you sure you want to format this"
					  " disk?  You will destroy any data it"
					  " currently contains."));

	/* Create a table to hold some widgets. */
	table = gtk_table_new(2, 2, FALSE);
	gtk_container_set_border_width(GTK_CONTAINER(table), PAD);

	/* If it's the kind of device we low-level format, let the user
	 * disable that step. */
	if (info->fdformat) {
		fdformat = gtk_check_button_new_with_mnemonic(_("Perform a "
								"_low-level "
								"format."));
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fdformat), TRUE);
		gtk_widget_show(fdformat);
	} else
		fdformat = NULL;

	/* If it's a filesystem of type "auto", we also have to let the user
	 * select a filesystem type to format it as. */
	if (strcmp(info->fstype, "auto") == 0) {
		char *sbindir;
		size_t i, defaultfs;

		sbindir = g_path_get_dirname(PATH_MKFS);
		mkfs = g_strconcat(sbindir, "/mkfs.*", NULL);
		g_free(sbindir);

		options = gtk_combo_box_text_new();

		defaultfs = 0;
		results_valid = glob(mkfs, 0, NULL, &results) == 0;
		if (!results_valid)
			results.gl_pathc = 0;
		for(i = 0; i < results.gl_pathc; i++) {
			const char *text;

			text = results.gl_pathv[i] + strlen(mkfs) - 1;
			gtk_combo_box_text_append_text
				(GTK_COMBO_BOX_TEXT(options), text);
			if(strcmp(text, PREFERRED_FS) == 0)
				defaultfs = i;
		}

		gtk_combo_box_set_active(GTK_COMBO_BOX(options), defaultfs);
		gtk_widget_show(options);

		label = gtk_label_new_with_mnemonic(_("Select a _filesystem type to create:"));
		gtk_label_set_mnemonic_widget(GTK_LABEL(label), options);
		gtk_widget_show(label);
	} else {
		options = NULL;
		label = NULL;
	}

	/* Run the dialog. */
	if (fdformat)
		gtk_table_attach_defaults(GTK_TABLE(table), fdformat,
					  0, 2, 0, 1);
	if (options && label) {
		gtk_table_attach_defaults(GTK_TABLE(table), label,
					  0, 1, 1, 2);
		gtk_table_attach_defaults(GTK_TABLE(table), options,
					  1, 2, 1, 2);
	}
	gtk_widget_show_all(table);
	vbox = gtk_dialog_get_content_area (GTK_DIALOG(dialog));
	gtk_box_pack_start(GTK_BOX(vbox), table, TRUE, TRUE, 0);

	response = gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_hide(dialog);

	/* If the user said "yes", make a new filesystem on it. */
	if (response == GTK_RESPONSE_YES) {
		gint child;
		int status;
		char *fstype;

		if (GTK_IS_CHECK_BUTTON(fdformat)) {
			GtkToggleButton *toggle = GTK_TOGGLE_BUTTON(fdformat);
			if (gtk_toggle_button_get_active(toggle)) {
				command[0] = (char *)PATH_FDFORMAT;
				command[1] = info->dev;
				command[2] = NULL;
				if (g_spawn_async("/",
						  command, NULL,
						  G_SPAWN_DO_NOT_REAP_CHILD |
						  G_SPAWN_STDOUT_TO_DEV_NULL,
						  NULL, NULL,
						  &child, &error)) {
					/* FIXME: something better than busy waiting? */
					while (waitpid(child, &status, WNOHANG) == 0) {
						gtk_main_iteration_do(FALSE);
					}
				}
			}
		}
		fstype = NULL;
		if (options) {
			int fs;

			fs = gtk_combo_box_get_active(GTK_COMBO_BOX(options));
			if (fs >= 0 && (size_t)fs < results.gl_pathc)
				fstype = results.gl_pathv[fs]
					+ strlen(mkfs) - 1;
		}
		if (fstype == NULL) {
			fstype = info->fstype;
		}
		command[0] = (char *)PATH_MKFS;
		command[1] = (char *)"-t";
		command[2] = fstype;
		if (!strcmp(fstype,"vfat") || !strcmp(fstype,"msdos")) {
			command[3] = (char *)"-I";
			command[4] = info->dev;
			command[5] = NULL;
		} else {
			command[3] = info->dev;
			command[4] = NULL;
		}
		if (g_spawn_async("/",
				  command, NULL,
				  G_SPAWN_DO_NOT_REAP_CHILD |
				  G_SPAWN_STDOUT_TO_DEV_NULL,
				  NULL, NULL,
				  &child, &error)) {
			/* FIXME: something better than busy waiting? */
			while (waitpid(child, &status, WNOHANG) == 0) {
				gtk_main_iteration_do(FALSE);
			}
		}
	}

	gtk_widget_destroy(dialog);
	g_free(mkfs);
	if (results_valid)
		globfree(&results);
}

static void
response_callback(GtkWidget *emitter, gint response, gpointer user_data)
{
	struct mountinfo *info;
	char *command[3];
	char *messages = NULL;
	int status;
	GError *error = NULL;
	GtkWidget *toplevel, *dialog;

	(void)user_data;
	info = selected_info();
	switch (response) {
		case GTK_RESPONSE_CLOSE:
			gtk_main_quit();
			break;
		case ACTION_FORMAT:
			/* Format. */
			g_return_if_fail(info != NULL);
			toplevel = gtk_widget_get_toplevel(emitter);
			if (gtk_widget_is_toplevel(toplevel)) {
				gtk_widget_set_sensitive(toplevel, FALSE);
			}
			format(info);
			if (gtk_widget_is_toplevel(toplevel)) {
				gtk_widget_set_sensitive(toplevel, TRUE);
			}
			break;
		case ACTION_MOUNT:
			/* Mount/unmount. */
			g_return_if_fail(info != NULL);
			command[0] = info->mounted
				? (char *)PATH_UMOUNT : (char *)PATH_MOUNT;
			command[1] = info->dir;
			command[2] = NULL;
			if (g_spawn_sync("/",
					 command,
					 NULL,
					 G_SPAWN_STDOUT_TO_DEV_NULL,
					 NULL,
					 NULL,
					 NULL,
					 &messages,
					 &status,
					 &error)) {
				if (messages != NULL) {
					if (strlen(messages) > 0) {
						dialog = create_error_box(messages,
									  NULL);
						gtk_dialog_run(GTK_DIALOG(dialog));
						gtk_widget_destroy(dialog);
					}
					g_free(messages);
				}
			}
			changed_callback(NULL, NULL);
			break;
		default:
			break;
	}
}

static gboolean
create_usermount_window(void)
{
	GtkWidget *dialog, *treeview;
	GtkTreeIter iter;
	GtkListStore *model;
	GtkTreeViewColumn *column[3];
	GtkTreeSelection *selection;
	GtkCellRenderer *renderer;
	struct mountinfo *info, *i;

	/* First, create a list of filesystems which are configured on this
	 * system, by scanning /etc/fstab.  If we get an empty list back,
	 * bail out and just display an error dialog. */
	info = build_mountinfo_list();
	if (info == NULL) {
		dialog = create_error_box(_("There are no filesystems which you are allowed to mount or unmount.\nContact your administrator."),
					  _("User Mount Tool"));
		g_signal_connect_object(G_OBJECT(dialog), "destroy",
					G_CALLBACK(gtk_main_quit), NULL,
					G_CONNECT_AFTER);
		g_signal_connect_object(G_OBJECT(dialog), "delete-event",
					G_CALLBACK(gtk_main_quit), NULL,
					G_CONNECT_AFTER);
		gtk_dialog_run(GTK_DIALOG(dialog));
		return FALSE;
	}

	/* Create the dialog. */
	dialog = gtk_dialog_new_with_buttons(_("User Mount Tool"),
					     NULL,
					     0,
					     GTK_STOCK_CLOSE,
					     GTK_RESPONSE_CLOSE,
					     NULL);

	/* Connect default signal handlers. */
	g_signal_connect(G_OBJECT(dialog), "destroy", G_CALLBACK(gtk_main_quit),
			 NULL);
	g_signal_connect(G_OBJECT(dialog), "delete-event",
			 G_CALLBACK(gtk_main_quit), NULL);

	/* Set the window icon */
	gtk_window_set_icon_from_file(GTK_WINDOW(dialog),
				      PIXMAPDIR "/disks.png", NULL);

	/* Create the other buttons. */
	format_button = gtk_button_new_with_mnemonic(_("_Format"));
	gtk_widget_set_sensitive(format_button, FALSE);
	gtk_dialog_add_action_widget(GTK_DIALOG(dialog), format_button,
				     ACTION_FORMAT);
	mount_button = gtk_button_new_with_mnemonic(info->mounted
						    ? UNMOUNT_TEXT
						    : MOUNT_TEXT);
	gtk_widget_set_sensitive(mount_button, FALSE);
	gtk_dialog_add_action_widget(GTK_DIALOG(dialog), mount_button,
				     ACTION_MOUNT);

	/* Set a signal handler to handle button presses. */
	g_signal_connect(G_OBJECT(dialog), "response",
			 G_CALLBACK(response_callback), NULL);

	/* Build a tree model with this data. */
	model = gtk_list_store_new(4,
				   G_TYPE_STRING,
				   G_TYPE_STRING,
				   G_TYPE_STRING,
				   G_TYPE_POINTER);

	/* Now add a row to the list using the data. */
	for (i = info; i != NULL; i = i->next) {
		gtk_list_store_append(model, &iter);
		gtk_list_store_set(model, &iter,
				   0, i->dev,
				   1, i->dir,
				   2, i->fstype,
				   3, i,
				   -1);
	}

	/* Now add the columns to the tree view. */
	column[0] = gtk_tree_view_column_new();
	renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_column_set_title(column[0], _("Device"));
	gtk_tree_view_column_pack_start(column[0], renderer, TRUE);
	gtk_tree_view_column_add_attribute(column[0], renderer, "text", 0);

	column[1] = gtk_tree_view_column_new();
	renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_column_set_title(column[1], _("Directory"));
	gtk_tree_view_column_pack_start(column[1], renderer, TRUE);
	gtk_tree_view_column_add_attribute(column[1], renderer, "text", 1);

	column[2] = gtk_tree_view_column_new();
	renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_column_set_title(column[2], _("Filesystem"));
	gtk_tree_view_column_pack_start(column[2], renderer, TRUE);
	gtk_tree_view_column_add_attribute(column[2], renderer, "text", 2);

	/* Create the tree view and add the columns to it. */
	treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(model));
	g_object_unref(G_OBJECT(model));
	gtk_tree_view_append_column(GTK_TREE_VIEW(treeview), column[0]);
	gtk_tree_view_append_column(GTK_TREE_VIEW(treeview), column[1]);
	gtk_tree_view_append_column(GTK_TREE_VIEW(treeview), column[2]);

	/* Set up a callback for when the selection changes. */
	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
	tree_selection = selection;
	g_signal_connect(G_OBJECT(selection), "changed",
			 G_CALLBACK(changed_callback), NULL);

	/* Pack it in and show the dialog. */
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area
				   (GTK_DIALOG(dialog))),
			   treeview, TRUE, TRUE, 0);

	gtk_widget_show_all(dialog);

	return TRUE;
}

static void
format_one_device (const char *device)
{
	struct mountinfo *info, *i;

	info = build_mountinfo_list();

	for (i = info; i != NULL; i = i->next) {
		if (strcmp (i->dev, device) == 0)
			break;		
	}

	if (i && i->writable && !i->mounted) {
		format (i);
	} else {	
		GtkWidget *dialog;

		dialog = gtk_message_dialog_new (NULL, 
						 0,
						 GTK_MESSAGE_ERROR,
						 GTK_BUTTONS_OK,
						 _("The device '%s' can not be formatted."),
						 device);
                gtk_dialog_run(GTK_DIALOG(dialog));
	}
}

int
main(int argc, char *argv[])
{
	char *name;

	bindtextdomain(PACKAGE, LOCALEDIR);
	bind_textdomain_codeset(PACKAGE, "UTF-8");
	textdomain(PACKAGE);
	gtk_init(&argc, &argv);

	name = strrchr (argv[0], '/');
	name = name ? name + 1: argv[0];
	if (strcmp(name, "userformat") == 0) {
		if (argc != 2) {
			fprintf(stderr,
				_("Unexpected command-line arguments\n"));
			return 1;
		}
		format_one_device(argv[1]);
        } else {
		gboolean run_main;

		if (argc != 1) {
			fprintf(stderr,
				_("Unexpected command-line arguments\n"));
			return 1;
		}
		run_main = create_usermount_window();
		if (run_main) {
			gtk_main();
		}
	}

	return 0;
}

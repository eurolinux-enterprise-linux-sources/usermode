/* gsmclient object */

/*
 * Copyright (C) 2001 Havoc Pennington, inspired by various other
 * pieces of code including GnomeClient (C) 1998 Carsten Schaar,
 * Tom Tromey, and twm session code (C) 1998 The Open Group.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef GSM_CLIENT_H
#define GSM_CLIENT_H

#include <glib-object.h>

G_BEGIN_DECLS

typedef enum
{
  GSM_RESTART_IF_RUNNING  = 0, /* restart client in next session if
                                * it's running when the session is saved.
                                * The default.
                                */
  GSM_RESTART_ANYWAY      = 1, /* restart client in next session if it
                                * was ever in this session, even if it's
                                * already exited when the session is saved.
                                */
  GSM_RESTART_IMMEDIATELY = 2, /* respawn client if it crashes */
  
  GSM_RESTART_NEVER       = 3  /* don't save this client in the session */

} GsmRestartStyle;

/* Property names (specified in XSMP spec, section 11) */
#define GSM_CLIENT_PROPERTY_CLONE_COMMAND      "CloneCommand"
#define GSM_CLIENT_PROPERTY_CURRENT_DIRECTORY  "CurrentDirectory"
#define GSM_CLIENT_PROPERTY_DISCARD_COMMAND    "DiscardCommand"
#define GSM_CLIENT_PROPERTY_ENVIRONMENT        "Environment"
#define GSM_CLIENT_PROPERTY_PROCESS_ID         "ProcessID"
#define GSM_CLIENT_PROPERTY_PROGRAM            "Program"
#define GSM_CLIENT_PROPERTY_RESTART_COMMAND    "RestartCommand"
#define GSM_CLIENT_PROPERTY_RESIGN_COMMAND     "ResignCommand"
#define GSM_CLIENT_PROPERTY_RESTART_STYLE_HINT "RestartStyleHint"
#define GSM_CLIENT_PROPERTY_SHUTDOWN_COMMAND   "ShutdownCommand"
#define GSM_CLIENT_PROPERTY_USER_ID            "UserID"

/* GNOME extension */
#define GSM_CLIENT_PROPERTY_PRIORITY          "_GSM_Priority"

/* Traditional priorities */
#define GSM_CLIENT_PRIORITY_WINDOW_MANAGER    10 
#define GSM_CLIENT_PRIORITY_SETUP             20 /* e.g. background-properties-capplet */
#define GSM_CLIENT_PRIORITY_DESKTOP_COMPONENT 40 /* e.g. panel */
#define GSM_CLIENT_PRIORITY_NORMAL            50 /* all normal apps */


#define GSM_TYPE_CLIENT              (gsm_client_get_type ())
#define GSM_CLIENT(object)           (G_TYPE_CHECK_INSTANCE_CAST ((object), GSM_TYPE_CLIENT, GsmClient))
#define GSM_CLIENT_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), GSM_TYPE_CLIENT, GsmClientClass))
#define GSM_IS_CLIENT(object)        (G_TYPE_CHECK_INSTANCE_TYPE ((object), GSM_TYPE_CLIENT))
#define GSM_IS_CLIENT_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), GSM_TYPE_CLIENT))
#define GSM_CLIENT_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), GSM_TYPE_CLIENT, GsmClientClass))

typedef struct _GsmClient        GsmClient;
typedef struct _GsmClientClass   GsmClientClass;
typedef struct _GsmClientPrivate GsmClientPrivate;

struct _GsmClient
{
  GObject parent_instance;

  GsmClientPrivate *priv;
};

struct _GsmClientClass
{
  GObjectClass parent_class;

  void (* interaction_request_granted) (GsmClient *client,
                                        gboolean   errors_only);

  void (* save)                (GsmClient *client,
                                gboolean   is_phase2);

  void (* die)                 (GsmClient *client);

  void (* save_complete)       (GsmClient *client);
};

GType gsm_client_get_type (void) G_GNUC_CONST;

GsmClient* gsm_client_new (void);

void  gsm_client_connect    (GsmClient  *client,
                             const char *previous_id);
void  gsm_client_disconnect (GsmClient *client);

gboolean             gsm_client_get_connected (GsmClient *client);
G_CONST_RETURN char* gsm_client_get_id        (GsmClient *client);


gboolean gsm_client_get_save_is_logout  (GsmClient *client);
gboolean gsm_client_get_save_is_phase2  (GsmClient *client);

void gsm_client_request_save        (GsmClient *client);
void gsm_client_request_phase2_save (GsmClient *client);

/* request, then get interaction_request_granted signal and interact,
 * then call interaction_done after all interaction.
 */
void gsm_client_request_interaction (GsmClient *client);
void gsm_client_interaction_done    (GsmClient *client);

/* These affect all apps, not just this client. */
void gsm_client_request_global_session_save   (GsmClient *client);
void gsm_client_request_global_session_logout (GsmClient *client);

void     gsm_client_set_byte_property   (GsmClient    *client,
                                         const char   *prop_name,
                                         guint8        value);
void     gsm_client_set_string_property (GsmClient    *client,
                                         const char   *prop_name,
                                         const char   *str,
                                         int           len);
void     gsm_client_set_vector_property (GsmClient    *client,
                                         const char   *prop_name,
                                         int           argc,
                                         char        **argv);
gboolean gsm_client_get_byte_property   (GsmClient    *client,
                                         const char   *prop_name,
                                         guint8       *result);
gboolean gsm_client_get_string_property (GsmClient    *client,
                                         const char   *prop_name,
                                         char        **result);
gboolean gsm_client_get_vector_property (GsmClient    *client,
                                         const char   *prop_name,
                                         int          *argcp,
                                         char       ***argvp);

/* Convenience wrappers to set up the common properties
 * all clients will want to set.
 */

void gsm_client_set_clone_command   (GsmClient *client,
                                     int        argc,
                                     char     **argv);
void gsm_client_get_clone_command   (GsmClient *client,
                                     int       *argc,
                                     char    ***argv);
void gsm_client_set_restart_command (GsmClient *client,
                                     int        argc,
                                     char     **argv);
void gsm_client_get_restart_command (GsmClient *client,
                                     int       *argc,
                                     char    ***argv);
void gsm_client_set_discard_command (GsmClient *client,
                                     int        argc,
                                     char     **argv);
void gsm_client_get_discard_command (GsmClient *client,
                                     int       *argc,
                                     char    ***argv);

/* Desktop components such as window manager, panel, etc. may set
 * these properties.
 */
void            gsm_client_set_restart_style (GsmClient       *client,
                                              GsmRestartStyle  style);
GsmRestartStyle gsm_client_get_restart_style (GsmClient       *client);

void gsm_client_set_priority (GsmClient *client,
                              int        priority);
int  gsm_client_get_priority (GsmClient *client);


char* gsm_client_create_save_id (GsmClient *client);

/* Backdoor to allow direct use of SMlib API */
void* gsm_client_get_smc_connection (GsmClient *client);

G_END_DECLS

#endif /* GSM_CLIENT_H */

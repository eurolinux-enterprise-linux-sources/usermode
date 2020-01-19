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

#include <config.h>
#include "gsmclient.h"
#include <props.h> /* convenience lib in ../src */

#include <X11/ICE/ICElib.h>
#include <X11/SM/SMlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <gobject/gmarshal.h>

#include <libintl.h>
#define _(x) dgettext (GETTEXT_PACKAGE, x)

#ifdef  G_LOG_DOMAIN
#undef  G_LOG_DOMAIN
#endif
#define G_LOG_DOMAIN "GsmClient"

static void
gsm_verbose (const char *format, ...)
{
  va_list args;
  gchar *str;
  static int verbose = -1;
  
  if (verbose < 0)
    verbose = g_getenv ("GSM_CLIENT_VERBOSE") != NULL;

  if (!verbose)
    return;
  
  g_return_if_fail (format != NULL);
  
  va_start (args, format);
  str = g_strdup_vprintf (format, args);
  va_end (args);

  fprintf (stderr, "SM client: %s %d: ",
           g_get_prgname () ? g_get_prgname () : "",
           (int) getpid ());
  fputs (str, stderr);

  fflush (stderr);
  
  g_free (str);
}

typedef enum
{
  GSM_CLIENT_STATE_DISCONNECTED,
  GSM_CLIENT_STATE_IDLE,
  GSM_CLIENT_STATE_SAVING,
  GSM_CLIENT_STATE_SAVING_PHASE2,
  GSM_CLIENT_STATE_PHASE2_REQUESTED,
  GSM_CLIENT_STATE_INTERACT_REQUESTED,
  GSM_CLIENT_STATE_INTERACT_REQUESTED_PHASE2
} GsmClientState;

struct _GsmClientPrivate
{
  GsmClientState state;
  GList *properties;
  SmcConn connection;
  char *client_id;
  guint in_shutdown : 1;
  guint interaction_granted : 1;
  guint save_interaction_errors_only : 1;
  guint ignore_next_save : 1;
};

enum {
  INTERACTION_REQUEST_GRANTED,
  SAVE,
  DIE,
  SAVE_COMPLETE,
  LAST_SIGNAL
};

static void gsm_client_init        (GsmClient      *client);
static void gsm_client_class_init  (GsmClientClass *klass);
static void gsm_client_finalize    (GObject        *object);

static void ice_init               (void);

static void client_save_phase_2_callback       (SmcConn   smc_conn,
                                                SmPointer client_data);
static void client_save_yourself_callback      (SmcConn   smc_conn,
                                                SmPointer client_data,
                                                int       save_style,
                                                Bool      shutdown,
                                                int       interact_style,
                                                Bool      fast);
static void client_die_callback                (SmcConn   smc_conn,
                                                SmPointer client_data);
static void client_save_complete_callback      (SmcConn   smc_conn,
                                                SmPointer client_data);
static void client_shutdown_cancelled_callback (SmcConn   smc_conn,
                                                SmPointer client_data);
static void client_interact_callback           (SmcConn   smc_conn,
                                                SmPointer client_data);

static gpointer parent_class;
static guint signals[LAST_SIGNAL] = { 0 };


GType
gsm_client_get_type (void)
{
  static GType object_type = 0;

  g_type_init ();
  
  if (!object_type)
    {
      static const GTypeInfo object_info =
      {
        sizeof (GsmClientClass),
        (GBaseInitFunc) NULL,
        (GBaseFinalizeFunc) NULL,
        (GClassInitFunc) gsm_client_class_init,
        NULL,           /* class_finalize */
        NULL,           /* class_data */
        sizeof (GsmClient),
        0,              /* n_preallocs */
        (GInstanceInitFunc) gsm_client_init,
	NULL
      };
      
      object_type = g_type_register_static (G_TYPE_OBJECT,
                                            "GsmClient",
                                            &object_info, 0);
    }
  
  return object_type;
}

static void
push_prop (GsmClient *client,
           SmProp    *prop)
{
  client->priv->properties =
    g_list_prepend (client->priv->properties,
                    prop);
}

static void
gsm_client_init (GsmClient *client)
{
  char pid_str[64];
  int   empty_vector_len = 0;
  char *empty_vector[] = { NULL };
  
  client->priv = g_new (GsmClientPrivate, 1);
  client->priv->state = GSM_CLIENT_STATE_DISCONNECTED;
  client->priv->properties = NULL;
  client->priv->connection = NULL;
  client->priv->client_id = NULL;
  client->priv->in_shutdown = FALSE;
  client->priv->interaction_granted = FALSE;
  client->priv->save_interaction_errors_only = FALSE;
  client->priv->ignore_next_save = FALSE;
  
  /* Default property values (this code assumes we start
   * with an empty proplist)
   */
  push_prop (client, smprop_new_string (GSM_CLIENT_PROPERTY_CURRENT_DIRECTORY,
                                        g_get_current_dir (), -1));

  g_snprintf (pid_str, sizeof (pid_str), "%d", (int) getpid ());
  push_prop (client, smprop_new_string (GSM_CLIENT_PROPERTY_PROCESS_ID,
                                        pid_str, -1));

  push_prop (client, smprop_new_vector (GSM_CLIENT_PROPERTY_ENVIRONMENT,
                                        empty_vector_len,
                                        empty_vector));

  if (g_get_prgname ())
    push_prop (client, smprop_new_string (GSM_CLIENT_PROPERTY_PROGRAM,
                                          g_get_prgname (), -1));

  push_prop (client, smprop_new_card8 (GSM_CLIENT_PROPERTY_RESTART_STYLE_HINT,
                                       GSM_RESTART_IF_RUNNING));
  
  push_prop (client, smprop_new_string (GSM_CLIENT_PROPERTY_USER_ID,
                                        g_get_user_name (), -1));
}

static void
gsm_client_class_init (GsmClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  
  parent_class = g_type_class_peek_parent (klass);
  
  object_class->finalize = gsm_client_finalize;

  signals[INTERACTION_REQUEST_GRANTED] =
    g_signal_new ("interaction_request_granted",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GsmClientClass, interaction_request_granted),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__BOOLEAN,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_BOOLEAN);

  
  signals[SAVE] =
    g_signal_new ("save",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GsmClientClass, save),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__BOOLEAN,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_BOOLEAN);

  signals[DIE] =
    g_signal_new ("die",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GsmClientClass, die),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);

  signals[SAVE_COMPLETE] =
    g_signal_new ("save_complete",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GsmClientClass, save_complete),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);
}

static void
gsm_client_finalize (GObject *object)
{
  GsmClient *client;

  client = GSM_CLIENT (object);

  if (client->priv->connection)
    g_warning ("Bug in GsmClient reference counting! client not disconnected before finalization");
  
  proplist_free (client->priv->properties);
  g_free (client->priv->client_id);
  
  g_free (client->priv);
  
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/**
 * gsm_client_new:
 * 
 * Creates a new #GsmClient object. A #GsmClient represents
 * a connection to the session manager. After creating the
 * client object, set any properties on it, then
 * call gsm_client_connect() to connect to the session
 * manager.
 * 
 * Return value: a new #GsmClient
 **/
GsmClient*
gsm_client_new (void)
{
  return g_object_new (GSM_TYPE_CLIENT, NULL);
}

#define ERROR_STRING_LENGTH 256

void
gsm_client_connect (GsmClient  *client,
                    const char *previous_id)
{
  SmcCallbacks callbacks;
  char        *client_id;
  char error_string_ret[ERROR_STRING_LENGTH] = "";
  
  g_return_if_fail (GSM_IS_CLIENT (client));
  g_return_if_fail (client->priv->connection == NULL);

  if (g_getenv ("SESSION_MANAGER") == NULL)
    {
      gsm_verbose ("SESSION_MANAGER not set, not attempting to contact SM\n");
      return;
    }
  
  ice_init ();

  /* This code straight from gnome-client.c */
  
  callbacks.save_yourself.callback      = client_save_yourself_callback;
  callbacks.die.callback                = client_die_callback;
  callbacks.save_complete.callback      = client_save_complete_callback;
  callbacks.shutdown_cancelled.callback = client_shutdown_cancelled_callback;

  callbacks.save_yourself.client_data = client;
  callbacks.die.client_data = client;
  callbacks.save_complete.client_data = client;
  callbacks.shutdown_cancelled.client_data = client;  

  client_id = NULL;
  client->priv->connection = 
    SmcOpenConnection (NULL, client, 
                       SmProtoMajor, SmProtoMinor,
                       SmcSaveYourselfProcMask | SmcDieProcMask |
                       SmcSaveCompleteProcMask | 
                       SmcShutdownCancelledProcMask,
                       &callbacks,
                       (char*) previous_id, &client_id,
                       ERROR_STRING_LENGTH, error_string_ret);


  if (client_id)
    {
      client->priv->client_id = g_strdup (client_id);
      free (client_id);
      gsm_verbose ("Got client ID \"%s\" submitted previous id \"%s\"\n",
                   client->priv->client_id, previous_id ? previous_id : "none");
    }
  
  if (client->priv->connection == NULL)
    {
      fprintf (stderr,
               _("%s failed to connect to the session manager: %s\n"),
               g_get_prgname (), error_string_ret[0] ?
               error_string_ret : "no error message given");
    }

  if (client->priv->connection)
    {
      SmProp **props;
      int n_props;

      /* increment refcount; the SM connection owns one
       * reference, since the client must be alive
       * while connected. This also makes us safe
       * against our various callbacks being invoked with
       * client as user data.
       */
      g_object_ref (G_OBJECT (client));

      /* ignore the save-on-connect that we get as confirmation
       * of connection (only if we didn't have a previous ID)
       */
      if (previous_id == NULL)
        client->priv->ignore_next_save = TRUE;
      
      /* Enter idle state, sync proplist */
      client->priv->state = GSM_CLIENT_STATE_IDLE;
      
      proplist_as_array (client->priv->properties,
                         &props, &n_props);

      gsm_verbose ("Setting initial properties\n");
      if (n_props > 0)
        {
          SmcSetProperties (client->priv->connection,
                            n_props, props);
        }
      
      g_free (props);
    }
}

void
gsm_client_disconnect (GsmClient *client)
{
  if (client->priv->connection)
    {
      if (client->priv->state != GSM_CLIENT_STATE_IDLE)
        {
          g_warning ("GsmClient can't be disconnected during a save");
          return;
        }

      gsm_verbose ("Disconnecting\n");
      
      SmcCloseConnection (client->priv->connection,
                          0, NULL);

      client->priv->connection = NULL;
      client->priv->state = GSM_CLIENT_STATE_DISCONNECTED;
      client->priv->ignore_next_save = FALSE;
      
      /* remove reference owned by the connection */
      g_object_unref (G_OBJECT (client));
    }
}

static void
gsm_client_end_save (GsmClient *client,
                     gboolean   send_save_done)
{
  if (client->priv->connection == NULL)
    return;
  
  if (client->priv->state == GSM_CLIENT_STATE_IDLE)
    return;

  client->priv->state = GSM_CLIENT_STATE_IDLE;
  client->priv->in_shutdown = FALSE;
  client->priv->interaction_granted = FALSE;
  client->priv->save_interaction_errors_only = FALSE;

  gsm_verbose ("Entered IDLE state\n");

  if (send_save_done)
    {
      gsm_verbose ("Sending SaveYourselfDone\n");
      SmcSaveYourselfDone (client->priv->connection, True);
    }
}

gboolean
gsm_client_get_connected (GsmClient *client)
{
  g_return_val_if_fail (GSM_IS_CLIENT (client), FALSE);
  
  return client->priv->connection != NULL;
}

const char*
gsm_client_get_id (GsmClient *client)
{
  g_return_val_if_fail (GSM_IS_CLIENT (client), NULL);

  return client->priv->client_id;
}

gboolean
gsm_client_get_save_is_logout (GsmClient *client)
{
  g_return_val_if_fail (GSM_IS_CLIENT (client), FALSE);
  
  return client->priv->in_shutdown;
}

gboolean
gsm_client_get_save_is_phase2 (GsmClient *client)
{
  g_return_val_if_fail (GSM_IS_CLIENT (client), FALSE);
  
  return
    client->priv->state == GSM_CLIENT_STATE_SAVING_PHASE2 ||
    client->priv->state == GSM_CLIENT_STATE_INTERACT_REQUESTED_PHASE2;
}

void
gsm_client_request_save (GsmClient *client)
{
  g_return_if_fail (GSM_IS_CLIENT (client));
  
  if (client->priv->connection == NULL)
    return;

  if (client->priv->state != GSM_CLIENT_STATE_IDLE)
    {
      g_warning ("May not request a save during another save");
      return;
    }

  gsm_verbose ("Requesting SaveYourself\n");
  
  SmcRequestSaveYourself (client->priv->connection,
                          SmSaveBoth,
                          False,
                          SmInteractStyleAny,
                          False, False);
}

void
gsm_client_request_phase2_save (GsmClient *client)
{
  g_return_if_fail (GSM_IS_CLIENT (client));
  
  if (client->priv->connection == NULL)
    return;

  if (client->priv->state != GSM_CLIENT_STATE_SAVING)
    {
      g_warning ("May only request a phase 2 save during a regular save");
      return;
    }

  gsm_verbose ("Requesting Phase2 save (entering state PHASE2_REQUESTED)\n");
  
  client->priv->state = GSM_CLIENT_STATE_PHASE2_REQUESTED;
  
  SmcRequestSaveYourselfPhase2 (client->priv->connection,
                                client_save_phase_2_callback,
                                client);
}

void
gsm_client_request_interaction (GsmClient *client)
{
  g_return_if_fail (GSM_IS_CLIENT (client));
  
  if (client->priv->connection == NULL)
    return;
  
  if (client->priv->state == GSM_CLIENT_STATE_IDLE)
    {
      g_warning ("May only request interaction during a save");
      return;
    }

  if (client->priv->state == GSM_CLIENT_STATE_PHASE2_REQUESTED)
    {
      g_warning ("May not request interaction until requested phase 2 save begins");
      return;
    }
  
  /* Allow apps to request interaction from multiple code sections
   * that might need to interact.
   */
  if (client->priv->state == GSM_CLIENT_STATE_INTERACT_REQUESTED ||
      client->priv->state == GSM_CLIENT_STATE_INTERACT_REQUESTED_PHASE2)
    {
      gsm_verbose ("Ignoring interact request with one already pending\n");
      return;
    }

  if (client->priv->state == GSM_CLIENT_STATE_SAVING)
    {
      client->priv->state = GSM_CLIENT_STATE_INTERACT_REQUESTED;
      gsm_verbose ("Entering state INTERACT_REQUESTED\n");
    }
  else if (client->priv->state == GSM_CLIENT_STATE_SAVING_PHASE2)
    {
      client->priv->state = GSM_CLIENT_STATE_INTERACT_REQUESTED_PHASE2;
      gsm_verbose ("Entering state INTERACT_REQUESTED_PHASE2\n");
    }

  gsm_verbose ("Requesting interaction\n");
  SmcInteractRequest (client->priv->connection,
                      /* ignore this feature of the protocol by always
                       * claiming normal
                       */
                      SmDialogNormal,
                      client_interact_callback,
                      client);
}

void
gsm_client_interaction_done (GsmClient *client)
{
  g_return_if_fail (GSM_IS_CLIENT (client));
  
  if (client->priv->connection == NULL)
    return;

  if (!client->priv->interaction_granted ||
      !(client->priv->state == GSM_CLIENT_STATE_INTERACT_REQUESTED ||
        client->priv->state == GSM_CLIENT_STATE_INTERACT_REQUESTED_PHASE2))
    {
      g_warning ("Can only notify session manager of interaction completed if an interaction was requested and granted and not already completed and the save is still in progress");
      return;
    }

  gsm_verbose ("Sending InteractDone\n");
  
  SmcInteractDone (client->priv->connection,
                   /* do not cancel logout */
                   False);

  client->priv->interaction_granted = FALSE;

  if (client->priv->state == GSM_CLIENT_STATE_INTERACT_REQUESTED)
    {
      client->priv->state = GSM_CLIENT_STATE_SAVING;
      gsm_verbose ("Entering state SAVING\n");
    }
  else if (client->priv->state == GSM_CLIENT_STATE_INTERACT_REQUESTED_PHASE2)
    {
      client->priv->state = GSM_CLIENT_STATE_SAVING_PHASE2;
      gsm_verbose ("Entering state SAVING_PHASE2\n");
    }

  /* End the save. Because we always end the save "magically" -
   * after interaction is done if interaction is requested,
   * or after the "save" signal emission otherwise,
   * clients can't interact during both phase 1 and phase 2 -
   * if they interact in phase 1 they can't request phase 2,
   * since we just end the save cycle here.
   */
  gsm_client_end_save (client, TRUE);
}

void
gsm_client_request_global_session_save (GsmClient *client)
{
  g_return_if_fail (GSM_IS_CLIENT (client));

  if (client->priv->connection == NULL)
    return;

  if (client->priv->state != GSM_CLIENT_STATE_IDLE)
    {
      g_warning ("May not request a global save during another save");
      return;
    }

  gsm_verbose ("Requesting global save\n");
  
  SmcRequestSaveYourself (client->priv->connection,
                          SmSaveBoth,
                          False,
                          SmInteractStyleAny,
                          False,
                          True);
}

void
gsm_client_request_global_session_logout (GsmClient *client)
{
  g_return_if_fail (GSM_IS_CLIENT (client));

  if (client->priv->connection == NULL)
    return;

  if (client->priv->state != GSM_CLIENT_STATE_IDLE)
    {
      g_warning ("May not request a global save/logout during another save");
      return;
    }

  gsm_verbose ("Requesting global save and logout\n");
  SmcRequestSaveYourself (client->priv->connection,
                          SmSaveBoth,
                          True,
                          SmInteractStyleAny,
                          False,
                          True);
}

static void
replace_one_prop (GsmClient *client,
                  SmProp    *new_prop)
{
  GList *l;
  
  l = proplist_find_link_by_name (client->priv->properties,
                                     new_prop->name);
  if (l)
    {
      SmFreeProperty (l->data);
      l->data = new_prop;
    }
  else
    {
      push_prop (client, new_prop);
    }
  
  if (client->priv->connection != NULL)
    {
      SmProp *props[1];
      
      props[0] = new_prop;
      
      SmcSetProperties (client->priv->connection,
                        1, props);
    }
}

void
gsm_client_set_byte_property (GsmClient    *client,
                              const char   *prop_name,
                              guint8       value)
{
  g_return_if_fail (prop_name != NULL);

  gsm_verbose ("Setting prop '%s' to '%d'\n", prop_name, value);
  replace_one_prop (client, smprop_new_card8 (prop_name, value));
}

void
gsm_client_set_string_property (GsmClient    *client,
                                const char   *prop_name,
                                const char   *str,
                                int           len)
{
  g_return_if_fail (prop_name != NULL);
  g_return_if_fail (str != NULL);
  
  gsm_verbose ("Setting prop '%s' to a string\n", prop_name);
  replace_one_prop (client, smprop_new_string (prop_name, str, len));
}

void
gsm_client_set_vector_property (GsmClient    *client,
                                const char   *prop_name,
                                int           argc,
                                char        **argv)
{
  g_return_if_fail (prop_name != NULL);
  
  gsm_verbose ("Setting prop '%s' to a vector\n", prop_name);
  replace_one_prop (client, smprop_new_vector (prop_name, argc, argv));
}

gboolean
gsm_client_get_byte_property (GsmClient    *client,
                              const char   *prop_name,
                              guint8       *result)
{
  int tmp;
  
  g_return_val_if_fail (prop_name != NULL, FALSE);
  g_return_val_if_fail (result != NULL, FALSE);

  tmp = 0;
  
  if (proplist_find_card8 (client->priv->properties,
                           prop_name,
                           &tmp))
    {
      *result = tmp;
      return TRUE;
    }
  else
    return FALSE;
}

gboolean
gsm_client_get_string_property (GsmClient    *client,
                                const char   *prop_name,
                                char        **result)
{
  g_return_val_if_fail (prop_name != NULL, FALSE);
  g_return_val_if_fail (result != NULL, FALSE);

  return proplist_find_string (client->priv->properties,
                               prop_name,
                               result);
}

gboolean
gsm_client_get_vector_property (GsmClient    *client,
                                const char   *prop_name,
                                int          *argcp,
                                char       ***argvp)
{
  g_return_val_if_fail (prop_name != NULL, FALSE);
  g_return_val_if_fail (argcp != NULL, FALSE);
  g_return_val_if_fail (argvp != NULL, FALSE);

  return proplist_find_vector (client->priv->properties,
                               prop_name,
                               argcp, argvp);
}

void
gsm_client_set_clone_command (GsmClient *client,
                              int        argc,
                              char     **argv)
{
  gsm_client_set_vector_property (client,
                                  GSM_CLIENT_PROPERTY_CLONE_COMMAND,
                                  argc, argv);
}

void
gsm_client_get_clone_command (GsmClient *client,
                              int       *argc,
                              char    ***argv)
{
  gsm_client_get_vector_property (client,
                                  GSM_CLIENT_PROPERTY_CLONE_COMMAND,
                                  argc, argv);
}

void
gsm_client_set_restart_command (GsmClient *client,
                                int        argc,
                                char     **argv)
{
  gsm_client_set_vector_property (client,
                                  GSM_CLIENT_PROPERTY_RESTART_COMMAND,
                                  argc, argv);
}

void
gsm_client_get_restart_command (GsmClient *client,
                                int       *argc,
                                char    ***argv)
{
  gsm_client_get_vector_property (client,
                                  GSM_CLIENT_PROPERTY_RESTART_COMMAND,
                                  argc, argv);
}

void
gsm_client_set_discard_command (GsmClient *client,
                                int        argc,
                                char     **argv)
{
  gsm_client_set_vector_property (client,
                                  GSM_CLIENT_PROPERTY_DISCARD_COMMAND,
                                  argc, argv);
}

void
gsm_client_get_discard_command (GsmClient *client,
                                int       *argc,
                                char    ***argv)
{
  gsm_client_get_vector_property (client,
                                  GSM_CLIENT_PROPERTY_DISCARD_COMMAND,
                                  argc, argv);
}

void
gsm_client_set_restart_style (GsmClient       *client,
                              GsmRestartStyle  style)
{
  g_return_if_fail (style >= GSM_RESTART_IF_RUNNING &&
                    style <= GSM_RESTART_NEVER);
  
  gsm_client_set_byte_property (client,
                                GSM_CLIENT_PROPERTY_RESTART_STYLE_HINT,
                                style);
}

GsmRestartStyle
gsm_client_get_restart_style (GsmClient *client)
{
  guint8 retval = GSM_RESTART_IF_RUNNING;

  if (gsm_client_get_byte_property (client,
                                    GSM_CLIENT_PROPERTY_RESTART_STYLE_HINT,
                                    &retval))
    {
      if (retval > GSM_RESTART_NEVER)
        retval = GSM_RESTART_IF_RUNNING;
    }

  return retval;
}

void
gsm_client_set_priority (GsmClient *client,
                         int        priority)
{
  g_return_if_fail (priority >= 0 && priority <= 100);
  
  gsm_client_set_byte_property (client,
                                GSM_CLIENT_PROPERTY_PRIORITY,
                                priority);
}

int
gsm_client_get_priority (GsmClient *client)
{
  guint8 retval = 50;
  
  if (gsm_client_get_byte_property (client,
                                    GSM_CLIENT_PROPERTY_PRIORITY,
                                    &retval))
    {
      if (retval > 99)
        retval = 50;
    }

  return retval;
}

void*
gsm_client_get_smc_connection (GsmClient *client)
{
  g_return_val_if_fail (GSM_IS_CLIENT (client), NULL);
  
  return client->priv->connection;
}


/**
 * gsm_client_create_save_id:
 * @client: a #GsmClient
 * 
 * Each time an app responds to a "save" signal, it should store its
 * state under a different ID, since each independent save instance
 * may be restored. This function returns a string that may be used as
 * a config prefix for that data. The return value will be a valid
 * element of a GConf key. The returned ID is not scoped with
 * the application name, so you should add the application name
 * if you store the ID in a global namespace.
 * 
 * Return value: an allocated string 
 **/
char*
gsm_client_create_save_id (GsmClient *client)
{
  g_return_val_if_fail (GSM_IS_CLIENT (client), NULL);
  
  return g_strdup_printf ("%d-%d-%u",
                          (int) getpid (),
                          (int) time (NULL),
                          g_random_int ());
                          
}

/******************************** Smc callbacks **********************/

static void
client_save_phase_2_callback (SmcConn   smc_conn,
                              SmPointer client_data)
{
  GsmClient *client;

  (void)smc_conn;
  client = client_data;

  gsm_verbose ("Phase2 callback\n");
  
  if (client->priv->state != GSM_CLIENT_STATE_PHASE2_REQUESTED)
    return;

  client->priv->state = GSM_CLIENT_STATE_SAVING_PHASE2;

  gsm_verbose ("Emitting save signal for phase2\n");
  g_signal_emit (G_OBJECT (client),
                 signals[SAVE],
                 0,
                 /* is_phase2 */
                 TRUE);
  gsm_verbose ("Done emitting save signal for phase2\n");

  if (client->priv->state == GSM_CLIENT_STATE_SAVING_PHASE2)
    {
      /* No one requested interaction, so signal the end of the
       * save. This forces clients to do the save synchronously out of
       * the "save" signal handler. And of course we can't signal save
       * errors to the SM.
       */
      gsm_client_end_save (client, TRUE);
    }
}

static void
client_save_yourself_callback (SmcConn   smc_conn,
                               SmPointer client_data,
                               int       save_style,
                               Bool      shutdown,
                               int       interact_style,
                               Bool      fast)
{
  GsmClient *client;

  (void)smc_conn;
  client = client_data;

  gsm_verbose ("SaveYourself callback\n");

  /* FIXME this is broken; if we get SaveYourself while already saving,
   * we have to send SaveYourselfDone for the current save, then
   * handle the new SaveYourself as appropriate. This is on
   * page 5 of xsmp.PS, under SaveYourself docs
   *
   * It's unclear if we're supposed to actually finish the current
   * save, or or just bail out and restart our save with the new
   * parameters.  It's also totally unclear how to expose this in the
   * GsmClient API.  I'm thinking we just keep a queue of pending
   * SaveYourself, and as we finish each one, process the next.
   * So move the body of this callback into a process_save_yourself handler,
   * and have this function simply queue a save yourself.
   */
  
  if (client->priv->state != GSM_CLIENT_STATE_IDLE)
    {
      gsm_verbose ("SaveYourself received while in state %d, ignoring\n",
                   client->priv->state);
      return;
    }

  if (client->priv->ignore_next_save)
    {
      /* Double check that this is a section 7.2 SaveYourself: */
      
      if (save_style == SmSaveLocal && 
	  interact_style == SmInteractStyleNone &&
	  !shutdown && !fast)
	{
          gsm_verbose ("Ignoring initial SaveYourself received on connect\n");
          client->priv->ignore_next_save = FALSE;
	  /* The protocol requires this even if xsm ignores it. */
	  SmcSaveYourselfDone (client->priv->connection, TRUE);
	  return;
	}
    }

  client->priv->state = GSM_CLIENT_STATE_SAVING;
  
  client->priv->in_shutdown = shutdown != FALSE;
  client->priv->save_interaction_errors_only = interact_style != SmInteractStyleAny;

  gsm_verbose ("Emitting save signal, in_shutdown = %d errors_only = %d\n",
               client->priv->in_shutdown,
               client->priv->save_interaction_errors_only);
  
  g_signal_emit (G_OBJECT (client),
                 signals[SAVE],
                 0,
                 /* is_phase2 */
                 FALSE);
  gsm_verbose ("Done emitting save signal\n");
  
  if (client->priv->state == GSM_CLIENT_STATE_SAVING)
    {
      /* No one requested interaction or phase 2,
       * so signal the end of the save. This forces
       * clients to do the save synchronously out of
       * the "save" signal handler. And of course
       * we can't signal save errors to the SM.
       */
      gsm_client_end_save (client, TRUE);
    }
}

static void
client_die_callback (SmcConn   smc_conn,
                     SmPointer client_data)
{
  GsmClient *client;

  (void)smc_conn;
  client = client_data;

  gsm_verbose ("Die callback, emitting die signal\n");
  
  g_signal_emit (G_OBJECT (client),
                 signals[DIE],
                 0);

  gsm_verbose ("Done emitting die signal\n");
  
  gsm_client_end_save (client, FALSE);
  
  gsm_client_disconnect (client);
}

static void
client_save_complete_callback (SmcConn   smc_conn,
                               SmPointer client_data)
{
  GsmClient *client;

  (void)smc_conn;
  client = client_data;

  gsm_verbose ("SaveComplete callback\n");
  
  /* Technically, clients should remain
   * "frozen" until they receive save complete, but
   * this is so painful to implement that no one is
   * going to do it. The signal here is mostly
   * for the benefit of xsmp-control
   */

  g_signal_emit (G_OBJECT (client),
                 signals[SAVE_COMPLETE],
                 0);
}

static void
client_shutdown_cancelled_callback (SmcConn   smc_conn,
                                    SmPointer client_data)
{
  GsmClient *client;

  (void)smc_conn;
  client = client_data;

  gsm_verbose ("ShutdownCancelled callback\n");
  
  if (client->priv->state == GSM_CLIENT_STATE_IDLE)
    return;

  gsm_client_end_save (client, TRUE);
}

static void
client_interact_callback (SmcConn   smc_conn,
                          SmPointer client_data)
{
  GsmClient *client;

  (void)smc_conn;
  client = client_data;

  gsm_verbose ("Interact callback\n");
  
  if (!(client->priv->state == GSM_CLIENT_STATE_INTERACT_REQUESTED ||
        client->priv->state == GSM_CLIENT_STATE_INTERACT_REQUESTED_PHASE2))
    return;

  client->priv->interaction_granted = TRUE;

  gsm_verbose ("Emitting interaction_request_granted signal\n");
  g_signal_emit (G_OBJECT (client),
                 signals[INTERACTION_REQUEST_GRANTED],
                 0,
                 client,
                 /* errors_only */
                 client->priv->save_interaction_errors_only);
  gsm_verbose ("Done emitting interaction_request_granted signal\n");
}

/******************************** ICE goo ****************************/


static void ice_io_error_handler               (IceConn     connection);
static void ice_connection_startup_or_shutdown (IceConn     connection,
                                                IcePointer  client_data,
                                                Bool        opening,
                                                IcePointer *watch_data);

static IceIOErrorHandler ice_installed_io_error_handler;

/* This is called when data is available on an ICE connection.  */
static gboolean
process_ice_messages (GIOChannel  *channel,
                      GIOCondition condition,
                      gpointer     client_data)
{
  IceConn connection = (IceConn) client_data;
  IceProcessMessagesStatus status;

  (void)channel;
  (void)condition;
  /* This blocks infinitely sometimes. I don't know what
   * to do about it. Checking "condition" just breaks
   * session management.
   */
  status = IceProcessMessages (connection, NULL, NULL);

  if (status == IceProcessMessagesIOError)
    {      
      /* We were disconnected */
      IceSetShutdownNegotiation (connection, False);
      IceCloseConnection (connection);

      return FALSE;
    }

  return TRUE;
}

/* This is called when a new ICE connection is made or shut down.  It
 * arranges for the ICE connection to be handled via the event loop.
 */
static void
ice_connection_startup_or_shutdown (IceConn     connection,
                                    IcePointer  client_data,
                                    Bool        opening,
                                    IcePointer *watch_data)
{
  guint input_id;

  (void)client_data;
  if (opening)
    {
      GIOChannel *channel;

      gsm_verbose ("Opening new ICE connection\n");
      
      /* Make sure we don't pass on these file descriptors to any
       * exec'ed children
       */
      fcntl (IceConnectionNumber (connection), F_SETFD,
             fcntl (IceConnectionNumber (connection), F_GETFD, 0) | FD_CLOEXEC);

      channel = g_io_channel_unix_new (IceConnectionNumber (connection));
      
      input_id = g_io_add_watch (channel,
                                 G_IO_IN | G_IO_ERR,
                                 process_ice_messages,
                                 connection);
      
      g_io_channel_unref (channel);
      
      *watch_data = (IcePointer) GUINT_TO_POINTER (input_id);
    }
  else 
    {
      gsm_verbose ("Closing ICE connection\n");
      
      input_id = GPOINTER_TO_UINT ((gpointer) *watch_data);

      g_source_remove (input_id);
    }
}

/* We call any handler installed before (or after) ice_init but 
 * avoid calling the default libICE handler which does an exit().
 *
 * This means we do nothing by default, which is probably correct,
 * the connection will get closed by libICE
 */
static void
ice_io_error_handler (IceConn connection)
{
  gsm_verbose ("IO error\n");
  if (ice_installed_io_error_handler)
    (*ice_installed_io_error_handler) (connection);
} 

static SmcErrorHandler old_smc_error_handler = NULL;

static void
smc_error_handler (SmcConn       smcConn,
                   Bool          swap,
                   int           offendingMinorOpcode,
                   unsigned long offendingSequence,
                   int           errorClass,
                   int           severity,
                   SmPointer     values)
{
  gsm_verbose ("Client session management error for %s (pid %d)\n",
               g_get_prgname () ? g_get_prgname () : "(unknown app)",
               (int) getpid ());

  if (old_smc_error_handler)
    (* old_smc_error_handler) (smcConn, swap, offendingMinorOpcode,
                               offendingSequence, errorClass, severity,
                               values);
}
     
static void
ice_init (void)
{
  static gboolean ice_initted = FALSE;

  if (!ice_initted)
    {
      IceIOErrorHandler default_handler;
      
      ice_installed_io_error_handler = IceSetIOErrorHandler (NULL);
      default_handler = IceSetIOErrorHandler (ice_io_error_handler);

      if (ice_installed_io_error_handler == default_handler)
	ice_installed_io_error_handler = NULL;

      /* FIXME do we need a plain IceErrorHandler in addition to the
       * IO error handler?
       */
      
      IceAddConnectionWatch (ice_connection_startup_or_shutdown, NULL);

      ice_initted = TRUE;

      gsm_verbose ("ICE initialized\n");

      /* This isn't really ICE at all, rather libSM, but oh well. */
      old_smc_error_handler = SmcSetErrorHandler (smc_error_handler);
    }
}


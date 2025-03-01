/*
 * Copyright (C) 2018, Matthias Clasen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "portal-private.h"
#include "utils-private.h"

/**
 * SECTION:account
 * @title: Accounts
 * @short_description: basic user information
 *
 * These functions let applications query basic information about
 * the user, such as user ID, name and avatar picture.
 *
 * The underlying portal is org.freedesktop.portal.Account.
 */

typedef struct {
  XdpPortal *portal;
  XdpParent *parent;
  char *parent_handle;
  char *reason;
  GTask *task;
  guint signal_id;
  char *request_path;
  guint cancelled_id;
} AccountCall;

static void
account_call_free (AccountCall *call)
{
  if (call->parent)
    {
      call->parent->unexport (call->parent);
      _xdp_parent_free (call->parent);
    }
  g_free (call->parent_handle);

  if (call->signal_id)
    g_dbus_connection_signal_unsubscribe (call->portal->bus, call->signal_id);

  if (call->cancelled_id)
    g_signal_handler_disconnect (g_task_get_cancellable (call->task), call->cancelled_id);

  g_free (call->request_path);

  g_object_unref (call->portal);
  g_object_unref (call->task);

  g_free (call->reason);

  g_free (call);
}

static void
response_received (GDBusConnection *bus,
                   const char *sender_name,
                   const char *object_path,
                   const char *interface_name,
                   const char *signal_name,
                   GVariant *parameters,
                   gpointer data)
{
  AccountCall *call = data;
  guint32 response;
  g_autoptr(GVariant) ret = NULL;

  if (call->cancelled_id)
    {
      g_signal_handler_disconnect (g_task_get_cancellable (call->task), call->cancelled_id);
      call->cancelled_id = 0;
    }

  g_variant_get (parameters, "(u@a{sv})", &response, &ret);

  if (response == 0)
    g_task_return_pointer (call->task, g_variant_ref (ret), (GDestroyNotify)g_variant_unref);
  else if (response == 1)
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Account canceled");
  else
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_FAILED, "Account failed");

  account_call_free (call);
}

static void get_user_information (AccountCall *call);

static void
parent_exported (XdpParent *parent,
                 const char *handle,
                 gpointer data)
{
  AccountCall *call = data;
  call->parent_handle = g_strdup (handle);
  get_user_information (call);
}

static void
cancelled_cb (GCancellable *cancellable,
              gpointer data)
{
  AccountCall *call = data;

  g_dbus_connection_call (call->portal->bus,
                          PORTAL_BUS_NAME,
                          call->request_path,
                          REQUEST_INTERFACE,
                          "Close",
                          NULL,
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL, NULL, NULL);
}

static void
get_user_information (AccountCall *call)
{
  GVariantBuilder options;
  g_autofree char *token = NULL;
  GCancellable *cancellable;

  if (call->parent_handle == NULL)
    {   
      call->parent->export (call->parent, parent_exported, call);
      return;
    }

  token = g_strdup_printf ("portal%d", g_random_int_range (0, G_MAXINT));
  call->request_path = g_strconcat (REQUEST_PATH_PREFIX, call->portal->sender, "/", token, NULL);
  call->signal_id = g_dbus_connection_signal_subscribe (call->portal->bus,
                                                        PORTAL_BUS_NAME,
                                                        REQUEST_INTERFACE,
                                                        "Response",
                                                        call->request_path,
                                                        NULL,
                                                        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                        response_received,
                                                        call,
                                                        NULL);

  cancellable = g_task_get_cancellable (call->task);
  if (cancellable)
    call->cancelled_id = g_signal_connect (cancellable, "cancelled", G_CALLBACK (cancelled_cb), call);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "handle_token", g_variant_new_string (token));
  g_variant_builder_add (&options, "{sv}", "reason", g_variant_new_string (call->reason));

  g_dbus_connection_call (call->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          "org.freedesktop.portal.Account",
                          "GetUserInformation",
                          g_variant_new ("(sa{sv})", call->parent_handle, &options),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          cancellable,
                          NULL,
                          NULL);
}

/**
 * xdp_portal_get_user_information:
 * @portal: a #XdpPortal
 * @parent: (nullable): parent window information
 * @reason: (nullable) a string that can be shown in the dialog to explain
 *    why the information is needed
 * @cancellable: (nullable): optional #GCancellable
 * @callback: (scope async): a callback to call when the request is done
 * @data: (closure): data to pass to @callback
 *
 * Gets information about the user.
 *
 * When the request is done, @callback will be called. You can then
 * call xdp_portal_get_user_information_finish() to get the results.
 */
void
xdp_portal_get_user_information (XdpPortal *portal,
                                 XdpParent *parent,
                                 const char *reason,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer data)
{
  AccountCall *call = NULL;

  g_return_if_fail (XDP_IS_PORTAL (portal));

  call = g_new0 (AccountCall, 1);
  call->portal = g_object_ref (portal);
  if (parent)
    call->parent = _xdp_parent_copy (parent);
  else
    call->parent_handle = g_strdup ("");
  call->reason = g_strdup (reason);
  call->task = g_task_new (portal, cancellable, callback, data);

  get_user_information (call);
}

/**
 * xdp_portal_get_user_information_finish:
 * @portal: a #XdpPortal
 * @result: a #GAsyncResult
 * @error: return location for an error
 *
 * Finishes the get-user-information request, and returns
 * the result in the form of a #GVariant dictionary containing
 * the following fields:
 * - id `s`: the user ID
 * - name `s`: the users real name
 * - image `s`: the uri of an image file for the users avatar picture
 *
 * Returns: (transfer full): a #GVariant dictionary with user information
 */
GVariant *
xdp_portal_get_user_information_finish (XdpPortal *portal,
                                        GAsyncResult *result,
                                        GError **error)
{
  g_return_val_if_fail (XDP_IS_PORTAL (portal), NULL);
  g_return_val_if_fail (g_task_is_valid (result, portal), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

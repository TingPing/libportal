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

#define GNU_SOURCE 1

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <glib/gstdio.h>
#include <gio/gunixfdlist.h>

#include "portal-private.h"
#include "utils-private.h"

/**
 * SECTION:email
 * @title: Email
 * @short_description: composing email messages
 *
 * These functions let applications send email, by prompting
 * the user to compose a message. The email may already have
 * an address, subject, body or attachments.
 *
 * The underlying portal is org.freedesktop.portal.Email.
 */

#ifndef O_PATH
#define O_PATH 0
#endif

typedef struct {
  XdpPortal *portal;
  XdpParent *parent;
  char *parent_handle;
  char *address;
  char *subject;
  char *body;
  char **attachments;
  guint signal_id;
  GTask *task;
  char *request_path;
  guint cancelled_id;
} EmailCall;

static void
email_call_free (EmailCall *call)
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

  g_free (call->address);
  g_free (call->subject);
  g_free (call->body);
  g_strfreev (call->attachments);

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
  EmailCall *call = data;
  guint32 response;
  g_autoptr(GVariant) ret = NULL;

  if (call->cancelled_id)
    {
      g_signal_handler_disconnect (g_task_get_cancellable (call->task), call->cancelled_id);
      call->cancelled_id = 0;
    }

  g_variant_get (parameters, "(u@a{sv})", &response, &ret);

  if (response == 0)
    g_task_return_boolean (call->task, TRUE);
  else if (response == 1)
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Email canceled");
  else
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_FAILED, "Email failed");

  email_call_free (call);
}

static void compose_email (EmailCall *call);

static void
parent_exported (XdpParent *parent,
                 const char *handle,
                 gpointer data)
{
  EmailCall *call = data;
  call->parent_handle = g_strdup (handle);
  compose_email (call);
}

static void
cancelled_cb (GCancellable *cancellable,
              gpointer data)
{
  EmailCall *call = data;

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
compose_email (EmailCall *call)
{
  GVariantBuilder options;
  g_autofree char *token = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
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
  if (call->address)
    g_variant_builder_add (&options, "{sv}", "address", g_variant_new_string (call->address));
  if (call->subject)
    g_variant_builder_add (&options, "{sv}", "subject", g_variant_new_string (call->subject));
  if (call->body)
    g_variant_builder_add (&options, "{sv}", "body", g_variant_new_string (call->body));
  if (call->attachments)
    {
      GVariantBuilder attach_fds;
      int i;

      fd_list = g_unix_fd_list_new ();
      g_variant_builder_init (&attach_fds, G_VARIANT_TYPE ("ah"));

      for (i = 0; call->attachments[i]; i++)
        {
          g_autoptr(GError) error = NULL;
          int fd;
          int fd_in;

          fd = g_open (call->attachments[i], O_PATH | O_CLOEXEC);
          if (fd == -1)
            {
              g_warning ("Failed to open %s, skipping", call->attachments[i]);
              continue;
            }
          fd_in = g_unix_fd_list_append (fd_list, fd, &error);
          if (error)
            {
              g_warning ("Failed to add %s to request, skipping: %s", call->attachments[i], error->message);
              continue;
            }
          g_variant_builder_add (&attach_fds, "h", fd_in);
        }

      g_variant_builder_add (&options, "{sv}", "attachment_fds", g_variant_builder_end (&attach_fds));
    }
  
  g_dbus_connection_call_with_unix_fd_list (call->portal->bus,
                                            PORTAL_BUS_NAME,
                                            PORTAL_OBJECT_PATH,
                                            "org.freedesktop.portal.Email",
                                            "ComposeEmail",
                                            g_variant_new ("(sa{sv})", call->parent_handle, &options),
                                            NULL,
                                            G_DBUS_CALL_FLAGS_NONE,
                                            -1,
                                            fd_list,
                                            cancellable,
                                            NULL,
                                            NULL);
}

/**
 * xdp_portal_compose_email:
 * @portal: a #XdpPortal
 * @parent: (nullable): parent window information
 * @address: (nullable): the email address to send to
 * @subject: (nullable): the subject for the email
 * @body: (nullable): the body for the email
 * @attachments: (nullable): an array of paths for files to attach
 * @cancellable: (nullable): optional #GCancellable
 * @callback: (scope async): a callback to call when the request is done
 * @data: (closure): data to pass to @callback
 *
 * Presents a window that lets the user compose an email,
 * with some pre-filled information.
 *
 * When the request is done, @callback will be called. You can then
 * call xdp_portal_compose_email_finish() to get the results.
 */
void
xdp_portal_compose_email (XdpPortal *portal,
                          XdpParent *parent,
                          const char *address,
                          const char *subject,
                          const char *body,
                          const char *const *attachments,
                          GCancellable *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer data)
{
  EmailCall *call;

  g_return_if_fail (XDP_IS_PORTAL (portal));

  call = g_new0 (EmailCall, 1);
  call->portal = g_object_ref (portal);
  if (parent)
    call->parent = _xdp_parent_copy (parent);
  else
    call->parent_handle = g_strdup ("");
  call->address = g_strdup (address);
  call->subject = g_strdup (subject);
  call->body = g_strdup (body);
  call->attachments = g_strdupv ((char **)attachments);
  call->task = g_task_new (portal, cancellable, callback, data);

  compose_email (call);
}

/**
 * xdp_portal_compose_email_finish:
 * @portal: a #XdpPortal
 * @result: a #GAsyncResult
 * @error: return location for an error
 *
 * Finishes the compose-email request.
 *
 * Returns: %TRUE if the request was handled successfully
 */
gboolean
xdp_portal_compose_email_finish (XdpPortal *portal,
                                 GAsyncResult *result,
                                 GError **error)
{
  g_return_val_if_fail (XDP_IS_PORTAL (portal), FALSE);
  g_return_val_if_fail (g_task_is_valid (result, portal), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

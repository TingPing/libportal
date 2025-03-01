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
 * SECTION:screenshot
 * @title: Screenshot
 * @short_description: take a screenshot
 *
 * These functions let the application take a screenshot or pick a color.
 * 
 * The underlying portal is org.freedesktop.portal.Screenshot.
 */

typedef struct {
  XdpPortal *portal;
  XdpParent *parent;
  char *parent_handle;
  gboolean color;
  gboolean modal;
  gboolean interactive;
  guint signal_id;
  GTask *task;
  char *request_path;
  guint cancelled_id;
} ScreenshotCall;

static void
screenshot_call_free (ScreenshotCall *call)
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
  ScreenshotCall *call = data;
  guint32 response;
  g_autoptr(GVariant) ret = NULL;

  if (call->cancelled_id)
    {
      g_signal_handler_disconnect (g_task_get_cancellable (call->task), call->cancelled_id);
      call->cancelled_id = 0;
    }

  g_variant_get (parameters, "(u@a{sv})", &response, &ret);

  if (response == 0)
    {
      if (call->color)
        {
          g_autoptr(GVariant) color = NULL;
          g_variant_lookup (ret, "color", "@(ddd)", &color);
          if (color)
            g_task_return_pointer (call->task, g_variant_ref (color), (GDestroyNotify) g_variant_unref);
          else
            g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_FAILED, "Color not received");
        }
      else
        {
          const char *uri;
          g_variant_lookup (ret, "uri", "&s", &uri);
          if (uri)
            g_task_return_pointer (call->task, g_strdup (uri), g_free);
          else
            g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_FAILED, "Screenshot not received");
       }
    }
  else if (response == 1)
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Screenshot canceled");
  else
    g_task_return_new_error (call->task, G_IO_ERROR, G_IO_ERROR_FAILED, "Screenshot failed");

  screenshot_call_free (call);
}

static void take_screenshot (ScreenshotCall *call);

static void
parent_exported (XdpParent *parent,
                 const char *handle,
                 gpointer data)
{
  ScreenshotCall *call = data;
  call->parent_handle = g_strdup (handle);
  take_screenshot (call);  
}

static void
cancelled_cb (GCancellable *cancellable,
              gpointer data)
{
  ScreenshotCall *call = data;

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
take_screenshot (ScreenshotCall *call)
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
  if (!call->color)
    {
      g_variant_builder_add (&options, "{sv}", "modal", g_variant_new_boolean (call->modal));
      g_variant_builder_add (&options, "{sv}", "interactive", g_variant_new_boolean (call->interactive));
    }

  g_dbus_connection_call (call->portal->bus,
                          PORTAL_BUS_NAME,
                          PORTAL_OBJECT_PATH,
                          "org.freedesktop.portal.Screenshot",
                          call->color ? "PickColor" : "Screenshot",
                          g_variant_new ("(sa{sv})", call->parent_handle, &options),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          cancellable,
                          NULL,
                          NULL);
}

/**
 * xdp_portal_take_screenshot:
 * @portal: a #XdpPortal
 * @parent: (nullable): parent window information
 * @modal: whether to presend a modal dialog
 * @interactive: whether the dialog should offer options
 * @cancellable: (nullable): optional #GCancellable
 * @callback: (scope async): a callback to call when the request is done
 * @data: (closure): data to pass to @callback
 *
 * Takes a screenshot.
 * 
 * When the request is done, @callback will be called. You can then
 * call xdp_portal_take_screenshot_finish() to get the results.
 */
void
xdp_portal_take_screenshot (XdpPortal *portal,
                            XdpParent *parent,
                            gboolean modal,
                            gboolean interactive,
                            GCancellable *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer data)
{
  ScreenshotCall *call;

  g_return_if_fail (XDP_IS_PORTAL (portal));

  call = g_new0 (ScreenshotCall, 1);
  call->color = FALSE;
  call->portal = g_object_ref (portal);
  if (parent)
    call->parent = _xdp_parent_copy (parent);
  else
    call->parent_handle = g_strdup ("");
  call->modal = modal;
  call->interactive = interactive;
  call->task = g_task_new (portal, cancellable, callback, data);

  take_screenshot (call);
}

/**
 * xdp_portal_take_screenshot_finish:
 * @portal: a #XdpPortal
 * @result: a #GAsyncResult
 * @error: return location for an error
 *
 * Finishes a screenshot request, and returns
 * the result in the form of a URI pointing to an image file.
 *
 * Returns: (transfer full): URI pointing to an image file
 */
char *
xdp_portal_take_screenshot_finish (XdpPortal *portal,
                                   GAsyncResult *result,
                                   GError **error)
{
  g_return_val_if_fail (XDP_IS_PORTAL (portal), NULL);
  g_return_val_if_fail (g_task_is_valid (result, portal), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/**
 * xdp_portal_pick_color:
 * @portal: a #XdpPortal
 * @parent: (nullable): parent window information
 * @cancellable: (nullable): optional #GCancellable
 * @callback: (scope async): a callback to call when the request is done
 * @data: (closure): data to pass to @callback
 *
 * Lets the user pick a color from the screen.
 * 
 * When the request is done, @callback will be called. You can then
 * call xdp_portal_pick_color_finish() to get the results.
 */
void
xdp_portal_pick_color (XdpPortal *portal,
                       XdpParent *parent,
                       GCancellable *cancellable,
                       GAsyncReadyCallback  callback,
                       gpointer data)
{
  ScreenshotCall *call;

  g_return_if_fail (XDP_IS_PORTAL (portal));

  call = g_new0 (ScreenshotCall, 1);
  call->color = TRUE;
  call->portal = g_object_ref (portal);
  if (parent)
    call->parent = _xdp_parent_copy (parent);
  else
    call->parent_handle = g_strdup ("");
  call->task = g_task_new (portal, cancellable, callback, data);

  take_screenshot (call);
}

/**
 * xdp_portal_pick_color_finish:
 * @portal: a #XdpPortal
 * @result: a #GAsyncResult
 * @error: return location for an error
 *
 * Finishes a pick-color request, and returns
 * the result in the form of a GVariant of the form (ddd), containing
 * red, green and blue components in the range [0,1].
 *
 * Returns: (transfer full): GVariant containing the color
 */
GVariant *
xdp_portal_pick_color_finish (XdpPortal *portal,
                              GAsyncResult *result,
                              GError **error)
{
  GVariant *ret;

  g_return_val_if_fail (XDP_IS_PORTAL (portal), NULL);
  g_return_val_if_fail (g_task_is_valid (result, portal), NULL);

  ret = (GVariant *) g_task_propagate_pointer (G_TASK (result), error);
  return ret ? g_variant_ref (ret) : NULL; 
}

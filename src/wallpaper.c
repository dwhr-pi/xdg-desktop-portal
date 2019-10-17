/*
 * Copyright © 2019 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
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
 *
 * Authors:
 *       Felipe Borges <feborges@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>

#include "wallpaper.h"
#include "permissions.h"
#include "request.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define PERMISSION_TABLE "wallpaper"
#define PERMISSION_ID "wallpaper"

typedef struct _Wallpaper Wallpaper;
typedef struct _WallpaperClass WallpaperClass;

struct _Wallpaper
{
  XdpWallpaperSkeleton parent_instance;
};

struct _WallpaperClass
{
  XdpWallpaperSkeletonClass parent_class;
};

static XdpImplWallpaper *impl;
static XdpImplAccess *access_impl;
static Wallpaper *wallpaper;

GType wallpaper_get_type (void) G_GNUC_CONST;
static void wallpaper_iface_init (XdpWallpaperIface *iface);

G_DEFINE_TYPE_WITH_CODE (Wallpaper, wallpaper, XDP_TYPE_WALLPAPER_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_WALLPAPER, wallpaper_iface_init));

static gboolean
get_set_wallpaper_allowed (const gchar *app_id)
{
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;
  g_autoptr(GError) error = NULL;

  if (!xdp_impl_permission_store_call_lookup_sync (get_permission_store (),
                                                   PERMISSION_TABLE,
                                                   PERMISSION_ID,
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_debug ("No wallpaper permissions found: %s", error->message);
      g_clear_error (&error);
    }

  if (out_perms != NULL)
    {
      const gchar **perms;
      if (g_variant_lookup (out_perms, app_id, "^a&s", &perms))
        {
          g_autofree gchar *a = g_strjoinv (" ", (gchar **)perms);

          g_debug ("Wallpaper permissions for %s: %s", app_id, a);

          return g_strv_contains (perms, "yes");
        }
    }

  return FALSE;
}

static void
authorize_app (const gchar *app_id)
{
  g_autoptr(GError) error = NULL;
  const char *permissions[2];

  permissions[0] = "yes";
  permissions[1] = NULL;

  if (!xdp_impl_permission_store_call_set_permission_sync (get_permission_store (),
                                                          PERMISSION_TABLE,
                                                          TRUE,
                                                          PERMISSION_ID,
                                                          app_id,
                                                          (const char * const*)permissions,
                                                          NULL,
                                                          &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Error updating permission store: %s", error->message);
    }
}

static void
send_response (Request *request,
               guint response)
{
  if (request->exported)
    {
      GVariantBuilder opt_builder;

      g_debug ("sending response: %d", response);
      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
      xdp_request_emit_response (XDP_REQUEST (request),
                                 response,
                                 g_variant_builder_end (&opt_builder));
      request_unexport (request);
    }
}

static void
handle_set_wallpaper_uri_done (GObject *source,
                               GAsyncResult *result,
                               gpointer data)
{
  g_autoptr(Request) request = data;
  guint response = 2;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;

  if (!xdp_impl_wallpaper_call_set_wallpaper_uri_finish (XDP_IMPL_WALLPAPER (source),
                                                         &response,
                                                         result,
                                                         &error))
    {
      g_warning ("A backend call failed: %s", error->message);
    }
}

static gboolean
validate_set_on (const char *key,
                 GVariant *value,
                 GVariant *options,
                 GError **error)
{
  const char *string = g_variant_get_string (value, NULL);

  return ((g_strcmp0 (string, "both") == 0) ||
          (g_strcmp0 (string, "background") == 0) ||
          (g_strcmp0 (string, "lockscreen") == 0));
}

static XdpOptionKey wallpaper_options[] = {
  { "show-preview", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "set-on", G_VARIANT_TYPE_STRING, validate_set_on }
};

static void
handle_set_wallpaper_in_thread_func (GTask *task,
                                     gpointer source_object,
                                     gpointer task_data,
                                     GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  const char *parent_window;
  const char *app_id = xdp_app_info_get_id (request->app_info);
  g_autoptr(GError) error = NULL;
  g_autofree char *uri = NULL;
  g_autofree char *basename = NULL;
  GVariantBuilder opt_builder;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  g_autoptr(GVariant) options = NULL;
  gboolean show_preview = FALSE;
  int fd;

  parent_window = ((const char *)g_object_get_data (G_OBJECT (request), "parent-window"));
  uri = g_strdup ((const char *)g_object_get_data (G_OBJECT (request), "uri"));
  fd = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (request), "fd"));
  options = ((GVariant *)g_object_get_data (G_OBJECT (request), "options"));

  REQUEST_AUTOLOCK (request);

  if (app_id[0] == '\0' && getenv("TEST_WALLPAPER"))
    app_id = "org.gnome.PortalTest";

  g_variant_lookup (options, "show-preview", "b", &show_preview);
  if (!show_preview && !get_set_wallpaper_allowed (app_id))
    {
      guint access_response = 2;
      g_autoptr(GVariant) access_results = NULL;
      GVariantBuilder access_opt_builder;
      g_autofree gchar *title = NULL;
      g_autofree gchar *subtitle = NULL;
      const gchar *body;

      g_variant_builder_init (&access_opt_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "deny_label", g_variant_new_string (_("Deny")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "grant_label", g_variant_new_string (_("Allow")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "icon", g_variant_new_string ("preferences-desktop-wallpaper-symbolic"));

      if (g_str_equal (app_id, ""))
        {
          title = g_strdup (_("Allow Applications to Set Backgrounds?"));
          subtitle = g_strdup (_("An application is requesting to be able to change the background image."));
        }
      else
        {
          g_autoptr(GDesktopAppInfo) info = NULL;
          const gchar *id;
          const gchar *name;

          id = g_strconcat (app_id, ".desktop", NULL);
          info = g_desktop_app_info_new (id);
          name = g_app_info_get_display_name (G_APP_INFO (info));

          title = g_strdup_printf (_("Allow %s to Set Backgrounds?"), name);
          subtitle = g_strdup_printf (_("%s is requesting to be able to change the background image."), name);
        }

      body = _("This permission can be changed at any time from the privacy settings.");

      if (!xdp_impl_access_call_access_dialog_sync (access_impl,
                                                    request->id,
                                                    app_id,
                                                    parent_window,
                                                    title,
                                                    subtitle,
                                                    body,
                                                    g_variant_builder_end (&access_opt_builder),
                                                    &access_response,
                                                    &access_results,
                                                    NULL,
                                                    &error))
        {
          g_warning ("Failed to show access dialog: %s", error->message);
          return;
        }

      if (access_response == 0)
        {
          authorize_app (app_id);
        }
      else
        {
          send_response (request, 2);

          return;
        }
    }

  if (!uri)
    {
      g_autofree char *path = NULL;

      path = xdp_app_info_get_path_for_fd (request->app_info, fd, 0, NULL, NULL);
      if (path == NULL)
        {
          /* Reject the request */
          if (request->exported)
            {
              xdp_request_emit_response (XDP_REQUEST (request),
                                         XDG_DESKTOP_PORTAL_RESPONSE_OTHER,
                                         NULL);
              request_unexport (request);
            }
          return;
        }

      basename = g_path_get_basename (path);

      uri = g_filename_to_uri (path, NULL, NULL);
      g_object_set_data_full (G_OBJECT (request), "uri", g_strdup (uri), g_free);
    }

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                  request->id,
                                                  NULL, &error);
  request_set_impl_request (request, impl_request);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_filter_options (options, &opt_builder,
                      wallpaper_options, G_N_ELEMENTS (wallpaper_options),
                      NULL);

  xdp_impl_wallpaper_call_set_wallpaper_uri (impl,
                                             request->id,
                                             app_id,
                                             parent_window,
                                             uri,
                                             g_variant_builder_end (&opt_builder),
                                             NULL,
                                             handle_set_wallpaper_uri_done,
                                             g_object_ref (request));
}

static gboolean
handle_set_wallpaper_uri (XdpWallpaper *object,
                          GDBusMethodInvocation *invocation,
                          const char *arg_parent_window,
                          const char *arg_uri,
                          GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;

  g_object_set_data_full (G_OBJECT (request), "uri", g_strdup (arg_uri), g_free);
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);
  g_object_set_data_full (G_OBJECT (request),
                          "options",
                          g_variant_ref (arg_options),
                          (GDestroyNotify)g_variant_unref);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdp_wallpaper_complete_set_wallpaper_uri (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_set_wallpaper_in_thread_func);

  return TRUE;  
}

static gboolean
handle_set_wallpaper_file (XdpWallpaper *object,
                           GDBusMethodInvocation *invocation,
                           GUnixFDList *fd_list,
                           const char *arg_parent_window,
                           GVariant *arg_fd,
                           GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GTask) task = NULL;
  int fd_id, fd;
  g_autoptr(GError) error = NULL;

  g_variant_get (arg_fd, "h", &fd_id);
  fd = g_unix_fd_list_get (fd_list, fd_id, &error);
  if (fd == -1)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  g_object_set_data (G_OBJECT (request), "fd", GINT_TO_POINTER (fd));
  g_object_set_data_full (G_OBJECT (request), "parent-window", g_strdup (arg_parent_window), g_free);
  g_object_set_data_full (G_OBJECT (request),
                          "options",
                          g_variant_ref (arg_options),
                          (GDestroyNotify)g_variant_unref);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdp_wallpaper_complete_set_wallpaper_file (object, invocation, NULL, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_set_wallpaper_in_thread_func);

  return TRUE;
}
static void
wallpaper_iface_init (XdpWallpaperIface *iface)
{
  iface->handle_set_wallpaper_uri = handle_set_wallpaper_uri;
  iface->handle_set_wallpaper_file = handle_set_wallpaper_file;
}

static void
wallpaper_init (Wallpaper *wallpaper)
{
  xdp_wallpaper_set_version (XDP_WALLPAPER (wallpaper), 1);
}

static void
wallpaper_class_init (WallpaperClass *klass)
{
}

GDBusInterfaceSkeleton *
wallpaper_create (GDBusConnection *connection,
		  const char *dbus_name_wallpaper)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_wallpaper_proxy_new_sync (connection,
                                            G_DBUS_PROXY_FLAGS_NONE,
                                            dbus_name_wallpaper,
                                            DESKTOP_PORTAL_OBJECT_PATH,
                                            NULL,
                                            &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create wallpaper proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (impl), G_MAXINT);
  wallpaper = g_object_new (wallpaper_get_type (), NULL);

  access_impl = xdp_impl_access_proxy_new_sync (connection,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                dbus_name_wallpaper,
                                                DESKTOP_PORTAL_OBJECT_PATH,
                                                NULL,
                                                &error);

  return G_DBUS_INTERFACE_SKELETON (wallpaper);
}

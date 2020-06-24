/*  webdav-cb.c - common code for x11 and GTK+ backend handling webdav copy&paste

    Copyright 2020 Red Hat, Inc.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <syslog.h>

#include "webdav-cb.h"

/* FIXME:
 * From my testing, gvfs-dav with avahi doesn't seem stable enough,
 * so let's simply use the usual port 9843 when mounting the shared folder for now.
 *
 * This is a bit unfortunate because the port can be customized with the -p option,
 * while the service name "Spice client folder" is hardcoded.
 *
 * Issues with gvfs-dav and avahi:
 * - https://bugzilla.redhat.com/show_bug.cgi?id=1843035
 *   - similar to https://bugzilla.redhat.com/show_bug.cgi?id=1773219
 * - https://gitlab.gnome.org/GNOME/gvfs/-/issues/498
 *   - hence the %2520 in CLIPBOARD_WEBDAV_URI below
 * - fixed recently
 *   - https://gitlab.gnome.org/GNOME/gvfs/-/issues/449
 */
// #define CLIPBOARD_WEBDAV_URI "dav+sd://Spice%2520client%2520folder._webdav._tcp.local"
#define CLIPBOARD_WEBDAV_URI "dav://localhost:9843"

typedef enum clipboard_action {
    CLIPBOARD_ACTION_COPY,
    CLIPBOARD_ACTION_CUT,
} clipboard_action;

static GMount *webdav_mount;
static GVolumeMonitor *monitor;
static GCancellable *cancellable;

static gchar *clipboard_data_to_uris(const gchar *target, const gchar *mount_uri,
    const gchar *data, gsize size, GError **err)
{
    if (!data || size < 1) {
        /* this is valid input */
        return NULL;
    }
    if (data[size-1]) {
        g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
            "received list of uris that is not null-terminated");
        return NULL;
    }

    clipboard_action action;
    if (!g_strcmp0(data, "copy")) {
        action = CLIPBOARD_ACTION_COPY;
    } else if (!g_strcmp0(data, "cut")) {
        action = CLIPBOARD_ACTION_CUT;
    } else {
        g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "first line of uri list must specify clipboard action");
        return NULL;
    }
    /* skip the action line, since we're only interested in
     * the actual uris from this point forward  */
    gsize action_len = strlen(data) + 1;
    data += action_len;
    size -= action_len;

    if (size < 1) {
        return NULL;
    }

    GString *str = g_string_new(NULL);
    const gchar *delimiter = "\n";
    gboolean end_with_delimiter = FALSE;

    /* TODO: add support for more file managers
     * (and then update clipboard_format_tmpl in x11-priv.h) */
    if (!g_strcmp0(target, "text/uri-list")) {
        delimiter = "\r\n";
        if (action == CLIPBOARD_ACTION_CUT) {
            syslog(LOG_WARNING, "cutting is not supported with 'text/uri-list' target");
        }
    } else if (!g_strcmp0(target, "text/plain;charset=utf-8")) {
        /* Nautilus uses text clipboard since
         * https://gitlab.gnome.org/GNOME/nautilus/commit/1f77023b5769c773dd9261e5294c0738bf6a3115 */
        end_with_delimiter = TRUE;
        g_string_append(str, "x-special/nautilus-clipboard\n");
        g_string_append (str, action == CLIPBOARD_ACTION_CUT ? "cut\n" : "copy\n");
    } else if (!g_strcmp0(target, "application/x-kde-cutselection")) {
        /* KDE Dolphin handles text/uri-list just fine,
         * but this atom is needed to distinguish between copy and move */
        g_string_append(str, action == CLIPBOARD_ACTION_CUT ? "1" : "0");
        return g_string_free(str, FALSE);
    } else if (!g_strcmp0(target, "x-special/gnome-copied-files") ||
               !g_strcmp0(target, "x-special/mate-copied-files")) {
        /* Nautilus moved away from this approach,
         * but there's a bunch of other file managers that do use it, such as:
         * Nemo (Cinnamon), Thunar (Xfce), Deepin File Manager (Deepin), Xfe; Caja (Mate) */
        g_string_append(str, action == CLIPBOARD_ACTION_CUT ? "cut\n" : "copy\n");
    } else {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "conversion to uri target %s is not supported", target);
        return g_string_free(str, TRUE);
    }

    for (const gchar *item = data; item < data + size; item += strlen(item) + 1) {
        gchar *escaped = g_uri_escape_string(item, G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, TRUE);
        gchar *uri = g_build_filename(mount_uri, escaped, NULL);
        g_string_append(str, uri);
        g_string_append(str, delimiter);
        g_free(escaped);
        g_free(uri);
    }

    if (!end_with_delimiter) {
        g_string_truncate(str, str->len - strlen(delimiter));
    }

    return g_string_free(str, FALSE);
}

static gchar *clipboard_webdav_mount_get_uri(GMount *mount, GError **err)
{
    GFile *root;
    gchar *path, *uri;

    root = g_mount_get_root(mount);
    path = g_file_get_path(root);

    if (path) {
        /* gvfs-fuse is running, so we get path as follows:
         * "/run/user/<UID>/gvfs/dav+sd:host=SpiceClipboard._webdav._tcp.local"
         * but we still need to convert it to uri */
        uri = g_filename_to_uri(path, NULL, err);
        g_free(path);
    } else {
        /* gvfs-fuse is not running, let's return the CLIPBOARD_WEBDAV_URI ("dav+sd://..."),
         * so that at least gio apps can access the shared files  */
        uri = g_file_get_uri(root);
        syslog(LOG_WARNING, "gvfs-fuse doesn't seem to be running, "
                            "file copy functionality may be limited");
    }
    g_object_unref(root);

    return uri;
}

static void resolve_task(GTask *task, const gchar *target, GBytes *data)
{
    GError *err = NULL;
    gchar *mount_uri, *uris;

    mount_uri = clipboard_webdav_mount_get_uri(webdav_mount, &err);
    if (!mount_uri) {
        g_task_return_error(task, err);
        g_object_unref(task);
        return;
    }

    uris = clipboard_data_to_uris(target, mount_uri,
        g_bytes_get_data(data, NULL), g_bytes_get_size(data), &err);
    g_free(mount_uri);
    if (err) {
        g_task_return_error(task, err);
        g_object_unref(task);
        return;
    }

    g_task_return_pointer(task, uris, g_free);
    g_object_unref(task);
}

static void unmounted_cb(GMount *mount, gpointer user_data)
{
    syslog(LOG_DEBUG, "%s unmounted", CLIPBOARD_WEBDAV_URI);
    g_clear_object(&webdav_mount);
}

static void mount_found_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GTask *task = user_data;
    GError *err = NULL;

    webdav_mount = g_file_find_enclosing_mount_finish(G_FILE(source), res, &err);

    if (err) {
        g_task_return_error(task, err);
        g_object_unref(task);
    }
    syslog(LOG_DEBUG, "mount %s found", CLIPBOARD_WEBDAV_URI);

    g_signal_connect(webdav_mount, "unmounted", G_CALLBACK(unmounted_cb), NULL);

    gchar *target = g_object_get_data(G_OBJECT(task), "target");
    GBytes *data = g_task_get_task_data(task);
    resolve_task(task, target, data);
}

static void mounted_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GFile *f = G_FILE(source);
    GTask *task = user_data;
    GError *err = NULL;

    g_file_mount_enclosing_volume_finish(f, res, &err);

    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) {
        g_clear_error(&err);
    } else if (err) {
        g_task_return_error(task, err);
        g_object_unref(task);
        return;
    }
    syslog(LOG_DEBUG, "%s mounted successfully", CLIPBOARD_WEBDAV_URI);

    g_file_find_enclosing_mount_async(f, G_PRIORITY_DEFAULT, cancellable, mount_found_cb, task);
}

static void clipboard_webdav_mount_async(GTask *task)
{
    g_return_if_fail(webdav_mount == NULL);

    syslog(LOG_DEBUG, "mounting %s", CLIPBOARD_WEBDAV_URI);

    GFile *f = g_file_new_for_uri(CLIPBOARD_WEBDAV_URI);
    g_file_mount_enclosing_volume(f,
                                  G_MOUNT_MOUNT_NONE,
                                  NULL, /* GMountOperation */
                                  cancellable,
                                  mounted_cb,
                                  task);
    g_object_unref(f);
}

gchar *clipboard_data_translate_to_uris_finish(GObject *source,
    GAsyncResult *res, gsize *size, GError **err)
{
    *size = 0;
    g_return_val_if_fail(g_task_is_valid(res, source), NULL);

    gchar *uris = g_task_propagate_pointer(G_TASK(res), err);
    if (uris) {
        *size = strlen(uris);
    }
    return uris;
}

void clipboard_data_translate_to_uris_async(const gchar *target, GBytes *data,
    GCancellable *cancel, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new(NULL, cancel, callback, user_data);

    if (!webdav_mount) {
        g_task_set_task_data(task, g_bytes_ref(data), (GDestroyNotify)g_bytes_unref);
        g_object_set_data_full(G_OBJECT(task), "target", g_strdup(target), g_free);
        clipboard_webdav_mount_async(task);
        return;
    }

    resolve_task(task, target, data);
}

void clipboard_webdav_init()
{
    /* we listen to the "unmounted" signal,
     * but without the GVolumeMonitor, the signal is not emitted,
     * although the docs don't seem to mention it!
     * https://gitlab.gnome.org/GNOME/gvfs/-/issues/494 */

    monitor = g_volume_monitor_get();
    webdav_mount = NULL;
    cancellable = g_cancellable_new();
}

void clipboard_webdav_finalize()
{
    g_cancellable_cancel(cancellable);
    g_clear_object(&cancellable);
    g_clear_object(&webdav_mount);
    g_clear_object(&monitor);
}

/*  webdav-cb.h

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

#pragma once

#include <gio/gio.h>

void clipboard_webdav_init();
void clipboard_webdav_finalize();

/* converts the @data received from spice-gtk to the given @target;
 * supported targets are:
 * - "text/uri-list"
 * - "text/plain;charset=utf-8"
 * - "application/x-kde-cutselection"
 * - "x-special/gnome-copied-files"
 * - "x-special/mate-copied-files"
 */
void clipboard_data_translate_to_uris_async(const gchar *target, GBytes *data,
    GCancellable *cancel, GAsyncReadyCallback callback, gpointer user_data);

gchar *clipboard_data_translate_to_uris_finish(GObject *source,
    GAsyncResult *res, gsize *size, GError **err);

/*  vdagent-connection.c

    Copyright 2019 Red Hat, Inc.

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
#include <fcntl.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <gio/gunixsocketaddress.h>

#include "vdagent-connection.h"

typedef struct {
    GIOStream         *io_stream;
    gboolean           opening;
    VDAgentConnErrorCb error_cb;
    GCancellable      *cancellable;

    GQueue            *write_queue;
    gsize              bytes_written;

    gsize              header_size;
    gpointer           header_buf;
    gpointer           data_buf;
} VDAgentConnectionPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(VDAgentConnection, vdagent_connection, G_TYPE_OBJECT)

static void read_next_message(VDAgentConnection *self);

GIOStream *vdagent_file_open(const gchar *path, GError **err)
{
    gint fd, errsv;

    fd = g_open(path, O_RDWR);
    if (fd == -1) {
        errsv = errno;
        g_set_error_literal(err, G_FILE_ERROR,
                            g_file_error_from_errno(errsv),
                            g_strerror(errsv));
        return NULL;
    }

    return g_simple_io_stream_new(g_unix_input_stream_new(fd, TRUE),
                                  g_unix_output_stream_new(fd, TRUE));
}

GIOStream *vdagent_socket_connect(const gchar *path, GError **err)
{
    GSocketConnection *conn;
    GSocketAddress *addr;
    GSocketClient *client;

    addr = g_unix_socket_address_new(path);
    client = g_object_new(G_TYPE_SOCKET_CLIENT,
                          "family", G_SOCKET_FAMILY_UNIX,
                          "type", G_SOCKET_TYPE_STREAM,
                          NULL);
    conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, err);
    g_object_unref(client);
    g_object_unref(addr);
    return G_IO_STREAM(conn);
}

static void vdagent_connection_init(VDAgentConnection *self)
{
    VDAgentConnectionPrivate *priv = vdagent_connection_get_instance_private(self);
    priv->cancellable = g_cancellable_new();
    priv->write_queue = g_queue_new();
}

static void vdagent_connection_dispose(GObject *obj)
{
    VDAgentConnection *self = VDAGENT_CONNECTION(obj);
    VDAgentConnectionPrivate *priv = vdagent_connection_get_instance_private(self);

    g_clear_object(&priv->cancellable);
    g_clear_object(&priv->io_stream);

    G_OBJECT_CLASS(vdagent_connection_parent_class)->dispose(obj);
}

static void vdagent_connection_finalize(GObject *obj)
{
    VDAgentConnection *self = VDAGENT_CONNECTION(obj);
    VDAgentConnectionPrivate *priv = vdagent_connection_get_instance_private(self);

    g_queue_free_full(priv->write_queue, (GDestroyNotify)g_bytes_unref);
    g_free(priv->header_buf);
    g_free(priv->data_buf);

    G_OBJECT_CLASS(vdagent_connection_parent_class)->finalize(obj);
}

static void vdagent_connection_class_init(VDAgentConnectionClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    gobject_class->dispose  = vdagent_connection_dispose;
    gobject_class->finalize = vdagent_connection_finalize;
}

void vdagent_connection_setup(VDAgentConnection *self,
                              GIOStream         *io_stream,
                              gboolean           wait_on_opening,
                              gsize              header_size,
                              VDAgentConnErrorCb error_cb)
{
    VDAgentConnectionPrivate *priv = vdagent_connection_get_instance_private(self);
    priv->io_stream = io_stream;
    priv->opening = wait_on_opening;
    priv->header_size = header_size;
    priv->header_buf = g_malloc(header_size);
    priv->error_cb = error_cb;

    read_next_message(self);
}

void vdagent_connection_destroy(gpointer p)
{
    g_return_if_fail(VDAGENT_IS_CONNECTION(p));

    VDAgentConnection *self = VDAGENT_CONNECTION(p);
    VDAgentConnectionPrivate *priv = vdagent_connection_get_instance_private(self);
    g_cancellable_cancel(priv->cancellable);
    g_io_stream_close(priv->io_stream, NULL, NULL);
    g_object_unref(self);
}

gint vdagent_connection_get_peer_pid(VDAgentConnection *self,
                                     GError           **err)
{
    VDAgentConnectionPrivate *priv = vdagent_connection_get_instance_private(self);
    GSocket *sock;
    GCredentials *cred;
    gint pid = -1;

    g_return_val_if_fail(G_IS_SOCKET_CONNECTION(priv->io_stream), pid);

    sock = g_socket_connection_get_socket(G_SOCKET_CONNECTION(priv->io_stream));
    cred = g_socket_get_credentials(sock, err);
    if (cred) {
        pid = g_credentials_get_unix_pid(cred, NULL);
        g_object_unref(cred);
    }

    return pid;
}

/* Performs single write operation,
 * returns TRUE if there's still data to be written, otherwise FALSE. */
static gboolean do_write(VDAgentConnection *self, gboolean block)
{
    VDAgentConnectionPrivate *priv = vdagent_connection_get_instance_private(self);
    GOutputStream *out;
    GBytes *msg;
    gssize res;
    GError *err = NULL;

    msg = g_queue_peek_head(priv->write_queue);
    out = g_io_stream_get_output_stream(priv->io_stream);

    if (!msg) {
        return FALSE;
    }

    res = g_pollable_stream_write(out,
        g_bytes_get_data(msg, NULL) + priv->bytes_written,
        g_bytes_get_size(msg) - priv->bytes_written,
        block, priv->cancellable, &err);

    if (err) {
        if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
            g_error_free(err);
            return TRUE;
        } else if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_error_free(err);
            return FALSE;
        } else {
            priv->error_cb(self, err);
            return FALSE;
        }
    }

    priv->bytes_written += res;

    if (priv->bytes_written == g_bytes_get_size(msg)) {
        g_bytes_unref(g_queue_pop_head(priv->write_queue));
        priv->bytes_written = 0;
    }

    return !g_queue_is_empty(priv->write_queue);
}

static gboolean out_stream_ready_cb(GObject *pollable_stream,
                                    gpointer user_data)
{
    if (do_write(user_data, FALSE)) {
        return TRUE;
    }
    g_object_unref(user_data);
    return FALSE;
}

void vdagent_connection_write(VDAgentConnection *self,
                              gpointer           data,
                              gsize              size)
{
    VDAgentConnectionPrivate *priv = vdagent_connection_get_instance_private(self);
    GPollableOutputStream *out;
    GSource *source;

    g_queue_push_tail(priv->write_queue, g_bytes_new_take(data, size));

    if (g_queue_get_length(priv->write_queue) == 1) {
        out = G_POLLABLE_OUTPUT_STREAM(g_io_stream_get_output_stream(priv->io_stream));

        source = g_pollable_output_stream_create_source(out, priv->cancellable);
        g_source_set_callback(source, G_SOURCE_FUNC(out_stream_ready_cb),
            g_object_ref(self), NULL);
        g_source_attach(source, NULL);
        g_source_unref(source);
    }
}

void vdagent_connection_flush(VDAgentConnection *self)
{
    while (do_write(self, TRUE));
}

static void message_read_cb(GObject      *source_object,
                            GAsyncResult *res,
                            gpointer      user_data)
{
    VDAgentConnection *self = user_data;
    VDAgentConnectionPrivate *priv = vdagent_connection_get_instance_private(self);
    GInputStream *in = G_INPUT_STREAM(source_object);
    GError *err = NULL;
    gsize bytes_read, data_size;

    g_input_stream_read_all_finish(in, res, &bytes_read, &err);
    if (err) {
        if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_error_free(err);
        } else {
            priv->error_cb(self, err);
        }
        goto unref;
    }

    if (bytes_read == 0) {
        /* see virtio-port.c for the rationale behind this */
        if (priv->opening) {
            g_usleep(10000);
            read_next_message(self);
        } else {
            priv->error_cb(self, NULL);
        }
        goto unref;
    }
    priv->opening = FALSE;

    if (!priv->data_buf) {
        /* we've read the message header, now let's read its body */
        data_size = VDAGENT_CONNECTION_GET_CLASS(self)->handle_header(
            self, priv->header_buf);

        if (g_cancellable_is_cancelled(priv->cancellable)) {
            goto unref;
        }

        if (data_size > 0) {
            priv->data_buf = g_malloc(data_size);
            g_input_stream_read_all_async(in,
                priv->data_buf, data_size,
                G_PRIORITY_DEFAULT, priv->cancellable,
                message_read_cb, g_object_ref(self));
            goto unref;
        }
    }

    VDAGENT_CONNECTION_GET_CLASS(self)->handle_message(
        self, priv->header_buf, priv->data_buf);

    g_clear_pointer(&priv->data_buf, g_free);
    read_next_message(self);

unref:
    g_object_unref(self);
}

static void read_next_message(VDAgentConnection *self)
{
    VDAgentConnectionPrivate *priv = vdagent_connection_get_instance_private(self);
    GInputStream *in;

    if (g_cancellable_is_cancelled(priv->cancellable)) {
        return;
    }

    in = g_io_stream_get_input_stream(priv->io_stream);

    g_input_stream_read_all_async(in,
        priv->header_buf, priv->header_size,
        G_PRIORITY_DEFAULT, priv->cancellable,
        message_read_cb, g_object_ref(self));
}

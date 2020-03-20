/*  udscs.c Unix Domain Socket Client Server framework. A framework for quickly
    creating select() based servers capable of handling multiple clients and
    matching select() based clients using variable size messages.

    Copyright 2010 Red Hat, Inc.

    Red Hat Authors:
    Hans de Goede <hdegoede@redhat.com>

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
#include <config.h>

#include <stdlib.h>
#include <syslog.h>
#include <glib-unix.h>
#include <gio/gunixsocketaddress.h>
#include "udscs.h"
#include "vdagentd-proto-strings.h"
#include "vdagent-connection.h"

struct _UdscsConnection {
    VDAgentConnection parent_instance;
    int debug;
    udscs_read_callback read_callback;
};

G_DEFINE_TYPE(UdscsConnection, udscs_connection, VDAGENT_TYPE_CONNECTION)

static void debug_print_message_header(UdscsConnection             *conn,
                                       struct udscs_message_header *header,
                                       const gchar                 *direction)
{
    const gchar *type = "invalid message";

    if (conn == NULL || conn->debug == FALSE)
        return;

    if (header->type < G_N_ELEMENTS(vdagentd_messages))
        type = vdagentd_messages[header->type];

    syslog(LOG_DEBUG, "%p %s %s, arg1: %u, arg2: %u, size %u",
        conn, direction, type, header->arg1, header->arg2, header->size);
}

static gsize conn_handle_header(VDAgentConnection *conn,
                                gpointer           header_buf)
{
    return ((struct udscs_message_header *)header_buf)->size;
}

static void conn_handle_message(VDAgentConnection *conn,
                                gpointer           header_buf,
                                gpointer           data)
{
    UdscsConnection *self = UDSCS_CONNECTION(conn);
    struct udscs_message_header *header = header_buf;

    debug_print_message_header(self, header, "received");

    self->read_callback(self, header, data);
}

static void udscs_connection_init(UdscsConnection *self)
{
}

static void udscs_connection_finalize(GObject *obj)
{
    UdscsConnection *self = UDSCS_CONNECTION(obj);

    if (self->debug) {
        syslog(LOG_DEBUG, "%p disconnected", self);
    }

    G_OBJECT_CLASS(udscs_connection_parent_class)->finalize(obj);
}

static void udscs_connection_class_init(UdscsConnectionClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    VDAgentConnectionClass *conn_class = VDAGENT_CONNECTION_CLASS(klass);

    gobject_class->finalize = udscs_connection_finalize;

    conn_class->handle_header = conn_handle_header;
    conn_class->handle_message = conn_handle_message;
}

UdscsConnection *udscs_connect(const char *socketname,
    udscs_read_callback read_callback,
    VDAgentConnErrorCb error_cb,
    int debug)
{
    GIOStream *io_stream;
    UdscsConnection *conn;
    GError *err = NULL;

    io_stream = vdagent_socket_connect(socketname, &err);
    if (err) {
        syslog(LOG_ERR, "%s: %s", __func__, err->message);
        g_error_free(err);
        return NULL;
    }

    conn = g_object_new(UDSCS_TYPE_CONNECTION, NULL);
    conn->debug = debug;
    conn->read_callback = read_callback;
    vdagent_connection_setup(VDAGENT_CONNECTION(conn),
                             io_stream,
                             FALSE,
                             sizeof(struct udscs_message_header),
                             error_cb);

    if (conn->debug) {
        syslog(LOG_DEBUG, "%p connected to %s", conn, socketname);
    }

    return conn;
}

void udscs_write(UdscsConnection *conn, uint32_t type, uint32_t arg1,
    uint32_t arg2, const uint8_t *data, uint32_t size)
{
    gpointer buf;
    guint buf_size;
    struct udscs_message_header header;

    buf_size = sizeof(header) + size;
    buf = g_malloc(buf_size);

    header.type = type;
    header.arg1 = arg1;
    header.arg2 = arg2;
    header.size = size;

    memcpy(buf, &header, sizeof(header));
    memcpy(buf + sizeof(header), data, size);

    debug_print_message_header(conn, &header, "sent");

    vdagent_connection_write(VDAGENT_CONNECTION(conn), buf, buf_size);
}

#ifndef UDSCS_NO_SERVER

/* ---------- Server-side implementation ---------- */

struct udscs_server {
    GSocketService *service;
    GList *connections;

    int debug;
    udscs_connect_callback connect_callback;
    udscs_read_callback read_callback;
    VDAgentConnErrorCb error_cb;
};

static gboolean udscs_server_accept_cb(GSocketService    *service,
                                       GSocketConnection *socket_conn,
                                       GObject           *source_object,
                                       gpointer           user_data);

struct udscs_server *udscs_server_new(
    udscs_connect_callback connect_callback,
    udscs_read_callback read_callback,
    VDAgentConnErrorCb error_cb,
    int debug)
{
    struct udscs_server *server;

    server = g_new0(struct udscs_server, 1);
    server->debug = debug;
    server->connect_callback = connect_callback;
    server->read_callback = read_callback;
    server->error_cb = error_cb;
    server->service = g_socket_service_new();
    g_socket_service_stop(server->service);

    g_signal_connect(server->service, "incoming",
        G_CALLBACK(udscs_server_accept_cb), server);

    return server;
}

void udscs_server_listen_to_socket(struct udscs_server *server,
                                   gint                 fd,
                                   GError             **err)
{
    GSocket *socket;

    socket = g_socket_new_from_fd(fd, err);
    if (socket == NULL) {
        return;
    }
    g_socket_listener_add_socket(G_SOCKET_LISTENER(server->service),
                                 socket, NULL, err);
    g_object_unref(socket);
}

void udscs_server_listen_to_address(struct udscs_server *server,
                                    const gchar         *addr,
                                    GError             **err)
{
    GSocketAddress *sock_addr;

    sock_addr = g_unix_socket_address_new(addr);
    g_socket_listener_add_address(G_SOCKET_LISTENER(server->service),
                                  sock_addr,
                                  G_SOCKET_TYPE_STREAM,
                                  G_SOCKET_PROTOCOL_DEFAULT,
                                  NULL, NULL, err);
    g_object_unref(sock_addr);
}

void udscs_server_start(struct udscs_server *server)
{
    g_socket_service_start(server->service);
}

void udscs_server_destroy_connection(struct udscs_server *server,
                                     UdscsConnection     *conn)
{
    server->connections = g_list_remove(server->connections, conn);
    vdagent_connection_destroy(conn);
}

void udscs_destroy_server(struct udscs_server *server)
{
    if (!server)
        return;

    g_list_free_full(server->connections, vdagent_connection_destroy);
    g_object_unref(server->service);
    g_free(server);
}

static gboolean udscs_server_accept_cb(GSocketService    *service,
                                       GSocketConnection *socket_conn,
                                       GObject           *source_object,
                                       gpointer           user_data)
{
    struct udscs_server *server = user_data;
    UdscsConnection *new_conn;

    new_conn = g_object_new(UDSCS_TYPE_CONNECTION, NULL);
    new_conn->debug = server->debug;
    new_conn->read_callback = server->read_callback;
    g_object_ref(socket_conn);
    vdagent_connection_setup(VDAGENT_CONNECTION(new_conn),
                             G_IO_STREAM(socket_conn),
                             FALSE,
                             sizeof(struct udscs_message_header),
                             server->error_cb);

    server->connections = g_list_prepend(server->connections, new_conn);

    if (server->debug)
        syslog(LOG_DEBUG, "new client accepted: %p", new_conn);

    if (server->connect_callback)
        server->connect_callback(new_conn);

    return TRUE;
}

void udscs_server_write_all(struct udscs_server *server,
        uint32_t type, uint32_t arg1, uint32_t arg2,
        const uint8_t *data, uint32_t size)
{
    GList *l;
    for (l = server->connections; l; l = l->next) {
        udscs_write(UDSCS_CONNECTION(l->data), type, arg1, arg2, data, size);
    }
}

int udscs_server_for_all_clients(struct udscs_server *server,
    udscs_for_all_clients_callback func, void *priv)
{
    int r = 0;
    GList *l, *next;

    if (!server)
        return 0;

    l = server->connections;
    while (l) {
        next = l->next;
        r += func(l->data, priv);
        l = next;
    }
    return r;
}

#endif

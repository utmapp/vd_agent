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
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <glib.h>
#include <glib-unix.h>
#include "udscs.h"

struct udscs_buf {
    uint8_t *buf;
    size_t pos;
    size_t size;

    struct udscs_buf *next;
};

struct udscs_connection {
    int fd;
    const char * const *type_to_string;
    int no_types;
    int debug;
    void *user_data;
#ifndef UDSCS_NO_SERVER
    struct ucred peer_cred;
#endif

    /* Read stuff, single buffer, separate header and data buffer */
    int header_read;
    struct udscs_message_header header;
    struct udscs_buf data;

    /* Writes are stored in a linked list of buffers, with both the header
       + data for a single message in 1 buffer. */
    struct udscs_buf *write_buf;

    /* Callbacks */
    udscs_read_callback read_callback;
    udscs_disconnect_callback disconnect_callback;

    struct udscs_connection *next;
    struct udscs_connection *prev;

    GIOChannel                     *io_channel;
    guint                           write_watch_id;
    guint                           read_watch_id;
};

static gboolean udscs_io_channel_cb(GIOChannel *source,
                                    GIOCondition condition,
                                    gpointer data);

struct udscs_connection *udscs_connect(const char *socketname,
    udscs_read_callback read_callback,
    udscs_disconnect_callback disconnect_callback,
    const char * const type_to_string[], int no_types, int debug)
{
    int c;
    struct sockaddr_un address;
    struct udscs_connection *conn;

    conn = calloc(1, sizeof(*conn));
    if (!conn)
        return NULL;

    conn->type_to_string = type_to_string;
    conn->no_types = no_types;
    conn->debug = debug;

    conn->fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (conn->fd == -1) {
        syslog(LOG_ERR, "creating unix domain socket: %m");
        free(conn);
        return NULL;
    }

    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socketname);
    c = connect(conn->fd, (struct sockaddr *)&address, sizeof(address));
    if (c != 0) {
        if (conn->debug) {
            syslog(LOG_DEBUG, "connect %s: %m", socketname);
        }
        free(conn);
        return NULL;
    }

    conn->io_channel = g_io_channel_unix_new(conn->fd);
    if (!conn->io_channel) {
        udscs_destroy_connection(&conn);
        return NULL;
    }
    conn->read_watch_id =
        g_io_add_watch(conn->io_channel,
                       G_IO_IN | G_IO_ERR | G_IO_NVAL,
                       udscs_io_channel_cb,
                       conn);

    conn->read_callback = read_callback;
    conn->disconnect_callback = disconnect_callback;

    if (conn->debug)
        syslog(LOG_DEBUG, "%p connected to %s", conn, socketname);

    return conn;
}

void udscs_destroy_connection(struct udscs_connection **connp)
{
    struct udscs_buf *wbuf, *next_wbuf;
    struct udscs_connection *conn = *connp;

    if (!conn)
        return;

    if (conn->disconnect_callback)
        conn->disconnect_callback(conn);

    wbuf = conn->write_buf;
    while (wbuf) {
        next_wbuf = wbuf->next;
        free(wbuf->buf);
        free(wbuf);
        wbuf = next_wbuf;
    }

    free(conn->data.buf);
    conn->data.buf = NULL;

    if (conn->next)
        conn->next->prev = conn->prev;
    if (conn->prev)
        conn->prev->next = conn->next;

    close(conn->fd);

    if (conn->write_watch_id != 0)
        g_source_remove(conn->write_watch_id);
    if (conn->read_watch_id != 0)
        g_source_remove(conn->read_watch_id);
    g_clear_pointer(&conn->io_channel, g_io_channel_unref);

    if (conn->debug)
        syslog(LOG_DEBUG, "%p disconnected", conn);

    free(conn);
    *connp = NULL;
}

void udscs_set_user_data(struct udscs_connection *conn, void *data)
{
    conn->user_data = data;
}

void *udscs_get_user_data(struct udscs_connection *conn)
{
    if (!conn)
        return NULL;

    return conn->user_data;
}

int udscs_write(struct udscs_connection *conn, uint32_t type, uint32_t arg1,
    uint32_t arg2, const uint8_t *data, uint32_t size)
{
    struct udscs_buf *wbuf, *new_wbuf;
    struct udscs_message_header header;

    new_wbuf = malloc(sizeof(*new_wbuf));
    if (!new_wbuf)
        return -1;

    new_wbuf->pos = 0;
    new_wbuf->size = sizeof(header) + size;
    new_wbuf->next = NULL;
    new_wbuf->buf = malloc(new_wbuf->size);
    if (!new_wbuf->buf) {
        free(new_wbuf);
        return -1;
    }

    header.type = type;
    header.arg1 = arg1;
    header.arg2 = arg2;
    header.size = size;

    memcpy(new_wbuf->buf, &header, sizeof(header));
    memcpy(new_wbuf->buf + sizeof(header), data, size);

    if (conn->debug) {
        if (type < conn->no_types)
            syslog(LOG_DEBUG, "%p sent %s, arg1: %u, arg2: %u, size %u",
                   conn, conn->type_to_string[type], arg1, arg2, size);
        else
            syslog(LOG_DEBUG,
                   "%p sent invalid message %u, arg1: %u, arg2: %u, size %u",
                   conn, type, arg1, arg2, size);
    }

    if (conn->io_channel && conn->write_watch_id == 0)
        conn->write_watch_id =
            g_io_add_watch(conn->io_channel,
                           G_IO_OUT | G_IO_ERR | G_IO_NVAL,
                           udscs_io_channel_cb,
                           conn);

    if (!conn->write_buf) {
        conn->write_buf = new_wbuf;
        return 0;
    }

    /* maybe we should limit the write_buf stack depth ? */
    wbuf = conn->write_buf;
    while (wbuf->next)
        wbuf = wbuf->next;

    wbuf->next = new_wbuf;

    return 0;
}

/* A helper for udscs_do_read() */
static void udscs_read_complete(struct udscs_connection **connp)
{
    struct udscs_connection *conn = *connp;

    if (conn->debug) {
        if (conn->header.type < conn->no_types)
            syslog(LOG_DEBUG,
                   "%p received %s, arg1: %u, arg2: %u, size %u",
                   conn, conn->type_to_string[conn->header.type],
                   conn->header.arg1, conn->header.arg2, conn->header.size);
        else
            syslog(LOG_DEBUG,
               "%p received invalid message %u, arg1: %u, arg2: %u, size %u",
               conn, conn->header.type, conn->header.arg1, conn->header.arg2,
               conn->header.size);
    }

    if (conn->read_callback) {
        conn->read_callback(connp, &conn->header, conn->data.buf);
        if (!*connp) /* Was the connection disconnected by the callback ? */
            return;
    }

    free(conn->data.buf);
    memset(&conn->data, 0, sizeof(conn->data)); /* data.buf = NULL */
    conn->header_read = 0;
}

static void udscs_do_read(struct udscs_connection **connp)
{
    ssize_t n;
    size_t to_read;
    uint8_t *dest;
    struct udscs_connection *conn = *connp;

    if (conn->header_read < sizeof(conn->header)) {
        to_read = sizeof(conn->header) - conn->header_read;
        dest = (uint8_t *)&conn->header + conn->header_read;
    } else {
        to_read = conn->data.size - conn->data.pos;
        dest = conn->data.buf + conn->data.pos;
    }

    n = read(conn->fd, dest, to_read);
    if (n < 0) {
        if (errno == EINTR)
            return;
        syslog(LOG_ERR, "reading unix domain socket: %m, disconnecting %p",
               conn);
    }
    if (n <= 0) {
        udscs_destroy_connection(connp);
        return;
    }

    if (conn->header_read < sizeof(conn->header)) {
        conn->header_read += n;
        if (conn->header_read == sizeof(conn->header)) {
            if (conn->header.size == 0) {
                udscs_read_complete(connp);
                return;
            }
            conn->data.pos = 0;
            conn->data.size = conn->header.size;
            conn->data.buf = malloc(conn->data.size);
            if (!conn->data.buf) {
                syslog(LOG_ERR, "out of memory, disconnecting %p", conn);
                udscs_destroy_connection(connp);
                return;
            }
        }
    } else {
        conn->data.pos += n;
        if (conn->data.pos == conn->data.size)
            udscs_read_complete(connp);
    }
}

static void udscs_do_write(struct udscs_connection **connp)
{
    ssize_t n;
    size_t to_write;
    struct udscs_connection *conn = *connp;

    struct udscs_buf* wbuf = conn->write_buf;
    if (!wbuf) {
        syslog(LOG_ERR,
               "%p do_write called on a connection without a write buf ?!",
               conn);
        return;
    }

    to_write = wbuf->size - wbuf->pos;
    n = write(conn->fd, wbuf->buf + wbuf->pos, to_write);
    if (n < 0) {
        if (errno == EINTR)
            return;
        syslog(LOG_ERR, "writing to unix domain socket: %m, disconnecting %p",
               conn);
        udscs_destroy_connection(connp);
        return;
    }

    wbuf->pos += n;
    if (wbuf->pos == wbuf->size) {
        conn->write_buf = wbuf->next;
        free(wbuf->buf);
        free(wbuf);
    }
}

static gboolean udscs_io_channel_cb(GIOChannel *source,
                                    GIOCondition condition,
                                    gpointer data)
{
    struct udscs_connection *conn = data;

    if (condition & G_IO_IN) {
        udscs_do_read(&conn);
        if (conn == NULL)
            return G_SOURCE_REMOVE;
        return G_SOURCE_CONTINUE;
    }
    if (condition & G_IO_OUT) {
        udscs_do_write(&conn);
        if (conn == NULL)
            return G_SOURCE_REMOVE;
        if (conn->write_buf)
            return G_SOURCE_CONTINUE;
        conn->write_watch_id = 0;
        return G_SOURCE_REMOVE;
    }

    udscs_destroy_connection(&conn);
    return G_SOURCE_REMOVE;
}


#ifndef UDSCS_NO_SERVER

/* ---------- Server-side implementation ---------- */

struct udscs_server {
    int fd;
    const char * const *type_to_string;
    int no_types;
    int debug;
    struct udscs_connection connections_head;
    udscs_connect_callback connect_callback;
    udscs_read_callback read_callback;
    udscs_disconnect_callback disconnect_callback;
};

struct udscs_server *udscs_create_server_for_fd(int fd,
    udscs_connect_callback connect_callback,
    udscs_read_callback read_callback,
    udscs_disconnect_callback disconnect_callback,
    const char * const type_to_string[], int no_types, int debug)
{
    struct udscs_server *server;

    if (fd <= 0) {
        syslog(LOG_ERR, "Invalid file descriptor: %i", fd);
        return NULL;
    }

    server = calloc(1, sizeof(*server));
    if (!server)
        return NULL;

    server->type_to_string = type_to_string;
    server->no_types = no_types;
    server->debug = debug;
    server->fd = fd;
    server->connect_callback = connect_callback;
    server->read_callback = read_callback;
    server->disconnect_callback = disconnect_callback;

    return server;
}

struct udscs_server *udscs_create_server(const char *socketname,
    udscs_connect_callback connect_callback,
    udscs_read_callback read_callback,
    udscs_disconnect_callback disconnect_callback,
    const char * const type_to_string[], int no_types, int debug)
{
    int c;
    int fd;
    struct sockaddr_un address;

    fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        syslog(LOG_ERR, "creating unix domain socket: %m");
        return NULL;
    }

    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socketname);
    c = bind(fd, (struct sockaddr *)&address, sizeof(address));
    if (c != 0) {
        syslog(LOG_ERR, "bind %s: %m", socketname);
        return NULL;
    }

    c = listen(fd, 5);
    if (c != 0) {
        syslog(LOG_ERR, "listen: %m");
        return NULL;
    }

    return udscs_create_server_for_fd(fd, connect_callback, read_callback,
                                      disconnect_callback, type_to_string,
                                      no_types, debug);
}

void udscs_destroy_server(struct udscs_server *server)
{
    struct udscs_connection *conn, *next_conn;

    if (!server)
        return;

    conn = server->connections_head.next;
    while (conn) {
        next_conn = conn->next;
        udscs_destroy_connection(&conn);
        conn = next_conn;
    }
    close(server->fd);
    free(server);
}

struct ucred udscs_get_peer_cred(struct udscs_connection *conn)
{
    return conn->peer_cred;
}

static void udscs_server_accept(struct udscs_server *server) {
    struct udscs_connection *new_conn, *conn;
    struct sockaddr_un address;
    socklen_t length = sizeof(address);
    int r, fd;

    fd = accept(server->fd, (struct sockaddr *)&address, &length);
    if (fd == -1) {
        if (errno == EINTR)
            return;
        syslog(LOG_ERR, "accept: %m");
        return;
    }

    new_conn = calloc(1, sizeof(*conn));
    if (!new_conn) {
        syslog(LOG_ERR, "out of memory, disconnecting new client");
        close(fd);
        return;
    }

    new_conn->fd = fd;
    new_conn->type_to_string = server->type_to_string;
    new_conn->no_types = server->no_types;
    new_conn->debug = server->debug;
    new_conn->read_callback = server->read_callback;
    new_conn->disconnect_callback = server->disconnect_callback;

    length = sizeof(new_conn->peer_cred);
    r = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &new_conn->peer_cred, &length);
    if (r != 0) {
        syslog(LOG_ERR, "Could not get peercred, disconnecting new client");
        close(fd);
        free(new_conn);
        return;
    }

    conn = &server->connections_head;
    while (conn->next)
        conn = conn->next;

    new_conn->prev = conn;
    conn->next = new_conn;

    if (server->debug)
        syslog(LOG_DEBUG, "new client accepted: %p, pid: %d",
               new_conn, (int)new_conn->peer_cred.pid);

    if (server->connect_callback)
        server->connect_callback(new_conn);
}

int udscs_server_fill_fds(struct udscs_server *server, fd_set *readfds,
        fd_set *writefds)
{
    struct udscs_connection *conn;
    int nfds;

    if (!server)
        return -1;

    nfds = server->fd + 1;
    FD_SET(server->fd, readfds);

    conn = server->connections_head.next;
    while (conn) {
        FD_SET(conn->fd, readfds);
        if (conn->write_buf)
            FD_SET(conn->fd, writefds);

        if (conn->fd >= nfds)
            nfds = conn->fd + 1;

        conn = conn->next;
    }

    return nfds;
}

void udscs_server_handle_fds(struct udscs_server *server, fd_set *readfds,
        fd_set *writefds)
{
    struct udscs_connection *conn, *next_conn;

    if (!server)
        return;

    if (FD_ISSET(server->fd, readfds))
        udscs_server_accept(server);

    conn = server->connections_head.next;
    while (conn) {
        /* conn may be destroyed by udscs_do_read() or udscs_do_write()
         * (when disconnected), so get the next connection first. */
        next_conn = conn->next;

        if (FD_ISSET(conn->fd, readfds))
            udscs_do_read(&conn);
        if (conn && FD_ISSET(conn->fd, writefds))
            udscs_do_write(&conn);

        conn = next_conn;
    }
}

int udscs_server_write_all(struct udscs_server *server,
        uint32_t type, uint32_t arg1, uint32_t arg2,
        const uint8_t *data, uint32_t size)
{
    struct udscs_connection *conn;

    conn = server->connections_head.next;
    while (conn) {
        if (udscs_write(conn, type, arg1, arg2, data, size))
            return -1;
        conn = conn->next;
    }

    return 0;
}

int udscs_server_for_all_clients(struct udscs_server *server,
    udscs_for_all_clients_callback func, void *priv)
{
    int r = 0;
    struct udscs_connection *conn, *next_conn;

    if (!server)
        return 0;

    conn = server->connections_head.next;
    while (conn) {
        /* Get next conn as func may destroy the current conn */
        next_conn = conn->next;
        r += func(&conn, priv);
        conn = next_conn;
    }
    return r;
}

#endif

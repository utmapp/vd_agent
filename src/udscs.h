/*  udscs.h Unix Domain Socket Client Server framework header file

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

#ifndef __UDSCS_H
#define __UDSCS_H

#include <stdint.h>
#include <sys/socket.h>
#include <glib-object.h>
#include "vdagent-connection.h"

G_BEGIN_DECLS

#define UDSCS_TYPE_CONNECTION udscs_connection_get_type()
G_DECLARE_FINAL_TYPE(UdscsConnection, udscs_connection, UDSCS, CONNECTION, VDAgentConnection)

/* ---------- Generic bits and client-side API ---------- */

struct udscs_message_header {
    uint32_t type;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t size;
};

/* Callbacks with this type will be called when a complete message has been
 * received. The callback does not own the data buffer and should not free it.
 * The data buffer will be freed shortly after the read callback returns.
 */
typedef void (*udscs_read_callback)(UdscsConnection *conn,
    struct udscs_message_header *header, uint8_t *data);

/* Connect to the unix domain socket specified by socketname.
 * Only sockets bound to a pathname are supported.
 *
 * If debug is true then the events on this connection will be traced.
 * This includes the incoming and outgoing message names.
 */
UdscsConnection *udscs_connect(const char *socketname,
    udscs_read_callback read_callback,
    VDAgentConnErrorCb error_cb,
    int debug);

/* Queue a message for delivery to the client connected through conn.
 */
void udscs_write(UdscsConnection *conn, uint32_t type, uint32_t arg1,
        uint32_t arg2, const uint8_t *data, uint32_t size);

#ifndef UDSCS_NO_SERVER

/* ---------- Server-side API ---------- */

struct udscs_server;

/* Callbacks with this type will be called when a new connection to a
 * server is accepted.
 */
typedef void (*udscs_connect_callback)(UdscsConnection *conn);

/* Initialize a new udscs_server struct.
 *
 * If debug is true then the events on this socket and related individual
 * connections will be traced.
 * This includes the incoming and outgoing message names.
 */
struct udscs_server *udscs_server_new(
    udscs_connect_callback connect_callback,
    udscs_read_callback read_callback,
    VDAgentConnErrorCb error_cb,
    int debug);

/* Start listening on a pre-configured socket specified by the given @fd.
 * This can be used with systemd socket activation, etc. */
void udscs_server_listen_to_socket(struct udscs_server *server,
                                   gint                 fd,
                                   GError             **err);

/* Create a new socket, bind it to @address and start listening on it. */
void udscs_server_listen_to_address(struct udscs_server *server,
                                    const gchar         *addr,
                                    GError             **err);

void udscs_server_destroy_connection(struct udscs_server *server,
                                     UdscsConnection     *conn);

/* Close all the server's connections and releases the corresponding
 * resources.
 * Does nothing if server is NULL.
 */
void udscs_destroy_server(struct udscs_server *server);

/* Like udscs_write, but then send the message to all clients connected to
 * the server.
 */
void udscs_server_write_all(struct udscs_server *server,
    uint32_t type, uint32_t arg1, uint32_t arg2,
    const uint8_t *data, uint32_t size);

/* Callback type for udscs_server_for_all_clients. Clients can be disconnected
 * from this callback just like with a read callback.
 */
typedef int (*udscs_for_all_clients_callback)(UdscsConnection *conn,
    void *priv);

/* Call func for all clients connected to the server, passing through
 * priv to all func calls. Returns the total of the return values from all
 * calls to func or 0 if server is NULL.
 */
int udscs_server_for_all_clients(struct udscs_server *server,
    udscs_for_all_clients_callback func, void *priv);

#endif

G_END_DECLS

#endif

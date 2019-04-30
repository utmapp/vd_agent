/*  vdagent-connection.h

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

#ifndef __VDAGENT_CONNECTION_H
#define __VDAGENT_CONNECTION_H

#include <glib.h>
#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define VDAGENT_TYPE_CONNECTION vdagent_connection_get_type()
G_DECLARE_DERIVABLE_TYPE(VDAgentConnection, vdagent_connection, VDAGENT, CONNECTION, GObject)

/* Sublasses of VDAgentConnection must implement
 * handle_header and handle_message. */
struct _VDAgentConnectionClass {
    GObjectClass parent_class;

    /* Called when a message header has been read.
    *
    * Handler must parse the @header_buf and
    * return the size of message's body.
    *
    * @header_buf must not be freed. */
    gsize (*handle_header) (VDAgentConnection *self,
                            gpointer           header_buf);

    /* Called when a full message has been read.
    *
    * @header, @data must not be freed. */
    void (*handle_message) (VDAgentConnection *self,
                            gpointer           header_buf,
                            gpointer           data_buf);
};

/* Invoked when an error occurs during read or write.
 *
 * If @err is NULL, the connection was closed by the remote side,
 * otherwise the handler must free @err using g_error_free().
 *
 * VDAgentConnection will not continue with the given I/O-op that failed. */
typedef void (*VDAgentConnErrorCb)(VDAgentConnection *self, GError *err);

/* Open a file in @path for read and write.
 * Returns a new GIOStream to the given file or NULL when @err is set. */
GIOStream *vdagent_file_open(const gchar *path, GError **err);

/* Create a socket and initiate a new connection to the socket in @path.
 * Returns a new GIOStream or NULL when @err is set. */
GIOStream *vdagent_socket_connect(const gchar *path, GError **err);

/* Set up @self to use @io_stream and start reading from it.
 *
 * If @wait_on_opening is set to TRUE, EOF won't be treated as an error
 * until the first message is successfully read or written to the @io_stream. */
void vdagent_connection_setup(VDAgentConnection *self,
                              GIOStream         *io_stream,
                              gboolean           wait_on_opening,
                              gsize              header_size,
                              VDAgentConnErrorCb error_cb);


/* Cancel running I/O-operations, close the underlying FD and
 * unref the VDAgentConnection object. */
void vdagent_connection_destroy(gpointer p);

/* Append a message to the write queue.
 *
 * VDAgentConnection takes ownership of @data
 * and frees it once the message is flushed. */
void vdagent_connection_write(VDAgentConnection *self,
                              gpointer           data,
                              gsize              size);

/* Synchronously write all queued messages to the output stream. */
void vdagent_connection_flush(VDAgentConnection *self);

/* Returns the PID of the foreign process connected to the socket
 * or -1 with @err set. */
gint vdagent_connection_get_peer_pid(VDAgentConnection *self,
                                     GError           **err);

G_END_DECLS

#endif

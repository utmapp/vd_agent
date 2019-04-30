/*  vdagent-virtio-port.c virtio port communication code

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
#include <string.h>
#include <syslog.h>
#include <gio/gio.h>
#include <glib-unix.h>

#include "vdagent-connection.h"
#include "virtio-port.h"


struct vdagent_virtio_port_buf {
    uint8_t *buf;
    size_t size;
    size_t write_pos;
};

/* Data to keep track of the assembling of vdagent messages per chunk port,
   for de-multiplexing the messages */
struct vdagent_virtio_port_chunk_port_data {
    int message_header_read;
    int message_data_pos;
    VDAgentMessage message_header;
    uint8_t *message_data;
};

struct _VirtioPort {
    VDAgentConnection parent_instance;

    /* Per chunk port data */
    struct vdagent_virtio_port_chunk_port_data port_data[VDP_END_PORT];

    struct vdagent_virtio_port_buf write_buf;

    /* Callbacks */
    vdagent_virtio_port_read_callback read_callback;
    VDAgentConnErrorCb error_cb;
};

G_DEFINE_TYPE(VirtioPort, virtio_port, VDAGENT_TYPE_CONNECTION)

static void vdagent_virtio_port_do_chunk(VDAgentConnection *conn,
                                         gpointer header_data,
                                         gpointer chunk_data);

static gsize conn_handle_header(VDAgentConnection *conn,
                                gpointer header_buf)
{
    VirtioPort *self = VIRTIO_PORT(conn);
    VDIChunkHeader *header = header_buf;
    GError *err;

    header->size = GUINT32_FROM_LE(header->size);
    header->port = GUINT32_FROM_LE(header->port);

    if (header->size > VD_AGENT_MAX_DATA_SIZE) {
        err = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                          "chunk size %u too large", header->size);
        self->error_cb(conn, err);
        return 0;
    }
    if (header->port >= VDP_END_PORT) {
        err = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                          "chunk port %u out of range", header->port);
        self->error_cb(conn, err);
        return 0;
    }

    return header->size;
}

static void virtio_port_init(VirtioPort *self)
{
}

static void virtio_port_finalize(GObject *obj)
{
    VirtioPort *self = VIRTIO_PORT(obj);
    guint i;

    g_free(self->write_buf.buf);

    for (i = 0; i < VDP_END_PORT; i++) {
        g_free(self->port_data[i].message_data);
    }

    G_OBJECT_CLASS(virtio_port_parent_class)->finalize(obj);
}

static void virtio_port_class_init(VirtioPortClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    VDAgentConnectionClass *conn_class = VDAGENT_CONNECTION_CLASS(klass);

    gobject_class->finalize  = virtio_port_finalize;

    conn_class->handle_header = conn_handle_header;
    conn_class->handle_message = vdagent_virtio_port_do_chunk;
}

VirtioPort *vdagent_virtio_port_create(const char *portname,
    vdagent_virtio_port_read_callback read_callback,
    VDAgentConnErrorCb error_cb)
{
    VirtioPort *vport;
    GIOStream *io_stream;
    GError *err = NULL;

    io_stream = vdagent_file_open(portname, &err);
    if (err) {
        syslog(LOG_ERR, "%s: %s", __func__, err->message);
        g_clear_error(&err);
        io_stream = vdagent_socket_connect(portname, &err);
        if (err) {
            syslog(LOG_ERR, "%s: %s", __func__, err->message);
            g_error_free(err);
            return NULL;
        }
    }

    vport = g_object_new(VIRTIO_TYPE_PORT, NULL);

    /* When calling vdagent_connection_new(),
     * @wait_on_opening MUST be set to TRUE:
     *
     * When we open the virtio serial port, the following happens:
     * 1) The linux kernel virtio_console driver sends a
     *    VIRTIO_CONSOLE_PORT_OPEN message to qemu
     * 2) qemu's spicevmc chardev driver calls qemu_spice_add_interface to
     *    register the agent chardev with the spice-server
     * 3) spice-server then calls the spicevmc chardev driver's state
     *    callback to let it know it is ready to receive data
     * 4) The state callback sends a CHR_EVENT_OPENED to the virtio-console
     *    chardev backend
     * 5) The virtio-console chardev backend sends VIRTIO_CONSOLE_PORT_OPEN
     *    to the linux kernel virtio_console driver
     *
     * Until steps 1 - 5 have completed the linux kernel virtio_console
     * driver sees the virtio serial port as being in a disconnected state
     * and read will return 0 ! So if we blindly assume that a read 0 means
     * that the channel is closed we will hit a race here.
     */
    vdagent_connection_setup(VDAGENT_CONNECTION(vport),
                             io_stream,
                             TRUE,
                             sizeof(VDIChunkHeader),
                             error_cb);

    vport->read_callback = read_callback;
    vport->error_cb = error_cb;

    return vport;
}

void vdagent_virtio_port_write_start(
        VirtioPort *vport,
        uint32_t port_nr,
        uint32_t message_type,
        uint32_t message_opaque,
        uint32_t data_size)
{
    struct vdagent_virtio_port_buf *new_wbuf;
    VDIChunkHeader *chunk_header;
    VDAgentMessage *message_header;

    g_return_if_fail(vport->write_buf.buf == NULL);

    new_wbuf = &vport->write_buf;
    new_wbuf->write_pos = 0;
    new_wbuf->size = sizeof(*chunk_header) + sizeof(*message_header) + data_size;
    new_wbuf->buf = g_malloc(new_wbuf->size);

    chunk_header = (VDIChunkHeader *) (new_wbuf->buf + new_wbuf->write_pos);
    chunk_header->port = GUINT32_TO_LE(port_nr);
    chunk_header->size = GUINT32_TO_LE(sizeof(*message_header) + data_size);
    new_wbuf->write_pos += sizeof(*chunk_header);

    message_header = (VDAgentMessage *) (new_wbuf->buf + new_wbuf->write_pos);
    message_header->protocol = GUINT32_TO_LE(VD_AGENT_PROTOCOL);
    message_header->type = GUINT32_TO_LE(message_type);
    message_header->opaque = GUINT64_TO_LE(message_opaque);
    message_header->size = GUINT32_TO_LE(data_size);
    new_wbuf->write_pos += sizeof(*message_header);
}

int vdagent_virtio_port_write_append(VirtioPort *vport,
                                     const uint8_t *data, uint32_t size)
{
    struct vdagent_virtio_port_buf *wbuf;

    if (size == 0) {
        return 0;
    }

    wbuf = &vport->write_buf;
    if (!wbuf->buf) {
        syslog(LOG_ERR, "can't append without a buffer");
        return -1;
    }

    if (wbuf->size - wbuf->write_pos < size) {
        syslog(LOG_ERR, "can't append to full buffer");
        return -1;
    }

    memcpy(wbuf->buf + wbuf->write_pos, data, size);
    wbuf->write_pos += size;

    if (wbuf->write_pos == wbuf->size) {
        vdagent_connection_write(VDAGENT_CONNECTION(vport), wbuf->buf, wbuf->size);
        wbuf->buf = NULL;
    }
    return 0;
}

void vdagent_virtio_port_write(
        VirtioPort *vport,
        uint32_t port_nr,
        uint32_t message_type,
        uint32_t message_opaque,
        const uint8_t *data,
        uint32_t data_size)
{
    vdagent_virtio_port_write_start(vport, port_nr, message_type,
                                    message_opaque, data_size);
    vdagent_virtio_port_write_append(vport, data, data_size);
}

void vdagent_virtio_port_reset(VirtioPort *vport, int port)
{
    if (port >= VDP_END_PORT) {
        syslog(LOG_ERR, "vdagent_virtio_port_reset port out of range");
        return;
    }
    g_free(vport->port_data[port].message_data);
    memset(&vport->port_data[port], 0, sizeof(vport->port_data[0]));
}

static void vdagent_virtio_port_do_chunk(VDAgentConnection *conn,
                                         gpointer header_data,
                                         gpointer chunk_data)
{
    int avail, read, pos = 0;
    VirtioPort *vport = VIRTIO_PORT(conn);
    VDIChunkHeader *chunk_header = header_data;
    struct vdagent_virtio_port_chunk_port_data *port =
        &vport->port_data[chunk_header->port];

    if (port->message_header_read < sizeof(port->message_header)) {
        read = sizeof(port->message_header) - port->message_header_read;
        if (read > chunk_header->size) {
            read = chunk_header->size;
        }
        memcpy((uint8_t *)&port->message_header + port->message_header_read,
               chunk_data, read);
        port->message_header_read += read;
        if (port->message_header_read == sizeof(port->message_header)) {

            port->message_header.protocol = GUINT32_FROM_LE(port->message_header.protocol);
            port->message_header.type = GUINT32_FROM_LE(port->message_header.type);
            port->message_header.opaque = GUINT64_FROM_LE(port->message_header.opaque);
            port->message_header.size = GUINT32_FROM_LE(port->message_header.size);

            if (port->message_header.size) {
                port->message_data = g_malloc(port->message_header.size);
            }
        }
        pos = read;
    }

    if (port->message_header_read == sizeof(port->message_header)) {
        read  = port->message_header.size - port->message_data_pos;
        avail = chunk_header->size - pos;

        if (avail > read) {
            GError *err = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                      "chunk larger than message, lost sync?");
            vport->error_cb(VDAGENT_CONNECTION(vport), err);
            return;
        }

        if (avail < read)
            read = avail;

        if (read) {
            memcpy(port->message_data + port->message_data_pos,
                   chunk_data + pos, read);
            port->message_data_pos += read;
        }

        if (port->message_data_pos == port->message_header.size) {
            if (vport->read_callback) {
                vport->read_callback(vport, chunk_header->port,
                                     &port->message_header, port->message_data);
            }
            port->message_header_read = 0;
            port->message_data_pos = 0;
            g_clear_pointer(&port->message_data, g_free);
        }
    }
}

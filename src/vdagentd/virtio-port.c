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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <glib.h>

#include "virtio-port.h"


struct vdagent_virtio_port_buf {
    uint8_t *buf;
    size_t pos;
    size_t size;
    size_t write_pos;

    struct vdagent_virtio_port_buf *next;
};

/* Data to keep track of the assembling of vdagent messages per chunk port,
   for de-multiplexing the messages */
struct vdagent_virtio_port_chunk_port_data {
    int message_header_read;
    int message_data_pos;
    VDAgentMessage message_header;
    uint8_t *message_data;
};

struct vdagent_virtio_port {
    int fd;
    int opening;
    int is_uds;

    /* Chunk read stuff, single buffer, separate header and data buffer */
    int chunk_header_read;
    int chunk_data_pos;
    VDIChunkHeader chunk_header;
    uint8_t chunk_data[VD_AGENT_MAX_DATA_SIZE];

    /* Per chunk port data */
    struct vdagent_virtio_port_chunk_port_data port_data[VDP_END_PORT];

    /* Writes are stored in a linked list of buffers, with both the header
       + data for a single message in 1 buffer. */
    struct vdagent_virtio_port_buf *write_buf;

    /* Callbacks */
    vdagent_virtio_port_read_callback read_callback;
    vdagent_virtio_port_disconnect_callback disconnect_callback;
};

static void vdagent_virtio_port_do_write(struct vdagent_virtio_port **vportp);
static void vdagent_virtio_port_do_read(struct vdagent_virtio_port **vportp);

struct vdagent_virtio_port *vdagent_virtio_port_create(const char *portname,
    vdagent_virtio_port_read_callback read_callback,
    vdagent_virtio_port_disconnect_callback disconnect_callback)
{
    struct vdagent_virtio_port *vport;
    struct sockaddr_un address;
    int c;

    vport = calloc(1, sizeof(*vport));
    if (!vport)
        return 0;

    vport->fd = open(portname, O_RDWR);
    if (vport->fd == -1) {
        vport->fd = socket(PF_UNIX, SOCK_STREAM, 0);
        if (vport->fd == -1) {
            goto error;
        }
        address.sun_family = AF_UNIX;
        snprintf(address.sun_path, sizeof(address.sun_path), "%s", portname);
        c = connect(vport->fd, (struct sockaddr *)&address, sizeof(address));
        if (c == 0) {
            vport->is_uds = 1;
        } else {
            goto error;
        }
    } else {
        vport->is_uds = 0;
    }
    vport->opening = 1;

    vport->read_callback = read_callback;
    vport->disconnect_callback = disconnect_callback;

    return vport;

error:
    syslog(LOG_ERR, "open %s: %m", portname);
    if (vport->fd != -1) {
        close(vport->fd);
    }
    free(vport);
    return NULL;
}

void vdagent_virtio_port_destroy(struct vdagent_virtio_port **vportp)
{
    struct vdagent_virtio_port_buf *wbuf, *next_wbuf;
    struct vdagent_virtio_port *vport = *vportp;
    int i;

    if (!vport)
        return;

    if (vport->disconnect_callback)
        vport->disconnect_callback(vport);

    wbuf = vport->write_buf;
    while (wbuf) {
        next_wbuf = wbuf->next;
        free(wbuf->buf);
        free(wbuf);
        wbuf = next_wbuf;
    }

    for (i = 0; i < VDP_END_PORT; i++) {
        free(vport->port_data[i].message_data);
    }

    close(vport->fd);
    free(vport);
    *vportp = NULL;
}

int vdagent_virtio_port_fill_fds(struct vdagent_virtio_port *vport,
        fd_set *readfds, fd_set *writefds)
{
    if (!vport)
        return -1;

    FD_SET(vport->fd, readfds);
    if (vport->write_buf)
        FD_SET(vport->fd, writefds);

    return vport->fd + 1;
}

void vdagent_virtio_port_handle_fds(struct vdagent_virtio_port **vportp,
        fd_set *readfds, fd_set *writefds)
{
    if (!*vportp)
        return;

    if (FD_ISSET((*vportp)->fd, readfds))
        vdagent_virtio_port_do_read(vportp);

    if (*vportp && FD_ISSET((*vportp)->fd, writefds))
        vdagent_virtio_port_do_write(vportp);
}

static struct vdagent_virtio_port_buf* vdagent_virtio_port_get_last_wbuf(
    struct vdagent_virtio_port *vport)
{
    struct vdagent_virtio_port_buf *wbuf;

    wbuf = vport->write_buf;
    if (!wbuf)
        return NULL;

    while (wbuf->next)
        wbuf = wbuf->next;

    return wbuf;
}

int vdagent_virtio_port_write_start(
        struct vdagent_virtio_port *vport,
        uint32_t port_nr,
        uint32_t message_type,
        uint32_t message_opaque,
        uint32_t data_size)
{
    struct vdagent_virtio_port_buf *wbuf, *new_wbuf;
    VDIChunkHeader chunk_header;
    VDAgentMessage message_header;

    new_wbuf = malloc(sizeof(*new_wbuf));
    if (!new_wbuf)
        return -1;

    new_wbuf->pos = 0;
    new_wbuf->write_pos = 0;
    new_wbuf->size = sizeof(chunk_header) + sizeof(message_header) + data_size;
    new_wbuf->next = NULL;
    new_wbuf->buf = malloc(new_wbuf->size);
    if (!new_wbuf->buf) {
        free(new_wbuf);
        return -1;
    }

    chunk_header.port = GUINT32_TO_LE(port_nr);
    chunk_header.size = GUINT32_TO_LE(sizeof(message_header) + data_size);
    memcpy(new_wbuf->buf + new_wbuf->write_pos, &chunk_header,
           sizeof(chunk_header));
    new_wbuf->write_pos += sizeof(chunk_header);

    message_header.protocol = GUINT32_TO_LE(VD_AGENT_PROTOCOL);
    message_header.type = GUINT32_TO_LE(message_type);
    message_header.opaque = GUINT64_TO_LE(message_opaque);
    message_header.size = GUINT32_TO_LE(data_size);
    memcpy(new_wbuf->buf + new_wbuf->write_pos, &message_header,
           sizeof(message_header));
    new_wbuf->write_pos += sizeof(message_header);

    if (!vport->write_buf) {
        vport->write_buf = new_wbuf;
        return 0;
    }

    wbuf = vdagent_virtio_port_get_last_wbuf(vport);
    wbuf->next = new_wbuf;

    return 0;
}

int vdagent_virtio_port_write_append(struct vdagent_virtio_port *vport,
                                     const uint8_t *data, uint32_t size)
{
    struct vdagent_virtio_port_buf *wbuf;

    wbuf = vdagent_virtio_port_get_last_wbuf(vport);
    if (!wbuf) {
        syslog(LOG_ERR, "can't append without a buffer");
        return -1;
    }

    if (wbuf->size - wbuf->write_pos < size) {
        syslog(LOG_ERR, "can't append to full buffer");
        return -1;
    }

    memcpy(wbuf->buf + wbuf->write_pos, data, size);
    wbuf->write_pos += size;
    return 0;
}

int vdagent_virtio_port_write(
        struct vdagent_virtio_port *vport,
        uint32_t port_nr,
        uint32_t message_type,
        uint32_t message_opaque,
        const uint8_t *data,
        uint32_t data_size)
{
    if (vdagent_virtio_port_write_start(vport, port_nr, message_type,
                                        message_opaque, data_size)) {
        return -1;
    }
    vdagent_virtio_port_write_append(vport, data, data_size);
    return 0;
}

void vdagent_virtio_port_flush(struct vdagent_virtio_port **vportp)
{
    while (*vportp && (*vportp)->write_buf)
        vdagent_virtio_port_do_write(vportp);
}

void vdagent_virtio_port_reset(struct vdagent_virtio_port *vport, int port)
{
    if (port >= VDP_END_PORT) {
        syslog(LOG_ERR, "vdagent_virtio_port_reset port out of range");
        return;
    }
    free(vport->port_data[port].message_data);
    memset(&vport->port_data[port], 0, sizeof(vport->port_data[0]));
}

static void vdagent_virtio_port_do_chunk(struct vdagent_virtio_port **vportp)
{
    int avail, read, pos = 0;
    struct vdagent_virtio_port *vport = *vportp;
    struct vdagent_virtio_port_chunk_port_data *port =
        &vport->port_data[vport->chunk_header.port];

    if (port->message_header_read < sizeof(port->message_header)) {
        read = sizeof(port->message_header) - port->message_header_read;
        if (read > vport->chunk_header.size) {
            read = vport->chunk_header.size;
        }
        memcpy((uint8_t *)&port->message_header + port->message_header_read,
               vport->chunk_data, read);
        port->message_header_read += read;
        if (port->message_header_read == sizeof(port->message_header)) {

            port->message_header.protocol = GUINT32_FROM_LE(port->message_header.protocol);
            port->message_header.type = GUINT32_FROM_LE(port->message_header.type);
            port->message_header.opaque = GUINT64_FROM_LE(port->message_header.opaque);
            port->message_header.size = GUINT32_FROM_LE(port->message_header.size);

            if (port->message_header.size) {
                port->message_data = malloc(port->message_header.size);
                if (!port->message_data) {
                    syslog(LOG_ERR, "out of memory, disconnecting virtio");
                    vdagent_virtio_port_destroy(vportp);
                    return;
                }
            }
        }
        pos = read;
    }

    if (port->message_header_read == sizeof(port->message_header)) {
        read  = port->message_header.size - port->message_data_pos;
        avail = vport->chunk_header.size - pos;

        if (avail > read) {
            syslog(LOG_ERR, "chunk larger than message, lost sync?");
            vdagent_virtio_port_destroy(vportp);
            return;
        }

        if (avail < read)
            read = avail;

        if (read) {
            memcpy(port->message_data + port->message_data_pos,
                   vport->chunk_data + pos, read);
            port->message_data_pos += read;
        }

        if (port->message_data_pos == port->message_header.size) {
            if (vport->read_callback) {
                int r = vport->read_callback(vport, vport->chunk_header.port,
                                 &port->message_header, port->message_data);
                if (r == -1) {
                    vdagent_virtio_port_destroy(vportp);
                    return;
                }
            }
            port->message_header_read = 0;
            port->message_data_pos = 0;
            free(port->message_data);
            port->message_data = NULL;
        }
    }
}

static int vport_read(struct vdagent_virtio_port *vport, uint8_t *buf, int len)
{
    if (vport->is_uds) {
        return recv(vport->fd, buf, len, 0);
    } else {
        return read(vport->fd, buf, len);
    }
}

static void vdagent_virtio_port_do_read(struct vdagent_virtio_port **vportp)
{
    ssize_t n;
    size_t to_read;
    uint8_t *dest;
    struct vdagent_virtio_port *vport = *vportp;

    if (vport->chunk_header_read < sizeof(vport->chunk_header)) {
        to_read = sizeof(vport->chunk_header) - vport->chunk_header_read;
        dest = (uint8_t *)&vport->chunk_header + vport->chunk_header_read;
    } else {
        to_read = vport->chunk_header.size - vport->chunk_data_pos;
        dest = vport->chunk_data + vport->chunk_data_pos;
    }

    n = vport_read(vport, dest, to_read);
    if (n < 0) {
        if (errno == EINTR)
            return;
        syslog(LOG_ERR, "reading from vdagent virtio port: %m");
    }
    if (n == 0 && vport->opening) {
        /* When we open the virtio serial port, the following happens:
           1) The linux kernel virtio_console driver sends a
              VIRTIO_CONSOLE_PORT_OPEN message to qemu
           2) qemu's spicevmc chardev driver calls qemu_spice_add_interface to
              register the agent chardev with the spice-server
           3) spice-server then calls the spicevmc chardev driver's state
              callback to let it know it is ready to receive data
           4) The state callback sends a CHR_EVENT_OPENED to the virtio-console
              chardev backend
           5) The virtio-console chardev backend sends VIRTIO_CONSOLE_PORT_OPEN
              to the linux kernel virtio_console driver

           Until steps 1 - 5 have completed the linux kernel virtio_console
           driver sees the virtio serial port as being in a disconnected state
           and read will return 0 ! So if we blindly assume that a read 0 means
           that the channel is closed we will hit a race here.

           Therefore we ignore read returning 0 until we've successfully read
           or written some data. If we hit this race we also sleep a bit here
           to avoid busy waiting until the above steps complete */
        usleep(10000);
        return;
    }
    if (n <= 0) {
        vdagent_virtio_port_destroy(vportp);
        return;
    }
    vport->opening = 0;

    if (vport->chunk_header_read < sizeof(vport->chunk_header)) {
        vport->chunk_header_read += n;
        if (vport->chunk_header_read == sizeof(vport->chunk_header)) {
            vport->chunk_header.size = GUINT32_FROM_LE(vport->chunk_header.size);
            vport->chunk_header.port = GUINT32_FROM_LE(vport->chunk_header.port);
            if (vport->chunk_header.size > VD_AGENT_MAX_DATA_SIZE) {
                syslog(LOG_ERR, "chunk size %u too large",
                       vport->chunk_header.size);
                vdagent_virtio_port_destroy(vportp);
                return;
            }
            if (vport->chunk_header.port >= VDP_END_PORT) {
                syslog(LOG_ERR, "chunk port %u out of range",
                       vport->chunk_header.port);
                vdagent_virtio_port_destroy(vportp);
                return;
            }
        }
    } else {
        vport->chunk_data_pos += n;
        if (vport->chunk_data_pos == vport->chunk_header.size) {
            vdagent_virtio_port_do_chunk(vportp);
            if (!*vportp)
                return;
            vport->chunk_header_read = 0;
            vport->chunk_data_pos = 0;
        }
    }
}

static int vport_write(struct vdagent_virtio_port *vport, uint8_t *buf, int len)
{
    if (vport->is_uds) {
        return send(vport->fd, buf, len, 0);
    } else {
        return write(vport->fd, buf, len);
    }
}

static void vdagent_virtio_port_do_write(struct vdagent_virtio_port **vportp)
{
    ssize_t n;
    size_t to_write;
    struct vdagent_virtio_port *vport = *vportp;

    struct vdagent_virtio_port_buf* wbuf = vport->write_buf;
    if (!wbuf) {
        syslog(LOG_ERR, "do_write called on a port without a write buf ?!");
        return;
    }

    if (wbuf->write_pos != wbuf->size) {
        syslog(LOG_ERR, "do_write: buffer is incomplete!!");
        return;
    }

    to_write = wbuf->size - wbuf->pos;
    n = vport_write(vport, wbuf->buf + wbuf->pos, to_write);
    if (n < 0) {
        if (errno == EINTR)
            return;
        syslog(LOG_ERR, "writing to vdagent virtio port: %m");
        vdagent_virtio_port_destroy(vportp);
        return;
    }
    if (n > 0)
        vport->opening = 0;

    wbuf->pos += n;
    if (wbuf->pos == wbuf->size) {
        vport->write_buf = wbuf->next;
        free(wbuf->buf);
        free(wbuf);
    }
}

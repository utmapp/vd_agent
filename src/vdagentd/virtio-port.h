/*  virtio-port.h virtio port communication header

    Copyright 2010 - 2016 Red Hat, Inc.

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

#ifndef __VIRTIO_PORT_H
#define __VIRTIO_PORT_H

#include <stdint.h>
#include <spice/vd_agent.h>
#include <glib-object.h>
#include "vdagent-connection.h"

G_BEGIN_DECLS

#define VIRTIO_TYPE_PORT virtio_port_get_type()
G_DECLARE_FINAL_TYPE(VirtioPort, virtio_port, VIRTIO, PORT, VDAgentConnection)

/* Callbacks with this type will be called when a complete message has been
   received. */
typedef void (*vdagent_virtio_port_read_callback)(
    VirtioPort *vport,
    int port_nr,
    VDAgentMessage *message_header,
    uint8_t *data);

/* Create a vdagent virtio port object for port portname */
VirtioPort *vdagent_virtio_port_create(const char *portname,
    vdagent_virtio_port_read_callback read_callback,
    VDAgentConnErrorCb error_cb);

/* Queue a message for delivery, either bit by bit, or all at once */
void vdagent_virtio_port_write_start(
        VirtioPort *vport,
        uint32_t port_nr,
        uint32_t message_type,
        uint32_t message_opaque,
        uint32_t data_size);

int vdagent_virtio_port_write_append(
        VirtioPort *vport,
        const uint8_t *data,
        uint32_t size);

void vdagent_virtio_port_write(
        VirtioPort *vport,
        uint32_t port_nr,
        uint32_t message_type,
        uint32_t message_opaque,
        const uint8_t *data,
        uint32_t data_size);

void vdagent_virtio_port_reset(VirtioPort *vport, int port);

G_END_DECLS

#endif

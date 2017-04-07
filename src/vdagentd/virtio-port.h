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

#include <stdio.h>
#include <stdint.h>
#include <sys/select.h>
#include <spice/vd_agent.h>

struct vdagent_virtio_port;

/* Callbacks with this type will be called when a complete message has been
   received. Sometimes the callback may want to close the port, in this
   case do *not* call vdagent_virtio_port_destroy from the callback. The desire
   to close the port can be indicated be returning -1 from the callback,
   in other cases return 0. */
typedef int (*vdagent_virtio_port_read_callback)(
    struct vdagent_virtio_port *vport,
    int port_nr,
    VDAgentMessage *message_header,
    uint8_t *data);

/* Callbacks with this type will be called when the port is disconnected.
   Note:
   1) vdagent_virtio_port will destroy the port in question itself after
      this callback has completed!
   2) This callback is always called, even if the disconnect is initiated
      by the vdagent_virtio_port user through returning -1 from a read
      callback, or by explicitly calling vdagent_virtio_port_destroy */
typedef void (*vdagent_virtio_port_disconnect_callback)(
    struct vdagent_virtio_port *conn);


/* Create a vdagent virtio port object for port portname */
struct vdagent_virtio_port *vdagent_virtio_port_create(const char *portname,
    vdagent_virtio_port_read_callback read_callback,
    vdagent_virtio_port_disconnect_callback disconnect_callback);

/* The contents of portp will be made NULL */
void vdagent_virtio_port_destroy(struct vdagent_virtio_port **vportp);


/* Given a vdagent_virtio_port fill the fd_sets pointed to by readfds and
   writefds for select() usage.

   Return value: value of the highest fd + 1 */
int vdagent_virtio_port_fill_fds(struct vdagent_virtio_port *vport,
        fd_set *readfds, fd_set *writefds);

/* Handle any events flagged by select for the given vdagent_virtio_port.
   Note the port may be destroyed (when disconnected) by this call
   in this case the disconnect calllback will get called before the
   destruction and the contents of connp will be made NULL */
void vdagent_virtio_port_handle_fds(struct vdagent_virtio_port **vportp,
        fd_set *readfds, fd_set *writefds);


/* Queue a message for delivery, either bit by bit, or all at once

   Returns 0 on success -1 on error (only happens when malloc fails) */
int vdagent_virtio_port_write_start(
        struct vdagent_virtio_port *vport,
        uint32_t port_nr,
        uint32_t message_type,
        uint32_t message_opaque,
        uint32_t data_size);

int vdagent_virtio_port_write_append(
        struct vdagent_virtio_port *vport,
        const uint8_t *data,
        uint32_t size);

int vdagent_virtio_port_write(
        struct vdagent_virtio_port *vport,
        uint32_t port_nr,
        uint32_t message_type,
        uint32_t message_opaque,
        const uint8_t *data,
        uint32_t data_size);

void vdagent_virtio_port_flush(struct vdagent_virtio_port **vportp);
void vdagent_virtio_port_reset(struct vdagent_virtio_port *vport, int port);

#endif

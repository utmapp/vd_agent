/*  vdagentd-proto.h header file for the protocol over the unix domain socket
    between the vdagent process / xorg-client and the vdagentd (daemon).

    Copyright 2010-2013 Red Hat, Inc.

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

#ifndef __VDAGENTD_PROTO_H
#define __VDAGENTD_PROTO_H

#define VDAGENTD_SOCKET "/var/run/spice-vdagentd/spice-vdagent-sock"

#define DEFAULT_VIRTIO_PORT_PATH "/dev/virtio-ports/com.redhat.spice.0"

enum {
    VDAGENTD_GUEST_XORG_RESOLUTION, /* client -> daemon, arg1: overall width,
                                       arg2: overall height, data: array of
                                       vdagentd_guest_xorg_resolution */
    VDAGENTD_MONITORS_CONFIG, /* daemon -> client, VDAgentMonitorsConfig
                                 followed by num_monitors VDAgentMonConfig-s */
    VDAGENTD_CLIPBOARD_GRAB,    /* arg1: sel, data: array of supported types */
    VDAGENTD_CLIPBOARD_REQUEST, /* arg1: selection, arg 2 = type */
    VDAGENTD_CLIPBOARD_DATA,    /* arg1: sel, arg 2: type, data: data */
    VDAGENTD_CLIPBOARD_RELEASE, /* arg1: selection */
    VDAGENTD_VERSION,           /* daemon -> client, data: version string */
    VDAGENTD_AUDIO_VOLUME_SYNC,
    VDAGENTD_FILE_XFER_START,
    VDAGENTD_FILE_XFER_STATUS,
    VDAGENTD_FILE_XFER_DATA,
    VDAGENTD_FILE_XFER_DISABLE,
    VDAGENTD_CLIENT_DISCONNECTED,  /* daemon -> client */
    VDAGENTD_NO_MESSAGES /* Must always be last */
};

struct vdagentd_guest_xorg_resolution {
    int width;
    int height;
    int x;
    int y;
};

#endif

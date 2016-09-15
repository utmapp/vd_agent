/*  x11.h vdagent x11 code header file

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

#ifndef __VDAGENT_X11_H
#define __VDAGENT_X11_H

#include <stdio.h>
#include <spice/vd_agent.h>
#include "udscs.h"

struct vdagent_x11;

struct vdagent_x11 *vdagent_x11_create(struct udscs_connection *vdagentd,
    int debug, int sync);
void vdagent_x11_destroy(struct vdagent_x11 *x11, int vdagentd_disconnected);

int  vdagent_x11_get_fd(struct vdagent_x11 *x11);
void vdagent_x11_do_read(struct vdagent_x11 *x11);

void vdagent_x11_set_monitor_config(struct vdagent_x11 *x11,
    VDAgentMonitorsConfig *mon_config, int fallback);

void vdagent_x11_clipboard_grab(struct vdagent_x11 *x11, uint8_t selection,
    uint32_t *types, uint32_t type_count);
void vdagent_x11_clipboard_request(struct vdagent_x11 *x11,
    uint8_t selection, uint32_t type);
void vdagent_x11_clipboard_data(struct vdagent_x11 *x11, uint8_t selection,
    uint32_t type, uint8_t *data, uint32_t size);
void vdagent_x11_clipboard_release(struct vdagent_x11 *x11, uint8_t selection);

void vdagent_x11_client_disconnected(struct vdagent_x11 *x11);

int vdagent_x11_has_icons_on_desktop(struct vdagent_x11 *x11);

#endif

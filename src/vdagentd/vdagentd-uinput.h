/*  vdagentd-uinput.c vdagentd uinput handling header

    Copyright 2010-2012 Red Hat, Inc.

    Red Hat Authors:
    Hans de Goede <hdegoede@redhat.com>
    Gerd Hoffmann <kraxel@redhat.com>

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
#ifndef __VDAGENTD_UINPUT_H
#define __VDAGENTD_UINPUT_H

#include <stdio.h>
#include "vdagentd-proto.h"

struct vdagentd_uinput;

struct vdagentd_uinput *vdagentd_uinput_create(const char *devname,
    int width, int height,
    struct vdagentd_guest_xorg_resolution *screen_info, int screen_count,
    int debug, int fake);
void vdagentd_uinput_destroy(struct vdagentd_uinput **uinputp);

void vdagentd_uinput_do_mouse(struct vdagentd_uinput **uinputp,
        VDAgentMouseState *mouse);
void vdagentd_uinput_update_size(struct vdagentd_uinput **uinputp,
        int width, int height,
        struct vdagentd_guest_xorg_resolution *screen_info,
        int screen_count);

#endif

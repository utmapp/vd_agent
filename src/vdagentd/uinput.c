/*  vdagentd-uinput.c vdagentd uinput handling code

    Copyright 2010-2016 Red Hat, Inc.

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
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <spice/vd_agent.h>
#include "uinput.h"

struct vdagentd_uinput {
    const char *devname;
    int fd;
    int debug;
    int width;
    int height;
    struct vdagentd_guest_xorg_resolution *screen_info;
    int screen_count;
    VDAgentMouseState last;
    int fake;
};

struct vdagentd_uinput *vdagentd_uinput_create(const char *devname,
    int width, int height,
    struct vdagentd_guest_xorg_resolution *screen_info, int screen_count,
    int debug, int fake)
{
    struct vdagentd_uinput *uinput;

    uinput = calloc(1, sizeof(*uinput));
    if (!uinput)
        return NULL;

    uinput->devname = devname;
    uinput->fd      = -1; /* Gets opened by vdagentd_uinput_update_size() */
    uinput->debug   = debug;
    uinput->fake    = fake;

    vdagentd_uinput_update_size(&uinput, width, height,
                                screen_info, screen_count);

    return uinput;
}

void vdagentd_uinput_destroy(struct vdagentd_uinput **uinputp)
{
    struct vdagentd_uinput *uinput = *uinputp;

    if (!uinput)
        return;

    if (uinput->fd != -1)
        close(uinput->fd);
    free(uinput);
    *uinputp = NULL;
}

void vdagentd_uinput_update_size(struct vdagentd_uinput **uinputp,
        int width, int height,
        struct vdagentd_guest_xorg_resolution *screen_info,
        int screen_count)
{
    struct vdagentd_uinput *uinput = *uinputp;
    struct uinput_user_dev device = {
        .name = "spice vdagent tablet",
#ifdef WITH_STATIC_UINPUT
        .absmax  [ ABS_X ] = 32767,
        .absmax  [ ABS_Y ] = 32767,
#else
        .absmax  [ ABS_X ] = width - 1,
        .absmax  [ ABS_Y ] = height - 1,
#endif
    };
    int i, rc;

    if (uinput->debug) {
        syslog(LOG_DEBUG, "uinput-update-size: %dx%d", width, height);
        for (i = 0; i < screen_count; i++)
            syslog(LOG_DEBUG, "screen %d: +%d+%d", i, screen_info[i].x,
                   screen_info[i].y);
    }

    uinput->screen_info  = screen_info;
    uinput->screen_count = screen_count;

    if (uinput->width == width && uinput->height == height)
        return;

    uinput->width  = width;
    uinput->height = height;

    if (uinput->fd != -1)
#ifndef WITH_STATIC_UINPUT
        close(uinput->fd);
#else
        return;
#endif

    uinput->fd = open(uinput->devname, uinput->fake ? O_WRONLY : O_RDWR);
    if (uinput->fd == -1) {
        syslog(LOG_ERR, "open %s: %m", uinput->devname);
        vdagentd_uinput_destroy(uinputp);
        return;
    }

    if (uinput->fake) {
        /* fake device doesn't understand any ioctls and only writes events */
        return;
    }

    rc = write(uinput->fd, &device, sizeof(device));
    if (rc != sizeof(device)) {
        syslog(LOG_ERR, "write %s: %m", uinput->devname);
        vdagentd_uinput_destroy(uinputp);
        return;
    }

    /* buttons */
    ioctl(uinput->fd, UI_SET_EVBIT, EV_KEY);
    ioctl(uinput->fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(uinput->fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(uinput->fd, UI_SET_KEYBIT, BTN_RIGHT);

    /* wheel */
    ioctl(uinput->fd, UI_SET_EVBIT, EV_REL);
    ioctl(uinput->fd, UI_SET_RELBIT, REL_WHEEL);

    /* abs ptr */
    ioctl(uinput->fd, UI_SET_EVBIT, EV_ABS);
    ioctl(uinput->fd, UI_SET_ABSBIT, ABS_X);
    ioctl(uinput->fd, UI_SET_ABSBIT, ABS_Y);

    rc = ioctl(uinput->fd, UI_DEV_CREATE);
    if (rc < 0) {
        syslog(LOG_ERR, "create %s: %m", uinput->devname);
        vdagentd_uinput_destroy(uinputp);
    }
}

static void uinput_send_event(struct vdagentd_uinput **uinputp,
    __u16 type, __u16 code, __s32 value)
{
    struct vdagentd_uinput *uinput = *uinputp;
    struct input_event event = {
        .type  = type,
        .code  = code,
        .value = value,
    };
    int rc;

    rc = write(uinput->fd, &event, sizeof(event));
    if (rc != sizeof(event)) {
        syslog(LOG_ERR, "write %s: %m", uinput->devname);
        vdagentd_uinput_destroy(uinputp);
    }
}

void vdagentd_uinput_do_mouse(struct vdagentd_uinput **uinputp,
        VDAgentMouseState *mouse)
{
    struct vdagentd_uinput *uinput = *uinputp;
    struct button_s {
        const char *name;
        int mask;
        int btn;
    };
    static const struct button_s btns[] = {
        { .name = "left",   .mask =  VD_AGENT_LBUTTON_MASK, .btn = BTN_LEFT      },
        { .name = "middle", .mask =  VD_AGENT_MBUTTON_MASK, .btn = BTN_MIDDLE    },
        { .name = "right",  .mask =  VD_AGENT_RBUTTON_MASK, .btn = BTN_RIGHT     },
    };
    static const struct button_s wheel[] = {
        { .name = "up",     .mask =  VD_AGENT_UBUTTON_MASK, .btn = 1  },
        { .name = "down",   .mask =  VD_AGENT_DBUTTON_MASK, .btn = -1 },
    };
    int i, down;

    if (*uinputp) {
        if (mouse->display_id >= uinput->screen_count) {
            syslog(LOG_WARNING, "mouse event for unknown monitor (%d >= %d)",
                   mouse->display_id, uinput->screen_count);
            return;
        }
        if (uinput->debug)
            syslog(LOG_DEBUG, "mouse-event: mon %d %dx%d", mouse->display_id,
                   mouse->x, mouse->y);
        mouse->x += uinput->screen_info[mouse->display_id].x;
        mouse->y += uinput->screen_info[mouse->display_id].y;
#ifdef WITH_STATIC_UINPUT
        mouse->x = mouse->x * 32767 / (uinput->width - 1);
        mouse->y = mouse->y * 32767 / (uinput->height - 1);
#endif
    }

    if (*uinputp && uinput->last.x != mouse->x) {
        if (uinput->debug)
            syslog(LOG_DEBUG, "mouse: abs-x %d", mouse->x);
        uinput_send_event(uinputp, EV_ABS, ABS_X, mouse->x);
    }
    if (*uinputp && uinput->last.y != mouse->y) {
        if (uinput->debug)
            syslog(LOG_DEBUG, "mouse: abs-y %d", mouse->y);
        uinput_send_event(uinputp, EV_ABS, ABS_Y, mouse->y);
    }
    for (i = 0; i < sizeof(btns)/sizeof(btns[0]) && *uinputp; i++) {
        if ((uinput->last.buttons & btns[i].mask) ==
                (mouse->buttons & btns[i].mask))
            continue;
        down = !!(mouse->buttons & btns[i].mask);
        if (uinput->debug)
            syslog(LOG_DEBUG, "mouse: btn-%s %s",
                    btns[i].name, down ? "down" : "up");
        uinput_send_event(uinputp, EV_KEY, btns[i].btn, down);
    }
    for (i = 0; i < sizeof(wheel)/sizeof(wheel[0]) && *uinputp; i++) {
        if ((uinput->last.buttons & wheel[i].mask) ==
                (mouse->buttons & wheel[i].mask))
            continue;
        if (mouse->buttons & wheel[i].mask) {
            if (uinput->debug)
                syslog(LOG_DEBUG, "mouse: wheel-%s", wheel[i].name);
            uinput_send_event(uinputp, EV_REL, REL_WHEEL, wheel[i].btn);
        }
    }

    if (*uinputp) {
        if (uinput->debug)
            syslog(LOG_DEBUG, "mouse: syn");
        uinput_send_event(uinputp, EV_SYN, SYN_REPORT, 0);
    }

    if (*uinputp)
        uinput->last = *mouse;
}

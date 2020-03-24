/*
 * display.h- vdagent display handling header

    Copyright 2020 Red Hat, Inc.

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

#ifndef SRC_VDAGENT_DISPLAY_H_
#define SRC_VDAGENT_DISPLAY_H_

typedef struct VDAgentDisplay VDAgentDisplay;

VDAgentDisplay *vdagent_display_create(UdscsConnection *vdagentd, int debug, int sync);
void vdagent_display_destroy(VDAgentDisplay *display, int vdagentd_disconnected);

gboolean vdagent_display_has_icons_on_desktop(VDAgentDisplay *display);
void vdagent_display_handle_graphics_device_info(VDAgentDisplay *display, uint8_t *data, size_t size);
void vdagent_display_set_monitor_config(VDAgentDisplay *display,
                                        VDAgentMonitorsConfig *mon_config,
                                        int fallback);

struct vdagent_x11 *vdagent_display_get_x11(VDAgentDisplay *display);

void vdagent_display_send_daemon_guest_res(VDAgentDisplay *display, gboolean update);


#endif /* SRC_VDAGENT_DISPLAY_H_ */

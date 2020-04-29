/*  device-info.c utility function for looking up the xrandr output id for a
 *  given device address and display id
 *
 * Copyright 2018 Red Hat, Inc.
 *
 * Red Hat Authors:
 * Jonathon Jongsma <jjongsma@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <spice/vd_agent.h>
#include <X11/extensions/Xrandr.h>
#include <stdbool.h>

struct vdagent_x11;
bool lookup_xrandr_output_for_device_info(VDAgentDeviceDisplayInfo *device_info,
                                          Display *xdisplay,
                                          XRRScreenResources *xres,
                                          RROutput *output_id,
                                          bool has_virtual_zero_display);

int get_connector_name_for_device_info(VDAgentDeviceDisplayInfo *device_info,
                                       char *expected_name, size_t name_size,
                                       bool has_virtual_zero_display);

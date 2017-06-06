/*  vdagent file xfers header

    Copyright 2013 Red Hat, Inc.

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

#ifndef __VDAGENT_FILE_XFERS_H
#define __VDAGENT_FILE_XFERS_H

#include "udscs.h"

struct vdagent_file_xfers;

struct vdagent_file_xfers *vdagent_file_xfers_create(
        struct udscs_connection *vdagentd, const char *save_dir,
        int open_save_dir, int debug);
void vdagent_file_xfers_destroy(struct vdagent_file_xfers *xfer);

void vdagent_file_xfers_start(struct vdagent_file_xfers *xfers,
    VDAgentFileXferStartMessage *msg);
void vdagent_file_xfers_status(struct vdagent_file_xfers *xfers,
    VDAgentFileXferStatusMessage *msg);
void vdagent_file_xfers_data(struct vdagent_file_xfers *xfers,
    VDAgentFileXferDataMessage *msg);
void vdagent_file_xfers_error_disabled(struct udscs_connection *vdagentd,
    uint32_t msg_id);

#endif

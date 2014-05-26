/*  console-kit.h vdagentd ConsoleKit integration code - header

    Copyright 2010-2012 Red Hat, Inc.

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

#ifndef __SESSION_INFO_H
#define __SESSION_INFO_H

#include <stdio.h>
#include <stdint.h>
#include <glib.h>

struct session_info;

struct session_info *session_info_create(int verbose);
void session_info_destroy(struct session_info *ck);

int session_info_get_fd(struct session_info *ck);

const char *session_info_get_active_session(struct session_info *ck);
/* Note result must be free()-ed by caller */
char *session_info_session_for_pid(struct session_info *ck, uint32_t pid);

gboolean session_info_session_is_locked(struct session_info *si);
gboolean session_info_is_user(struct session_info *si);

#endif

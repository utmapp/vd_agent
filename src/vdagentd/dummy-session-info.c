/*  dummy-session-info.c spice vdagentd dummy session info handler

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

#include "session-info.h"

struct session_info *session_info_create(int verbose)
{
    return NULL;
}

void session_info_destroy(struct session_info *si)
{
}

int session_info_get_fd(struct session_info *si)
{
    return -1;
}

const char *session_info_get_active_session(struct session_info *si)
{
    return NULL;
}

char *session_info_session_for_pid(struct session_info *si, uint32_t pid)
{
    return NULL;
}

gboolean session_is_locked(struct session_info *ck)
{
    return FALSE;
}

gboolean session_info_is_user(struct session_info *si)
{
    return TRUE;
}

gboolean session_info_session_is_locked(G_GNUC_UNUSED struct session_info *si)
{
    return FALSE;
}

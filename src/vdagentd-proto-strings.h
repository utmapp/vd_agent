/*  vdagentd-proto-strings.h header file

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

#ifndef __VDAGENTD_PROTO_STRINGS_H
#define __VDAGENTD_PROTO_STRINGS_H

static const char * const vdagentd_messages[] = {
        "guest xorg resolution",
        "monitors config",
        "clipboard grab",
        "clipboard request",
        "clipboard data",
        "clipboard release",
        "version",
        "audio volume sync",
        "file xfer start",
        "file xfer status",
        "file xfer data",
        "file xfer disable",
        "client disconnected",
        "graphics device info",
};

#endif

/*  clipboard.c - vdagent clipboard handling code

    Copyright 2017 Red Hat, Inc.

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
# include <config.h>
#endif

#include "clipboard.h"

struct VDAgentClipboards {
    struct vdagent_x11 *x11;
};

void vdagent_clipboard_grab(VDAgentClipboards *c, guint sel_id,
                            guint32 *types, guint n_types)
{
    vdagent_x11_clipboard_grab(c->x11, sel_id, types, n_types);
}

void vdagent_clipboard_data(VDAgentClipboards *c, guint sel_id,
                            guint type, guchar *data, guint size)
{
    vdagent_x11_clipboard_data(c->x11, sel_id, type, data, size);
}

void vdagent_clipboard_release(VDAgentClipboards *c, guint sel_id)
{
    vdagent_x11_clipboard_release(c->x11, sel_id);
}

void vdagent_clipboards_release_all(VDAgentClipboards *c)
{
    vdagent_x11_client_disconnected(c->x11);
}

void vdagent_clipboard_request(VDAgentClipboards *c, guint sel_id, guint type)
{
    vdagent_x11_clipboard_request(c->x11, sel_id, type);
}

VDAgentClipboards *vdagent_clipboards_init(struct vdagent_x11 *x11)
{
    VDAgentClipboards *c;
    c = g_new0(VDAgentClipboards, 1);
    c->x11 = x11;

    return c;
}

void vdagent_clipboards_finalize(VDAgentClipboards *c)
{
    g_free(c);
}

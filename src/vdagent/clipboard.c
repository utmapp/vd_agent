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
#include <config.h>

#ifdef WITH_GTK
# include <gtk/gtk.h>
# include <syslog.h>

# include "vdagentd-proto.h"
# include "spice/vd_agent.h"
#endif

#include "clipboard.h"

#ifdef WITH_GTK
/* 2 selections supported - _SELECTION_CLIPBOARD = 0, _SELECTION_PRIMARY = 1 */
#define SELECTION_COUNT (VD_AGENT_CLIPBOARD_SELECTION_PRIMARY + 1)
#define TYPE_COUNT      (VD_AGENT_CLIPBOARD_IMAGE_JPG + 1)

static const GdkAtom sel_atom[] = {
    [VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD] = GDK_SELECTION_CLIPBOARD,
    [VD_AGENT_CLIPBOARD_SELECTION_PRIMARY] = GDK_SELECTION_PRIMARY,
};

G_STATIC_ASSERT(G_N_ELEMENTS(sel_atom) == SELECTION_COUNT);

static gint sel_id_from_clip(GtkClipboard *clipboard)
{
    GdkAtom sel = gtk_clipboard_get_selection(clipboard);
    int i;

    for (i = 0; i < G_N_ELEMENTS(sel_atom); i++) {
        if (sel == sel_atom[i]) {
            return i;
        }
    }

    g_return_val_if_reached(0);
}

enum {
    OWNER_NONE,
    OWNER_GUEST,
    OWNER_CLIENT
};

typedef struct {
    GMainLoop        *loop;
    GtkSelectionData *sel_data;
} AppRequest;

typedef struct {
    GtkClipboard *clipboard;
    guint         owner;

    GList        *requests_from_apps; /* VDAgent --> Client */
    GList        *requests_from_client; /* Client --> VDAgent */
    gpointer     *last_targets_req;

    GdkAtom       targets[TYPE_COUNT];
} Selection;
#endif

struct _VDAgentClipboards {
    GObject parent;

    UdscsConnection *conn;

#ifdef WITH_GTK
    Selection selections[SELECTION_COUNT];
#else
    struct vdagent_x11 *x11;
#endif
};

struct _VDAgentClipboardsClass
{
    GObjectClass parent;
};

G_DEFINE_TYPE(VDAgentClipboards, vdagent_clipboards, G_TYPE_OBJECT)

#ifdef WITH_GTK
static const struct {
    guint         type;
    const gchar  *atom_name;
} atom2agent[] = {
    {VD_AGENT_CLIPBOARD_UTF8_TEXT, "UTF8_STRING"},
    {VD_AGENT_CLIPBOARD_UTF8_TEXT, "text/plain;charset=utf-8"},
    {VD_AGENT_CLIPBOARD_UTF8_TEXT, "STRING"},
    {VD_AGENT_CLIPBOARD_UTF8_TEXT, "TEXT"},
    {VD_AGENT_CLIPBOARD_UTF8_TEXT, "text/plain"},
    {VD_AGENT_CLIPBOARD_IMAGE_PNG, "image/png"},
    {VD_AGENT_CLIPBOARD_IMAGE_BMP, "image/bmp"},
    {VD_AGENT_CLIPBOARD_IMAGE_BMP, "image/x-bmp"},
    {VD_AGENT_CLIPBOARD_IMAGE_BMP, "image/x-MS-bmp"},
    {VD_AGENT_CLIPBOARD_IMAGE_BMP, "image/x-win-bitmap"},
    {VD_AGENT_CLIPBOARD_IMAGE_TIFF,"image/tiff"},
    {VD_AGENT_CLIPBOARD_IMAGE_JPG, "image/jpeg"},
};

static guint get_type_from_atom(GdkAtom atom)
{
    gchar *name = gdk_atom_name(atom);
    int i;
    for (i = 0; i < G_N_ELEMENTS(atom2agent); i++) {
        if (!g_ascii_strcasecmp(name, atom2agent[i].atom_name)) {
            g_free(name);
            return atom2agent[i].type;
        }
    }
    g_free(name);
    return VD_AGENT_CLIPBOARD_NONE;
}

/* gtk_clipboard_request_(, callback, user_data) cannot be cancelled.
   Instead, gpointer *ref = request_ref_new() is passed to the callback.
   Callback can check using request_ref_is_cancelled(ref)
   whether request_ref_cancel(ref) was called.
   This mechanism enables cancellation of the request
   as well as passing VDAgentClipboards reference to the desired callback.
 */
static gpointer *request_ref_new(gpointer data)
{
    gpointer *ref = g_new(gpointer, 1);
    *ref = data;
    return ref;
}

static gpointer request_ref_free(gpointer *ref)
{
    gpointer data = *ref;
    g_free(ref);
    return data;
}

static void request_ref_cancel(gpointer *ref)
{
    g_return_if_fail(ref != NULL);
    *ref = NULL;
}

static gboolean request_ref_is_cancelled(gpointer *ref)
{
    g_return_val_if_fail(ref != NULL, TRUE);
    return *ref == NULL;
}

static void clipboard_new_owner(VDAgentClipboards *c, guint sel_id, guint new_owner)
{
    Selection *sel = &c->selections[sel_id];
    GList *l;
    /* let the other apps know no data is coming */
    for (l = sel->requests_from_apps; l != NULL; l= l->next) {
        AppRequest *req = l->data;
        g_main_loop_quit(req->loop);
    }
    g_clear_pointer(&sel->requests_from_apps, g_list_free);

    /* respond to pending client's data requests */
    for (l = sel->requests_from_client; l != NULL; l = l->next) {
        request_ref_cancel(l->data);
        if (c->conn)
            udscs_write(c->conn, VDAGENTD_CLIPBOARD_DATA,
                        sel_id, VD_AGENT_CLIPBOARD_NONE, NULL, 0);
    }
    g_clear_pointer(&sel->requests_from_client, g_list_free);

    sel->owner = new_owner;
}

static void clipboard_targets_received_cb(GtkClipboard *clipboard,
                                          GdkAtom      *atoms,
                                          gint          n_atoms,
                                          gpointer      user_data)
{
    if (request_ref_is_cancelled(user_data))
        return;

    VDAgentClipboards *c = request_ref_free(user_data);
    Selection *sel;
    guint32 types[G_N_ELEMENTS(atom2agent)];
    guint sel_id, type, n_types, a;

    sel_id = sel_id_from_clip(clipboard);
    sel = &c->selections[sel_id];
    sel->last_targets_req = NULL;

    if (atoms == NULL)
        return;

    for (type = 0; type < TYPE_COUNT; type++)
        sel->targets[type] = GDK_NONE;

    n_types = 0;
    for (a = 0; a < n_atoms; a++) {
        type = get_type_from_atom(atoms[a]);
        if (type == VD_AGENT_CLIPBOARD_NONE || sel->targets[type] != GDK_NONE)
            continue;

        sel->targets[type] = atoms[a];
        types[n_types] = type;
        n_types++;
    }

    if (n_types == 0) {
        syslog(LOG_WARNING, "%s: sel_id=%u: no target supported", __func__, sel_id);
        return;
    }

    clipboard_new_owner(c, sel_id, OWNER_GUEST);

    udscs_write(c->conn, VDAGENTD_CLIPBOARD_GRAB, sel_id, 0,
                (guint8 *)types, n_types * sizeof(guint32));
}

static void clipboard_owner_change_cb(GtkClipboard        *clipboard,
                                      GdkEventOwnerChange *event,
                                      gpointer             user_data)
{
    VDAgentClipboards *c = user_data;
    guint sel_id = sel_id_from_clip(clipboard);
    Selection *sel = &c->selections[sel_id];

    /* if the event was caused by gtk_clipboard_set_with_data(), ignore it  */
    if (gtk_clipboard_get_owner(clipboard) == G_OBJECT(c)) {
        return;
    }

    if (event->reason != GDK_OWNER_CHANGE_NEW_OWNER) {
        if (sel->owner == OWNER_GUEST) {
            clipboard_new_owner(c, sel_id, OWNER_NONE);
            udscs_write(c->conn, VDAGENTD_CLIPBOARD_RELEASE, sel_id, 0, NULL, 0);
        }
        return;
    }

    /* if there's a pending request for clipboard targets, cancel it */
    if (sel->last_targets_req)
        request_ref_cancel(sel->last_targets_req);

    sel->last_targets_req = request_ref_new(c);
    gtk_clipboard_request_targets(clipboard, clipboard_targets_received_cb,
                                  sel->last_targets_req);
}

static void clipboard_contents_received_cb(GtkClipboard     *clipboard,
                                           GtkSelectionData *sel_data,
                                           gpointer          user_data)
{
    if (request_ref_is_cancelled(user_data))
        return;

    VDAgentClipboards *c = request_ref_free(user_data);
    guint sel_id, type, target;

    sel_id = sel_id_from_clip(clipboard);
    c->selections[sel_id].requests_from_client =
        g_list_remove(c->selections[sel_id].requests_from_client, user_data);

    type = get_type_from_atom(gtk_selection_data_get_data_type(sel_data));
    target = get_type_from_atom(gtk_selection_data_get_target(sel_data));

    if (type == target) {
        udscs_write(c->conn, VDAGENTD_CLIPBOARD_DATA, sel_id, type,
                    gtk_selection_data_get_data(sel_data),
                    gtk_selection_data_get_length(sel_data));
    } else {
        syslog(LOG_WARNING, "%s: sel_id=%u: expected type %u, recieved %u, "
                            "skipping", __func__, sel_id, target, type);
        udscs_write(c->conn, VDAGENTD_CLIPBOARD_DATA, sel_id,
                    VD_AGENT_CLIPBOARD_NONE, NULL, 0);
    }
}

static void clipboard_get_cb(GtkClipboard     *clipboard,
                             GtkSelectionData *sel_data,
                             guint             info,
                             gpointer          user_data)
{
    AppRequest req;
    VDAgentClipboards *c = user_data;
    guint sel_id, type;

    sel_id = sel_id_from_clip(clipboard);
    g_return_if_fail(c->selections[sel_id].owner == OWNER_CLIENT);

    type = get_type_from_atom(gtk_selection_data_get_target(sel_data));
    g_return_if_fail(type != VD_AGENT_CLIPBOARD_NONE);

    req.sel_data = sel_data;
    req.loop = g_main_loop_new(NULL, FALSE);
    c->selections[sel_id].requests_from_apps =
        g_list_prepend(c->selections[sel_id].requests_from_apps, &req);

    udscs_write(c->conn, VDAGENTD_CLIPBOARD_REQUEST, sel_id, type, NULL, 0);

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gdk_threads_leave();
    g_main_loop_run(req.loop);
    gdk_threads_enter();
G_GNUC_END_IGNORE_DEPRECATIONS

    g_main_loop_unref(req.loop);
}

static void clipboard_clear_cb(GtkClipboard *clipboard, gpointer user_data)
{
    VDAgentClipboards *c = user_data;
    clipboard_new_owner(c, sel_id_from_clip(clipboard), OWNER_NONE);
}
#endif

void vdagent_clipboard_grab(VDAgentClipboards *c, guint sel_id,
                            guint32 *types, guint n_types)
{
#ifndef WITH_GTK
    vdagent_x11_clipboard_grab(c->x11, sel_id, types, n_types);
#else
    GtkTargetEntry targets[G_N_ELEMENTS(atom2agent)];
    Selection *sel;
    guint n_targets, i, t;

    g_return_if_fail(sel_id < SELECTION_COUNT);

    n_targets = 0;
    for (i = 0; i < G_N_ELEMENTS(atom2agent); i++)
        for (t = 0; t < n_types; t++)
            if (atom2agent[i].type == types[t]) {
                targets[n_targets].target = (gchar *)atom2agent[i].atom_name;
                n_targets++;
                break;
            }

    if (n_targets == 0) {
        syslog(LOG_WARNING, "%s: sel_id=%u: no type supported", __func__, sel_id);
        return;
    }

    sel = &c->selections[sel_id];

    if (sel->last_targets_req) {
        g_clear_pointer(&sel->last_targets_req, request_ref_cancel);
    }

    if (gtk_clipboard_set_with_owner(sel->clipboard,
                                     targets, n_targets,
                                     clipboard_get_cb, clipboard_clear_cb,
                                     G_OBJECT(c)))
        clipboard_new_owner(c, sel_id, OWNER_CLIENT);
    else {
        syslog(LOG_ERR, "%s: sel_id=%u: clipboard grab failed", __func__, sel_id);
        clipboard_new_owner(c, sel_id, OWNER_NONE);
    }
#endif
}

void vdagent_clipboard_data(VDAgentClipboards *c, guint sel_id,
                            guint type, guchar *data, guint size)
{
#ifndef WITH_GTK
    vdagent_x11_clipboard_data(c->x11, sel_id, type, data, size);
#else
    g_return_if_fail(sel_id < SELECTION_COUNT);
    Selection *sel = &c->selections[sel_id];
    AppRequest *req;
    GList *l;

    for (l = sel->requests_from_apps; l != NULL; l = l->next) {
        req = l->data;
        if (get_type_from_atom(gtk_selection_data_get_target(req->sel_data)) == type)
            break;
    }
    if (l == NULL) {
        syslog(LOG_WARNING, "%s: sel_id=%u: no corresponding request found for "
                            "type=%u, skipping", __func__, sel_id, type);
        return;
    }
    sel->requests_from_apps = g_list_delete_link(sel->requests_from_apps, l);

    gtk_selection_data_set(req->sel_data,
                           gtk_selection_data_get_target(req->sel_data),
                           8, data, size);

    g_main_loop_quit(req->loop);
#endif
}

void vdagent_clipboard_release(VDAgentClipboards *c, guint sel_id)
{
#ifndef WITH_GTK
    vdagent_x11_clipboard_release(c->x11, sel_id);
#else
    g_return_if_fail(sel_id < SELECTION_COUNT);
    if (c->selections[sel_id].owner != OWNER_CLIENT)
        return;

    clipboard_new_owner(c, sel_id, OWNER_NONE);
    gtk_clipboard_clear(c->selections[sel_id].clipboard);
#endif
}

void vdagent_clipboards_release_all(VDAgentClipboards *c)
{
#ifndef WITH_GTK
    vdagent_x11_client_disconnected(c->x11);
#else
    guint sel_id, owner;

    for (sel_id = 0; sel_id < SELECTION_COUNT; sel_id++) {
        owner = c->selections[sel_id].owner;
        clipboard_new_owner(c, sel_id, OWNER_NONE);
        if (owner == OWNER_CLIENT)
            gtk_clipboard_clear(c->selections[sel_id].clipboard);
        else if (owner == OWNER_GUEST && c->conn)
            udscs_write(c->conn, VDAGENTD_CLIPBOARD_RELEASE, sel_id, 0, NULL, 0);
    }
#endif
}

void vdagent_clipboard_request(VDAgentClipboards *c, guint sel_id, guint type)
{
#ifndef WITH_GTK
    vdagent_x11_clipboard_request(c->x11, sel_id, type);
#else
    Selection *sel;

    if (sel_id >= SELECTION_COUNT)
        goto err;
    sel = &c->selections[sel_id];
    if (sel->owner != OWNER_GUEST) {
        syslog(LOG_WARNING, "%s: sel_id=%d: received request "
                            "while not owning clipboard", __func__, sel_id);
        goto err;
    }
    if (type >= TYPE_COUNT || sel->targets[type] == GDK_NONE) {
        syslog(LOG_WARNING, "%s: sel_id=%d: unadvertised data type requested",
                            __func__, sel_id);
        goto err;
    }

    gpointer *ref = request_ref_new(c);
    sel->requests_from_client = g_list_prepend(sel->requests_from_client, ref);
    gtk_clipboard_request_contents(sel->clipboard, sel->targets[type],
                                   clipboard_contents_received_cb, ref);
    return;
err:
    udscs_write(c->conn, VDAGENTD_CLIPBOARD_DATA, sel_id,
                VD_AGENT_CLIPBOARD_NONE, NULL, 0);
#endif
}

static void
vdagent_clipboards_init(VDAgentClipboards *self)
{
}

VDAgentClipboards *vdagent_clipboards_new(struct vdagent_x11 *x11)
{
    VDAgentClipboards *self = g_object_new(VDAGENT_TYPE_CLIPBOARDS, NULL);

#ifndef WITH_GTK
    self->x11 = x11;
#else
    guint sel_id;

    for (sel_id = 0; sel_id < SELECTION_COUNT; sel_id++) {
        GtkClipboard *clipboard = gtk_clipboard_get(sel_atom[sel_id]);
        self->selections[sel_id].clipboard = clipboard;
        g_signal_connect(G_OBJECT(clipboard), "owner-change",
                         G_CALLBACK(clipboard_owner_change_cb), self);
    }
#endif

    return self;
}

void
vdagent_clipboards_set_conn(VDAgentClipboards *self, UdscsConnection *conn)
{
    self->conn = conn;
}

static void vdagent_clipboards_dispose(GObject *obj)
{
#ifdef WITH_GTK
    VDAgentClipboards *self = VDAGENT_CLIPBOARDS(obj);
    guint sel_id;

    for (sel_id = 0; sel_id < SELECTION_COUNT; sel_id++)
        g_signal_handlers_disconnect_by_func(self->selections[sel_id].clipboard,
            G_CALLBACK(clipboard_owner_change_cb), self);

    if (self->conn)
        vdagent_clipboards_release_all(self);
#endif
}

static void
vdagent_clipboards_class_init(VDAgentClipboardsClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS(klass);

    oclass->dispose = vdagent_clipboards_dispose;
}

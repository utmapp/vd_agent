/*
 * Copyright (C) 2024 osy
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include <config.h>

#include <stdio.h>
#include <syslog.h>
#include <sys/socket.h>
#include <glib-unix.h>
#include <spice/vd_agent.h>

#include "udscs.h"
#include "vdagentd-proto.h"
#include "vdagent.h"

#define MAX_RETRY_CONNECT_SYSTEM_AGENT 60

typedef struct VDAgent {
    UdscsConnection *conn;
    gint udscs_num_retry;
    const vdagent_cb_t *cb;
    void *ctx;

    GMainLoop *loop;
} VDAgent;

static int quit = 0;
static int parent_socket = -1;
static int version_mismatch = 0;

/* Command line options */
static gboolean debug = FALSE;
static gchar *vdagentd_socket = NULL;

static void vdagent_quit_loop(VDAgent *agent)
{
    if (agent->loop)
        g_main_loop_quit(agent->loop);
}

static gboolean vdagent_signal_handler(gpointer user_data)
{
    VDAgent *agent = user_data;
    quit = TRUE;
    vdagent_quit_loop(agent);
    return G_SOURCE_REMOVE;
}

static VDAgent *vdagent_new(const vdagent_cb_t *cb, void *ctx)
{
    VDAgent *agent = g_new0(VDAgent, 1);

    agent->loop = g_main_loop_new(NULL, FALSE);
    agent->cb = cb;
    agent->ctx = ctx;

    g_unix_signal_add(SIGINT, vdagent_signal_handler, agent);
    g_unix_signal_add(SIGHUP, vdagent_signal_handler, agent);
    g_unix_signal_add(SIGTERM, vdagent_signal_handler, agent);

    return agent;
}

static void vdagent_destroy(VDAgent *agent)
{
    g_clear_pointer(&agent->conn, vdagent_connection_destroy);

    while (g_source_remove_by_user_data(agent))
        continue;

    g_clear_pointer(&agent->loop, g_main_loop_unref);
    g_free(agent);
}

void vdagent_set_debug(int debugOption)
{
    debug = debugOption;
}

static guint32 convert_clipboard_type_to_raw(vdagent_clipboard_type_t type)
{
    switch (type) {
        case VDAgentClipboardTypeUTF8Text: return VD_AGENT_CLIPBOARD_UTF8_TEXT;
        case VDAgentClipboardTypeImagePNG: return VD_AGENT_CLIPBOARD_IMAGE_PNG;
        case VDAgentClipboardTypeImageBMP: return VD_AGENT_CLIPBOARD_IMAGE_BMP;
        case VDAgentClipboardTypeImageTIFF: return VD_AGENT_CLIPBOARD_IMAGE_TIFF;
        case VDAgentClipboardTypeImageJPG: return VD_AGENT_CLIPBOARD_IMAGE_JPG;
        case VDAgentClipboardTypeNone:
        default: return VD_AGENT_CLIPBOARD_NONE;
    }
}

static vdagent_clipboard_type_t convert_raw_to_clipboard_type(guint32 type)
{
    switch (type) {
        case VD_AGENT_CLIPBOARD_UTF8_TEXT: return VDAgentClipboardTypeUTF8Text;
        case VD_AGENT_CLIPBOARD_IMAGE_PNG: return VDAgentClipboardTypeImagePNG;
        case VD_AGENT_CLIPBOARD_IMAGE_BMP: return VDAgentClipboardTypeImageBMP;
        case VD_AGENT_CLIPBOARD_IMAGE_TIFF: return VDAgentClipboardTypeImageTIFF;
        case VD_AGENT_CLIPBOARD_IMAGE_JPG: return VDAgentClipboardTypeImageJPG;
        case VD_AGENT_CLIPBOARD_NONE:
        default: return VDAgentClipboardTypeNone;
    }
}

static guint32 convert_clipboard_select_to_raw(vdagent_clipboard_select_t sel)
{
    switch (sel) {
        case VDAgentClipboardSelectPrimary: return VD_AGENT_CLIPBOARD_SELECTION_PRIMARY;
        case VDAgentClipboardSelectSecondary: return VD_AGENT_CLIPBOARD_SELECTION_SECONDARY;
        case VDAgentClipboardSelectClipboard:
        default: return VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
    }
}

static vdagent_clipboard_select_t convert_raw_to_clipboard_select(guint32 sel)
{
    switch (sel) {
        case VD_AGENT_CLIPBOARD_SELECTION_PRIMARY: return VDAgentClipboardSelectPrimary;
        case VD_AGENT_CLIPBOARD_SELECTION_SECONDARY: return VDAgentClipboardSelectSecondary;
        case VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD:
        default: return VDAgentClipboardSelectClipboard;
    }
}

static void daemon_clipboard_request(VDAgent *agent,
                                     guint sel_id, guint type)
{
    if (agent->cb && agent->cb->clipboard_request) {
        if (!agent->cb->clipboard_request(agent, agent->ctx,
                                          convert_raw_to_clipboard_select(sel_id),
                                          convert_raw_to_clipboard_type(type))) {
            udscs_write(agent->conn, VDAGENTD_CLIPBOARD_DATA, sel_id,
                        VD_AGENT_CLIPBOARD_NONE, NULL, 0);
        }
    } else {
        syslog(LOG_WARNING, "%s: no callback installed", __func__);
    }
}

static void daemon_clipboard_grab(VDAgent *agent,
                                  guint sel_id, guint32 *types, guint n_types)
{
    if (agent->cb && agent->cb->clipboard_grab) {
        vdagent_clipboard_type_t *_types = calloc(n_types, sizeof(*_types));
        if (_types) {
            for (int i = 0; i < n_types; i++) {
                _types[i] = convert_raw_to_clipboard_type(types[i]);
            }
            agent->cb->clipboard_grab(agent, agent->ctx,
                                      convert_raw_to_clipboard_select(sel_id),
                                      _types, n_types);
            free(_types);
        }
    } else {
        syslog(LOG_WARNING, "%s: no callback installed", __func__);
    }
}

static void daemon_clipboard_data(VDAgent *agent,
                                  guint sel_id, guint32 type,
                                  guchar *data, guint size)
{
    if (agent->cb && agent->cb->clipboard_data) {
        agent->cb->clipboard_data(agent, agent->ctx,
                                  convert_raw_to_clipboard_select(sel_id),
                                  convert_raw_to_clipboard_type(type),
                                  data, size);
    } else {
        syslog(LOG_WARNING, "%s: no callback installed", __func__);
    }
}

static void daemon_clipboard_release(VDAgent *agent, guint sel_id)
{
    if (agent->cb && agent->cb->clipboard_release) {
        agent->cb->clipboard_release(agent, agent->ctx,
                                     convert_raw_to_clipboard_select(sel_id));
    } else {
        syslog(LOG_WARNING, "%s: no callback installed", __func__);
    }
}

static void daemon_read_complete(UdscsConnection *conn,
    struct udscs_message_header *header, uint8_t *data)
{
    VDAgent *agent = g_object_get_data(G_OBJECT(conn), "agent");

    switch (header->type) {
    case VDAGENTD_CLIPBOARD_REQUEST:
        daemon_clipboard_request(agent, header->arg1, header->arg2);
        break;
    case VDAGENTD_CLIPBOARD_GRAB:
        daemon_clipboard_grab(agent, header->arg1, (guint32 *)data, header->size / sizeof(guint32));
        break;
    case VDAGENTD_CLIPBOARD_DATA:
        daemon_clipboard_data(agent, header->arg1, header->arg2, data, header->size);
        break;
    case VDAGENTD_CLIPBOARD_RELEASE:
        daemon_clipboard_release(agent, header->arg1);
        break;
    case VDAGENTD_VERSION:
        if (strcmp((char *)data, VERSION) != 0) {
            syslog(LOG_INFO, "vdagentd version mismatch: got %s expected %s",
                   data, VERSION);
            vdagent_quit_loop(agent);
            version_mismatch = 1;
        }
        break;
    case VDAGENTD_CLIENT_DISCONNECTED:
        if (agent->cb && agent->cb->client_disconnected) {
            agent->cb->client_disconnected(agent, agent->ctx);
        }
        break;
    default:
        syslog(LOG_ERR, "Unknown message from vdagentd type: %d, ignoring",
               header->type);
    }
}

static void daemon_error_cb(VDAgentConnection *conn, GError *err)
{
    VDAgent *agent = g_object_get_data(G_OBJECT(conn), "agent");

    if (err) {
        syslog(LOG_ERR, "%s", err->message);
        g_error_free(err);
    }
    g_clear_pointer(&conn, vdagent_connection_destroy);
    vdagent_quit_loop(agent);
}

static gboolean clipboard_guest_update_cb(gpointer user_data)
{
    VDAgent *agent = user_data;

    if (agent->cb && agent->cb->clipboard_guest_update) {
        agent->cb->clipboard_guest_update(agent, agent->ctx);
    }
    return TRUE;
}

static gboolean vdagent_init_async_cb(gpointer user_data)
{
    VDAgent *agent = user_data;
    GError *err = NULL;

    agent->conn = udscs_connect(vdagentd_socket,
                                daemon_read_complete,
                                daemon_error_cb,
                                debug,
                                &err);
    if (agent->conn == NULL) {
        if (agent->udscs_num_retry == MAX_RETRY_CONNECT_SYSTEM_AGENT) {
            syslog(LOG_WARNING,
                   "Failed to connect to spice-vdagentd at %s (tried %d times)",
                   vdagentd_socket, agent->udscs_num_retry);
            g_error_free(err);
            goto err_init;
        }
        if (agent->udscs_num_retry == 0) {
            /* Log only when it fails and at the end */
            syslog(LOG_DEBUG,
                   "Failed to connect with spice-vdagentd due '%s'. Trying again in 1s",
                   err->message);
        }
        g_error_free(err);
        agent->udscs_num_retry++;
        g_timeout_add_seconds(1, vdagent_init_async_cb, agent);
        return G_SOURCE_REMOVE;
    }
    if (agent->udscs_num_retry != 0) {
        syslog(LOG_DEBUG,
               "Connected with spice-vdagentd after %d attempts",
               agent->udscs_num_retry);
    }
    agent->udscs_num_retry = 0;
    g_object_set_data(G_OBJECT(agent->conn), "agent", agent);

    g_timeout_add(100, clipboard_guest_update_cb, agent);

    return G_SOURCE_REMOVE;

err_init:
    vdagent_quit_loop(agent);
    quit = TRUE;
    return G_SOURCE_REMOVE;
}

int vdagent_start(const char *socketPath, const vdagent_cb_t *cb, void *ctx)
{
    VDAgent *agent;

    if (socketPath == NULL)
        vdagentd_socket = g_strdup(VDAGENTD_SOCKET);
    else
        vdagentd_socket = g_strdup(socketPath);

    openlog("spice-vdagent", LOG_PID | LOG_PERROR, LOG_USER);

    syslog(LOG_INFO, "vdagent started");

reconnect:
    if (version_mismatch) {
        syslog(LOG_INFO, "Version mismatch, restarting");
        sleep(1);
    }

    agent = vdagent_new(cb, ctx);

    g_timeout_add(0, vdagent_init_async_cb, agent);

    g_main_loop_run(agent->loop);

    vdagent_destroy(agent);
    agent = NULL;

    /* allow the VDAgentConnection to finalize properly */
    g_main_context_iteration(NULL, FALSE);

    if (!quit)
        goto reconnect;

    g_free(vdagentd_socket);

    return 0;
}

bool vdagent_clipboard_request(VDAgent *agent, vdagent_clipboard_select_t sel, vdagent_clipboard_type_t type)
{
    udscs_write(agent->conn,
                VDAGENTD_CLIPBOARD_REQUEST,
                convert_clipboard_select_to_raw(sel),
                convert_clipboard_type_to_raw(type),
                NULL, 0);
    return true;
}

bool vdagent_clipboard_grab(VDAgent *agent, vdagent_clipboard_select_t sel,
                            const vdagent_clipboard_type_t *types, int n_types)
{
    guint32 *_types = calloc(n_types, sizeof(*_types));

    if (!_types) {
        return false;
    }
    for (int i = 0; i < n_types; i++) {
        _types[i] = convert_clipboard_type_to_raw(types[i]);
    }
    udscs_write(agent->conn,
                VDAGENTD_CLIPBOARD_GRAB,
                convert_clipboard_select_to_raw(sel),
                0,
                (uint8_t *)_types, n_types * sizeof(guint32));
    free(_types);
    return true;
}

bool vdagent_clipboard_data(VDAgent *agent, vdagent_clipboard_select_t sel,
                            vdagent_clipboard_type_t type, const unsigned char *data, unsigned int size)
{
    udscs_write(agent->conn,
                VDAGENTD_CLIPBOARD_DATA,
                convert_clipboard_select_to_raw(sel),
                convert_clipboard_type_to_raw(type),
                data, size);
    return true;
}

bool vdagent_clipboard_release(VDAgent *agent, vdagent_clipboard_select_t sel)
{
    udscs_write(agent->conn,
                VDAGENTD_CLIPBOARD_RELEASE,
                convert_clipboard_select_to_raw(sel),
                0,
                NULL, 0);
    return true;
}

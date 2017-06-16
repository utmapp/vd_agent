/*  vdagentd.c vdagentd (daemon) code

    Copyright 2010-2013 Red Hat, Inc.

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <spice/vd_agent.h>
#include <glib.h>

#ifdef WITH_SYSTEMD_SOCKET_ACTIVATION
#include <systemd/sd-daemon.h>
#endif /* WITH_SYSTEMD_SOCKET_ACTIVATION */

#include "udscs.h"
#include "vdagentd-proto.h"
#include "vdagentd-proto-strings.h"
#include "uinput.h"
#include "xorg-conf.h"
#include "virtio-port.h"
#include "session-info.h"

struct agent_data {
    char *session;
    int width;
    int height;
    struct vdagentd_guest_xorg_resolution *screen_info;
    int screen_count;
};

/* variables */
static const char *pidfilename = "/var/run/spice-vdagentd/spice-vdagentd.pid";
static const char *portdev = DEFAULT_VIRTIO_PORT_PATH;
static const char *vdagentd_socket = VDAGENTD_SOCKET;
static const char *uinput_device = "/dev/uinput";
static int debug = 0;
static int uinput_fake = 0;
static int only_once = 0;
static struct udscs_server *server = NULL;
static struct vdagent_virtio_port *virtio_port = NULL;
static GHashTable *active_xfers = NULL;
static struct session_info *session_info = NULL;
static struct vdagentd_uinput *uinput = NULL;
static VDAgentMonitorsConfig *mon_config = NULL;
static uint32_t *capabilities = NULL;
static int capabilities_size = 0;
static const char *active_session = NULL;
static unsigned int session_count = 0;
static struct udscs_connection *active_session_conn = NULL;
static int agent_owns_clipboard[256] = { 0, };
static int quit = 0;
static int retval = 0;
static int client_connected = 0;
static int max_clipboard = -1;

/* utility functions */
static void virtio_msg_uint32_to_le(uint8_t *_msg, uint32_t size, uint32_t offset)
{
    uint32_t i, *msg = (uint32_t *)(_msg + offset);

    /* offset - size % 4 should be 0 - extra bytes are ignored */
    for (i = 0; i < (size - offset) / 4; i++)
        msg[i] = GUINT32_TO_LE(msg[i]);
}

static void virtio_msg_uint32_from_le(uint8_t *_msg, uint32_t size, uint32_t offset)
{
    uint32_t i, *msg = (uint32_t *)(_msg + offset);

    /* offset - size % 4 should be 0 - extra bytes are ignored */
    for (i = 0; i < (size - offset) / 4; i++)
        msg[i] = GUINT32_FROM_LE(msg[i]);
}

static void virtio_msg_uint16_from_le(uint8_t *_msg, uint32_t size, uint32_t offset)
{
    uint32_t i;
    uint16_t *msg = (uint16_t *)(_msg + offset);

    /* offset - size % 2 should be 0 - extra bytes are ignored */
    for (i = 0; i < (size - offset) / 2; i++)
        msg[i] = GUINT16_FROM_LE(msg[i]);
}

/* vdagentd <-> spice-client communication handling */
static void send_capabilities(struct vdagent_virtio_port *vport,
    uint32_t request)
{
    VDAgentAnnounceCapabilities *caps;
    uint32_t size;

    size = sizeof(*caps) + VD_AGENT_CAPS_BYTES;
    caps = calloc(1, size);
    if (!caps) {
        syslog(LOG_ERR, "out of memory allocating capabilities array (write)");
        return;
    }

    caps->request = request;
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_MOUSE_STATE);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_MONITORS_CONFIG);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_REPLY);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_SELECTION);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_SPARSE_MONITORS_CONFIG);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_GUEST_LINEEND_LF);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_MAX_CLIPBOARD);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_AUDIO_VOLUME_SYNC);
    virtio_msg_uint32_to_le((uint8_t *)caps, size, 0);

    vdagent_virtio_port_write(vport, VDP_CLIENT_PORT,
                              VD_AGENT_ANNOUNCE_CAPABILITIES, 0,
                              (uint8_t *)caps, size);
    free(caps);
}

static void do_client_disconnect(void)
{
    if (client_connected) {
        udscs_server_write_all(server, VDAGENTD_CLIENT_DISCONNECTED, 0, 0,
                               NULL, 0);
        client_connected = 0;
    }
}

void do_client_mouse(struct vdagentd_uinput **uinputp, VDAgentMouseState *mouse)
{
    vdagentd_uinput_do_mouse(uinputp, mouse);
    if (!*uinputp) {
        /* Try to re-open the tablet */
        struct agent_data *agent_data =
            udscs_get_user_data(active_session_conn);
        if (agent_data)
            *uinputp = vdagentd_uinput_create(uinput_device,
                                              agent_data->width,
                                              agent_data->height,
                                              agent_data->screen_info,
                                              agent_data->screen_count,
                                              debug > 1,
                                              uinput_fake);
        if (!*uinputp) {
            syslog(LOG_CRIT, "Fatal uinput error");
            retval = 1;
            quit = 1;
        }
    }
}

static void do_client_monitors(struct vdagent_virtio_port *vport, int port_nr,
    VDAgentMessage *message_header, VDAgentMonitorsConfig *new_monitors)
{
    VDAgentReply reply;
    uint32_t size;

    /* Store monitor config to send to agents when they connect */
    size = sizeof(VDAgentMonitorsConfig) +
           new_monitors->num_of_monitors * sizeof(VDAgentMonConfig);
    if (message_header->size != size) {
        syslog(LOG_ERR, "invalid message size for VDAgentMonitorsConfig");
        return;
    }

    vdagentd_write_xorg_conf(new_monitors);

    if (!mon_config ||
            mon_config->num_of_monitors != new_monitors->num_of_monitors) {
        free(mon_config);
        mon_config = malloc(size);
        if (!mon_config) {
            syslog(LOG_ERR, "out of memory allocating monitors config");
            return;
        }
    }
    memcpy(mon_config, new_monitors, size);

    /* Send monitor config to currently active agent */
    if (active_session_conn)
        udscs_write(active_session_conn, VDAGENTD_MONITORS_CONFIG, 0, 0,
                    (uint8_t *)mon_config, size);

    /* Acknowledge reception of monitors config to spice server / client */
    reply.type  = GUINT32_TO_LE(VD_AGENT_MONITORS_CONFIG);
    reply.error = GUINT32_TO_LE(VD_AGENT_SUCCESS);
    vdagent_virtio_port_write(vport, port_nr, VD_AGENT_REPLY, 0,
                              (uint8_t *)&reply, sizeof(reply));
}

static void do_client_volume_sync(struct vdagent_virtio_port *vport, int port_nr,
    VDAgentMessage *message_header,
    VDAgentAudioVolumeSync *avs)
{
    if (active_session_conn == NULL) {
        syslog(LOG_DEBUG, "No active session - Can't volume-sync");
        return;
    }

    udscs_write(active_session_conn, VDAGENTD_AUDIO_VOLUME_SYNC, 0, 0,
                (uint8_t *)avs, message_header->size);
}

static void do_client_capabilities(struct vdagent_virtio_port *vport,
    VDAgentMessage *message_header,
    VDAgentAnnounceCapabilities *caps)
{
    int new_size = VD_AGENT_CAPS_SIZE_FROM_MSG_SIZE(message_header->size);

    if (capabilities_size != new_size) {
        capabilities_size = new_size;
        free(capabilities);
        capabilities = malloc(capabilities_size * sizeof(uint32_t));
        if (!capabilities) {
            syslog(LOG_ERR, "oom allocating capabilities array (read)");
            capabilities_size = 0;
            return;
        }
    }
    memcpy(capabilities, caps->caps, capabilities_size * sizeof(uint32_t));
    if (caps->request) {
        /* Report the previous client has disconnected. */
        do_client_disconnect();
        if (debug)
            syslog(LOG_DEBUG, "New client connected");
        client_connected = 1;
        send_capabilities(vport, 0);
    }
}

static void do_client_clipboard(struct vdagent_virtio_port *vport,
    VDAgentMessage *message_header, uint8_t *data)
{
    uint32_t msg_type = 0, data_type = 0, size = message_header->size;
    uint8_t selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;

    if (!active_session_conn) {
        syslog(LOG_WARNING,
               "Could not find an agent connection belonging to the "
               "active session, ignoring client clipboard request");
        return;
    }

    if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
      selection = data[0];
      data += 4;
      size -= 4;
    }

    switch (message_header->type) {
    case VD_AGENT_CLIPBOARD_GRAB:
        msg_type = VDAGENTD_CLIPBOARD_GRAB;
        agent_owns_clipboard[selection] = 0;
        break;
    case VD_AGENT_CLIPBOARD_REQUEST: {
        VDAgentClipboardRequest *req = (VDAgentClipboardRequest *)data;
        msg_type = VDAGENTD_CLIPBOARD_REQUEST;
        data_type = req->type;
        data = NULL;
        size = 0;
        break;
    }
    case VD_AGENT_CLIPBOARD: {
        VDAgentClipboard *clipboard = (VDAgentClipboard *)data;
        msg_type = VDAGENTD_CLIPBOARD_DATA;
        data_type = clipboard->type;
        size = size - sizeof(VDAgentClipboard);
        data = clipboard->data;
        break;
    }
    case VD_AGENT_CLIPBOARD_RELEASE:
        msg_type = VDAGENTD_CLIPBOARD_RELEASE;
        data = NULL;
        size = 0;
        break;
    }

    udscs_write(active_session_conn, msg_type, selection, data_type,
                data, size);
}

/* Send file-xfer status to the client. In the case status is an error,
 * optional data for the client and log message may be specified. */
static void send_file_xfer_status(struct vdagent_virtio_port *vport,
                                  const char *msg, uint32_t id, uint32_t xfer_status,
                                  const uint8_t *data, uint32_t data_size)
{
    VDAgentFileXferStatusMessage *status;

    /* Replace new detailed errors with older generic VD_AGENT_FILE_XFER_STATUS_ERROR
     * when not supported by client */
    if (xfer_status > VD_AGENT_FILE_XFER_STATUS_SUCCESS &&
        !VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                 VD_AGENT_CAP_FILE_XFER_DETAILED_ERRORS)) {
        xfer_status = VD_AGENT_FILE_XFER_STATUS_ERROR;
        data_size = 0;
    }

    status = malloc(sizeof(*status) + data_size);
    status->id = GUINT32_TO_LE(id);
    status->result = GUINT32_TO_LE(xfer_status);
    if (data)
        memcpy(status->data, data, data_size);

    if (msg)
        syslog(LOG_WARNING, msg, id);

    if (vport)
        vdagent_virtio_port_write(vport, VDP_CLIENT_PORT,
                                  VD_AGENT_FILE_XFER_STATUS, 0,
                                  (uint8_t *)status, sizeof(*status) + data_size);

    free(status);
}

static void do_client_file_xfer(struct vdagent_virtio_port *vport,
                                VDAgentMessage *message_header,
                                uint8_t *data)
{
    uint32_t msg_type, id;
    struct udscs_connection *conn;

    switch (message_header->type) {
    case VD_AGENT_FILE_XFER_START: {
        VDAgentFileXferStartMessage *s = (VDAgentFileXferStartMessage *)data;
        if (!active_session_conn) {
            send_file_xfer_status(vport,
               "Could not find an agent connection belonging to the "
               "active session, cancelling client file-xfer request %u",
               s->id, VD_AGENT_FILE_XFER_STATUS_VDAGENT_NOT_CONNECTED, NULL, 0);
            return;
        } else if (session_info_session_is_locked(session_info)) {
            syslog(LOG_DEBUG, "Session is locked, skipping file-xfer-start");
            send_file_xfer_status(vport,
               "User's session is locked and cannot start file transfer. "
               "Cancelling client file-xfer request %u",
               s->id, VD_AGENT_FILE_XFER_STATUS_SESSION_LOCKED, NULL, 0);
            return;
        }
        udscs_write(active_session_conn, VDAGENTD_FILE_XFER_START, 0, 0,
                    data, message_header->size);
        return;
    }
    case VD_AGENT_FILE_XFER_STATUS: {
        VDAgentFileXferStatusMessage *s = (VDAgentFileXferStatusMessage *)data;
        msg_type = VDAGENTD_FILE_XFER_STATUS;
        id = s->id;
        break;
    }
    case VD_AGENT_FILE_XFER_DATA: {
        VDAgentFileXferDataMessage *d = (VDAgentFileXferDataMessage *)data;
        msg_type = VDAGENTD_FILE_XFER_DATA;
        id = d->id;
        break;
    }
    default:
        g_return_if_reached(); /* quiet uninitialized variable warning */
    }

    conn = g_hash_table_lookup(active_xfers, GUINT_TO_POINTER(id));
    if (!conn) {
        if (debug)
            syslog(LOG_DEBUG, "Could not find file-xfer %u (cancelled?)", id);
        return;
    }
    udscs_write(conn, msg_type, 0, 0, data, message_header->size);
}

static gsize vdagent_message_min_size[] =
{
    -1, /* Does not exist */
    sizeof(VDAgentMouseState), /* VD_AGENT_MOUSE_STATE */
    sizeof(VDAgentMonitorsConfig), /* VD_AGENT_MONITORS_CONFIG */
    sizeof(VDAgentReply), /* VD_AGENT_REPLY */
    sizeof(VDAgentClipboard), /* VD_AGENT_CLIPBOARD */
    sizeof(VDAgentDisplayConfig), /* VD_AGENT_DISPLAY_CONFIG */
    sizeof(VDAgentAnnounceCapabilities), /* VD_AGENT_ANNOUNCE_CAPABILITIES */
    sizeof(VDAgentClipboardGrab), /* VD_AGENT_CLIPBOARD_GRAB */
    sizeof(VDAgentClipboardRequest), /* VD_AGENT_CLIPBOARD_REQUEST */
    sizeof(VDAgentClipboardRelease), /* VD_AGENT_CLIPBOARD_RELEASE */
    sizeof(VDAgentFileXferStartMessage), /* VD_AGENT_FILE_XFER_START */
    sizeof(VDAgentFileXferStatusMessage), /* VD_AGENT_FILE_XFER_STATUS */
    sizeof(VDAgentFileXferDataMessage), /* VD_AGENT_FILE_XFER_DATA */
    0, /* VD_AGENT_CLIENT_DISCONNECTED */
    sizeof(VDAgentMaxClipboard), /* VD_AGENT_MAX_CLIPBOARD */
    sizeof(VDAgentAudioVolumeSync), /* VD_AGENT_AUDIO_VOLUME_SYNC */
};

static void vdagent_message_clipboard_from_le(VDAgentMessage *message_header,
        uint8_t *data)
{
    gsize min_size = vdagent_message_min_size[message_header->type];
    uint32_t *data_type = (uint32_t *) data;

    if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
        min_size += 4;
        data_type++;
    }

    switch (message_header->type) {
    case VD_AGENT_CLIPBOARD_REQUEST:
    case VD_AGENT_CLIPBOARD:
        *data_type = GUINT32_FROM_LE(*data_type);
        break;
    case VD_AGENT_CLIPBOARD_GRAB:
        virtio_msg_uint32_from_le(data, message_header->size, min_size);
        break;
    case VD_AGENT_CLIPBOARD_RELEASE:
        break;
    default:
        g_warn_if_reached();
    }
}

static void vdagent_message_file_xfer_from_le(VDAgentMessage *message_header,
        uint8_t *data)
{
    uint32_t *id = (uint32_t *)data;
    *id = GUINT32_FROM_LE(*id);
    id++; /* status */

    switch (message_header->type) {
    case VD_AGENT_FILE_XFER_DATA: {
       VDAgentFileXferDataMessage *msg = (VDAgentFileXferDataMessage *)data;
       msg->size = GUINT64_FROM_LE(msg->size);
       break;
    }
    case VD_AGENT_FILE_XFER_STATUS:
       *id = GUINT32_FROM_LE(*id); /* status */
       break;
    }
}

static gboolean vdagent_message_check_size(const VDAgentMessage *message_header)
{
    uint32_t min_size = 0;

    if (message_header->protocol != VD_AGENT_PROTOCOL) {
        syslog(LOG_ERR, "message with wrong protocol version ignoring");
        return FALSE;
    }

    if (!message_header->type ||
        message_header->type >= G_N_ELEMENTS(vdagent_message_min_size)) {
        syslog(LOG_WARNING, "unknown message type %d, ignoring",
               message_header->type);
        return FALSE;
    }

    min_size = vdagent_message_min_size[message_header->type];
    if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
        switch (message_header->type) {
        case VD_AGENT_CLIPBOARD_GRAB:
        case VD_AGENT_CLIPBOARD_REQUEST:
        case VD_AGENT_CLIPBOARD:
        case VD_AGENT_CLIPBOARD_RELEASE:
          min_size += 4;
        }
    }

    switch (message_header->type) {
    case VD_AGENT_MONITORS_CONFIG:
    case VD_AGENT_FILE_XFER_START:
    case VD_AGENT_FILE_XFER_DATA:
    case VD_AGENT_CLIPBOARD:
    case VD_AGENT_CLIPBOARD_GRAB:
    case VD_AGENT_AUDIO_VOLUME_SYNC:
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
        if (message_header->size < min_size) {
            syslog(LOG_ERR, "read: invalid message size: %u for message type: %u",
                   message_header->size, message_header->type);
            return FALSE;
        }
        break;
    case VD_AGENT_MOUSE_STATE:
    case VD_AGENT_FILE_XFER_STATUS:
    case VD_AGENT_DISPLAY_CONFIG:
    case VD_AGENT_REPLY:
    case VD_AGENT_CLIPBOARD_REQUEST:
    case VD_AGENT_CLIPBOARD_RELEASE:
    case VD_AGENT_MAX_CLIPBOARD:
    case VD_AGENT_CLIENT_DISCONNECTED:
        if (message_header->size != min_size) {
            syslog(LOG_ERR, "read: invalid message size: %u for message type: %u",
                   message_header->size, message_header->type);
            return FALSE;
        }
        break;
    default:
        g_warn_if_reached();
        return FALSE;
    }
    return TRUE;
}

static int virtio_port_read_complete(
        struct vdagent_virtio_port *vport,
        int port_nr,
        VDAgentMessage *message_header,
        uint8_t *data)
{
    if (!vdagent_message_check_size(message_header))
        return 0;

    switch (message_header->type) {
    case VD_AGENT_MOUSE_STATE:
        virtio_msg_uint32_from_le(data, message_header->size, 0);
        do_client_mouse(&uinput, (VDAgentMouseState *)data);
        break;
    case VD_AGENT_MONITORS_CONFIG:
        virtio_msg_uint32_from_le(data, message_header->size, 0);
        do_client_monitors(vport, port_nr, message_header,
                    (VDAgentMonitorsConfig *)data);
        break;
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
        virtio_msg_uint32_from_le(data, message_header->size, 0);
        do_client_capabilities(vport, message_header,
                        (VDAgentAnnounceCapabilities *)data);
        break;
    case VD_AGENT_CLIPBOARD_GRAB:
    case VD_AGENT_CLIPBOARD_REQUEST:
    case VD_AGENT_CLIPBOARD:
    case VD_AGENT_CLIPBOARD_RELEASE:
        vdagent_message_clipboard_from_le(message_header, data);
        do_client_clipboard(vport, message_header, data);
        break;
    case VD_AGENT_FILE_XFER_START:
    case VD_AGENT_FILE_XFER_STATUS:
    case VD_AGENT_FILE_XFER_DATA:
        vdagent_message_file_xfer_from_le(message_header, data);
        do_client_file_xfer(vport, message_header, data);
        break;
    case VD_AGENT_CLIENT_DISCONNECTED:
        vdagent_virtio_port_reset(vport, VDP_CLIENT_PORT);
        do_client_disconnect();
        break;
    case VD_AGENT_MAX_CLIPBOARD: {
        max_clipboard = GUINT32_FROM_LE(((VDAgentMaxClipboard *)data)->max);
        syslog(LOG_DEBUG, "Set max clipboard: %d", max_clipboard);
        break;
    }
    case VD_AGENT_AUDIO_VOLUME_SYNC: {
        VDAgentAudioVolumeSync *vdata = (VDAgentAudioVolumeSync *)data;
        virtio_msg_uint16_from_le((uint8_t *)vdata, message_header->size,
            offsetof(VDAgentAudioVolumeSync, volume));

        do_client_volume_sync(vport, port_nr, message_header, vdata);
        break;
    }
    default:
        g_warn_if_reached();
    }

    return 0;
}

static void virtio_write_clipboard(uint8_t selection, uint32_t msg_type,
    uint32_t data_type, uint8_t *data, uint32_t data_size)
{
    uint32_t size = data_size;

    if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
        size += 4;
    }
    if (data_type != -1) {
        size += 4;
    }

    vdagent_virtio_port_write_start(virtio_port, VDP_CLIENT_PORT, msg_type,
                                    0, size);

    if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
        uint8_t sel[4] = { selection, 0, 0, 0 };
        vdagent_virtio_port_write_append(virtio_port, sel, 4);
    }
    if (data_type != -1) {
        data_type = GUINT32_TO_LE(data_type);
        vdagent_virtio_port_write_append(virtio_port, (uint8_t*)&data_type, 4);
    }

    if (msg_type == VD_AGENT_CLIPBOARD_GRAB)
        virtio_msg_uint32_to_le(data, data_size, 0);
    vdagent_virtio_port_write_append(virtio_port, data, data_size);
}

/* vdagentd <-> vdagent communication handling */
static int do_agent_clipboard(struct udscs_connection *conn,
        struct udscs_message_header *header, uint8_t *data)
{
    uint8_t selection = header->arg1;
    uint32_t msg_type = 0, data_type = -1, size = header->size;

    if (!VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                 VD_AGENT_CAP_CLIPBOARD_BY_DEMAND))
        goto error;

    /* Check that this agent is from the currently active session */
    if (conn != active_session_conn) {
        if (debug)
            syslog(LOG_DEBUG, "%p clipboard req from agent which is not in "
                              "the active session?", conn);
        goto error;
    }

    if (!virtio_port) {
        syslog(LOG_ERR, "Clipboard req from agent but no client connection");
        goto error;
    }

    if (!VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                 VD_AGENT_CAP_CLIPBOARD_SELECTION) &&
            selection != VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD) {
        goto error;
    }

    switch (header->type) {
    case VDAGENTD_CLIPBOARD_GRAB:
        msg_type = VD_AGENT_CLIPBOARD_GRAB;
        agent_owns_clipboard[selection] = 1;
        break;
    case VDAGENTD_CLIPBOARD_REQUEST:
        msg_type = VD_AGENT_CLIPBOARD_REQUEST;
        data_type = header->arg2;
        size = 0;
        break;
    case VDAGENTD_CLIPBOARD_DATA:
        msg_type = VD_AGENT_CLIPBOARD;
        data_type = header->arg2;
        if (max_clipboard != -1 && size > max_clipboard) {
            syslog(LOG_WARNING, "clipboard is too large (%d > %d), discarding",
                   size, max_clipboard);
            virtio_write_clipboard(selection, msg_type, data_type, NULL, 0);
            return 0;
        }
        break;
    case VDAGENTD_CLIPBOARD_RELEASE:
        msg_type = VD_AGENT_CLIPBOARD_RELEASE;
        size = 0;
        agent_owns_clipboard[selection] = 0;
        break;
    default:
        syslog(LOG_WARNING, "unexpected clipboard message type");
        goto error;
    }

    if (size != header->size) {
        syslog(LOG_ERR,
               "unexpected extra data in clipboard msg, disconnecting agent");
        return -1;
    }

    virtio_write_clipboard(selection, msg_type, data_type, data, header->size);

    return 0;

error:
    if (header->type == VDAGENTD_CLIPBOARD_REQUEST) {
        /* Let the agent know no answer is coming */
        udscs_write(conn, VDAGENTD_CLIPBOARD_DATA,
                    selection, VD_AGENT_CLIPBOARD_NONE, NULL, 0);
    }
    return 0;
}

/* When we open the vdagent virtio channel, the server automatically goes into
   client mouse mode, so we can only have the channel open when we know the
   active session resolution. This function checks that we have an agent in the
   active session, and that it has told us its resolution. If these conditions
   are met it sets the uinput tablet device's resolution and opens the virtio
   channel (if it is not already open). If these conditions are not met, it
   closes both. */
static void check_xorg_resolution(void)
{
    struct agent_data *agent_data = udscs_get_user_data(active_session_conn);

    if (agent_data && agent_data->screen_info) {
        if (!uinput)
            uinput = vdagentd_uinput_create(uinput_device,
                                            agent_data->width,
                                            agent_data->height,
                                            agent_data->screen_info,
                                            agent_data->screen_count,
                                            debug > 1,
                                            uinput_fake);
        else
            vdagentd_uinput_update_size(&uinput,
                                        agent_data->width,
                                        agent_data->height,
                                        agent_data->screen_info,
                                        agent_data->screen_count);
        if (!uinput) {
            syslog(LOG_CRIT, "Fatal uinput error");
            retval = 1;
            quit = 1;
            return;
        }

        if (!virtio_port) {
            syslog(LOG_INFO, "opening vdagent virtio channel");
            virtio_port = vdagent_virtio_port_create(portdev,
                                                     virtio_port_read_complete,
                                                     NULL);
            if (!virtio_port) {
                syslog(LOG_CRIT, "Fatal error opening vdagent virtio channel");
                retval = 1;
                quit = 1;
                return;
            }
            send_capabilities(virtio_port, 1);
        }
    } else {
#ifndef WITH_STATIC_UINPUT
        vdagentd_uinput_destroy(&uinput);
#endif
        if (virtio_port) {
            vdagent_virtio_port_flush(&virtio_port);
            vdagent_virtio_port_destroy(&virtio_port);
            syslog(LOG_INFO, "closed vdagent virtio channel");
        }
    }
}

static int connection_matches_active_session(struct udscs_connection **connp,
    void *priv)
{
    struct udscs_connection **conn_ret = (struct udscs_connection **)priv;
    struct agent_data *agent_data = udscs_get_user_data(*connp);

    /* Check if this connection matches the currently active session */
    if (!agent_data->session || !active_session)
        return 0;
    if (strcmp(agent_data->session, active_session))
        return 0;

    *conn_ret = *connp;
    return 1;
}

static void release_clipboards(void)
{
    uint8_t sel;

    for (sel = 0; sel < VD_AGENT_CLIPBOARD_SELECTION_SECONDARY; ++sel) {
        if (agent_owns_clipboard[sel] && virtio_port) {
            vdagent_virtio_port_write(virtio_port, VDP_CLIENT_PORT,
                                      VD_AGENT_CLIPBOARD_RELEASE, 0, &sel, 1);
        }
        agent_owns_clipboard[sel] = 0;
    }
}

static void update_active_session_connection(struct udscs_connection *new_conn)
{
    if (session_info) {
        new_conn = NULL;
        if (!active_session)
            active_session = session_info_get_active_session(session_info);
        session_count = udscs_server_for_all_clients(server,
                                         connection_matches_active_session,
                                         (void*)&new_conn);
    } else {
        if (new_conn)
            session_count++;
        else
            session_count--;
    }

    if (new_conn && session_count != 1) {
        syslog(LOG_ERR, "multiple agents in one session, "
               "disabling agent to avoid potential information leak");
        new_conn = NULL;
    }

    if (new_conn == active_session_conn)
        return;

    active_session_conn = new_conn;
    if (debug)
        syslog(LOG_DEBUG, "%p is now the active session", new_conn);

    if (active_session_conn &&
        session_info != NULL &&
        !session_info_is_user(session_info)) {
        if (debug)
            syslog(LOG_DEBUG, "New session agent does not belong to user: "
                   "disabling file-xfer");
        udscs_write(active_session_conn, VDAGENTD_FILE_XFER_DISABLE, 0, 0,
                    NULL, 0);
    }

    if (active_session_conn && mon_config)
        udscs_write(active_session_conn, VDAGENTD_MONITORS_CONFIG, 0, 0,
                    (uint8_t *)mon_config, sizeof(VDAgentMonitorsConfig) +
                    mon_config->num_of_monitors * sizeof(VDAgentMonConfig));

    release_clipboards();

    check_xorg_resolution();
}

static gboolean remove_active_xfers(gpointer key, gpointer value, gpointer conn)
{
    if (value == conn) {
        send_file_xfer_status(virtio_port,
                              "Agent disc; cancelling file-xfer %u",
                              GPOINTER_TO_UINT(key),
                              VD_AGENT_FILE_XFER_STATUS_CANCELLED, NULL, 0);
        return 1;
    } else
        return 0;
}

static void agent_connect(struct udscs_connection *conn)
{
    struct agent_data *agent_data;

    agent_data = calloc(1, sizeof(*agent_data));
    if (!agent_data) {
        syslog(LOG_ERR, "Out of memory allocating agent data, disconnecting");
        udscs_destroy_connection(&conn);
        return;
    }

    if (session_info) {
        uint32_t pid = udscs_get_peer_cred(conn).pid;
        agent_data->session = session_info_session_for_pid(session_info, pid);
    }

    udscs_set_user_data(conn, (void *)agent_data);
    udscs_write(conn, VDAGENTD_VERSION, 0, 0,
                (uint8_t *)VERSION, strlen(VERSION) + 1);
    update_active_session_connection(conn);
}

static void agent_disconnect(struct udscs_connection *conn)
{
    struct agent_data *agent_data = udscs_get_user_data(conn);

    g_hash_table_foreach_remove(active_xfers, remove_active_xfers, conn);

    free(agent_data->session);
    agent_data->session = NULL;
    update_active_session_connection(NULL);

    free(agent_data->screen_info);
    free(agent_data);
}

static void agent_read_complete(struct udscs_connection **connp,
    struct udscs_message_header *header, uint8_t *data)
{
    struct agent_data *agent_data = udscs_get_user_data(*connp);

    switch (header->type) {
    case VDAGENTD_GUEST_XORG_RESOLUTION: {
        struct vdagentd_guest_xorg_resolution *res;
        int n = header->size / sizeof(*res);

        /* Detect older version session agent, but don't disconnect, as
           that stops it from getting the VDAGENTD_VERSION message, and then
           it will never re-exec the new version... */
        if (header->arg1 == 0 && header->arg2 == 0) {
            syslog(LOG_INFO, "got old session agent xorg resolution message, "
                             "ignoring");
            return;
        }

        if (header->size != n * sizeof(*res)) {
            syslog(LOG_ERR, "guest xorg resolution message has wrong size, "
                            "disconnecting agent");
            udscs_destroy_connection(connp);
            return;
        }

        free(agent_data->screen_info);
        res = malloc(n * sizeof(*res));
        if (!res) {
            syslog(LOG_ERR, "out of memory allocating screen info");
            n = 0;
        }
        memcpy(res, data, n * sizeof(*res));
        agent_data->width  = header->arg1;
        agent_data->height = header->arg2;
        agent_data->screen_info  = res;
        agent_data->screen_count = n;

        check_xorg_resolution();
        break;
    }
    case VDAGENTD_CLIPBOARD_GRAB:
    case VDAGENTD_CLIPBOARD_REQUEST:
    case VDAGENTD_CLIPBOARD_DATA:
    case VDAGENTD_CLIPBOARD_RELEASE:
        if (do_agent_clipboard(*connp, header, data)) {
            udscs_destroy_connection(connp);
            return;
        }
        break;
    case VDAGENTD_FILE_XFER_STATUS:{
        /* header->arg1 = file xfer task id, header->arg2 = file xfer status */
        switch (header->arg2) {
            case VD_AGENT_FILE_XFER_STATUS_NOT_ENOUGH_SPACE: {
                uint64_t free_space = GUINT64_TO_LE(*((uint64_t*)data));
                send_file_xfer_status(virtio_port, "Not enough free space. Cancelling file-xfer %u",
                                      header->arg1, header->arg2,
                                      (uint8_t*)&free_space, sizeof(uint64_t));
                break;
            }
            case VD_AGENT_FILE_XFER_STATUS_DISABLED:
                send_file_xfer_status(virtio_port, "File-xfer is disabled, cancelling",
                                      header->arg1, header->arg2, NULL, 0);
                break;
            default:
                send_file_xfer_status(virtio_port, NULL, header->arg1, header->arg2, NULL, 0);
        }

        if (header->arg2 == VD_AGENT_FILE_XFER_STATUS_CAN_SEND_DATA)
            g_hash_table_insert(active_xfers, GUINT_TO_POINTER(GUINT32_TO_LE(header->arg1)),
                                *connp);
        else
            g_hash_table_remove(active_xfers, GUINT_TO_POINTER(GUINT32_TO_LE(header->arg1)));
        break;
    }

    default:
        syslog(LOG_ERR, "unknown message from vdagent: %u, ignoring",
               header->type);
    }
}

/* main */

static void usage(FILE *fp)
{
    fprintf(fp,
            "Usage: spice-vdagentd [OPTIONS]\n\n"
            "Spice guest agent daemon, version %s.\n\n"
            "Options:\n"
            "  -h             print this text\n"
            "  -d             log debug messages (use twice for extra info)\n"
            "  -s <port>      set virtio serial port  [%s]\n"
            "  -S <filename>  set vdagent Unix domain socket [%s]\n"
            "  -u <dev>       set uinput device       [%s]\n"
            "  -f             treat uinput device as fake; no ioctls\n"
            "  -x             don't daemonize\n"
            "  -o             only handle one virtio serial session\n"
#ifdef HAVE_CONSOLE_KIT
            "  -X             disable console kit integration\n"
#endif
#ifdef HAVE_LIBSYSTEMD_LOGIN
            "  -X             disable systemd-logind integration\n"
#endif
            ,VERSION, portdev, vdagentd_socket, uinput_device);
}

static void daemonize(void)
{
    int x;
    FILE *pidfile;

    /* detach from terminal */
    switch (fork()) {
    case 0:
        close(0); close(1); close(2);
        setsid();
        x = open("/dev/null", O_RDWR); x = dup(x); x = dup(x);
        pidfile = fopen(pidfilename, "w");
        if (pidfile) {
            fprintf(pidfile, "%d\n", (int)getpid());
            fclose(pidfile);
        }
        break;
    case -1:
        syslog(LOG_ERR, "fork: %m");
        retval = 1;
    default:
        udscs_destroy_server(server);
        exit(retval);
    }
}

static void main_loop(void)
{
    fd_set readfds, writefds;
    int n, nfds;
    int ck_fd = 0;
    int once = 0;

    while (!quit) {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        nfds = udscs_server_fill_fds(server, &readfds, &writefds);
        n = vdagent_virtio_port_fill_fds(virtio_port, &readfds, &writefds);
        if (n >= nfds)
            nfds = n + 1;

        if (session_info) {
            ck_fd = session_info_get_fd(session_info);
            FD_SET(ck_fd, &readfds);
            if (ck_fd >= nfds)
                nfds = ck_fd + 1;
        }

        n = select(nfds, &readfds, &writefds, NULL, NULL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_CRIT, "Fatal error select: %m");
            retval = 1;
            break;
        }

        udscs_server_handle_fds(server, &readfds, &writefds);

        if (virtio_port) {
            once = 1;
            vdagent_virtio_port_handle_fds(&virtio_port, &readfds, &writefds);
            if (!virtio_port) {
                int old_client_connected = client_connected;
                syslog(LOG_CRIT,
                       "AIIEEE lost spice client connection, reconnecting");
                virtio_port = vdagent_virtio_port_create(portdev,
                                                     virtio_port_read_complete,
                                                     NULL);
                if (!virtio_port) {
                    syslog(LOG_CRIT,
                           "Fatal error opening vdagent virtio channel");
                    retval = 1;
                    break;
                }
                do_client_disconnect();
                client_connected = old_client_connected;
            }
        }
        else if (only_once && once)
        {
            syslog(LOG_INFO, "Exiting after one client session.");
            break;
        }

        if (session_info && FD_ISSET(ck_fd, &readfds)) {
            active_session = session_info_get_active_session(session_info);
            update_active_session_connection(NULL);
        }
    }
}

static void quit_handler(int sig)
{
    quit = 1;
}

int main(int argc, char *argv[])
{
    int c;
    int do_daemonize = 1;
    int want_session_info = 1;
    struct sigaction act;
    gboolean own_socket = TRUE;

    for (;;) {
        if (-1 == (c = getopt(argc, argv, "-dhxXfos:u:S:")))
            break;
        switch (c) {
        case 'd':
            debug++;
            break;
        case 's':
            portdev = optarg;
            break;
        case 'S':
            vdagentd_socket = optarg;
            break;
        case 'u':
            uinput_device = optarg;
            break;
        case 'f':
            uinput_fake = 1;
            break;
        case 'o':
            only_once = 1;
            break;
        case 'x':
            do_daemonize = 0;
            break;
        case 'X':
            want_session_info = 0;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            fputs("\n", stderr);
            usage(stderr);
            return 1;
        }
    }

    memset(&act, 0, sizeof(act));
    act.sa_flags = SA_RESTART;
    act.sa_handler = quit_handler;
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGQUIT, &act, NULL);

    openlog("spice-vdagentd", do_daemonize ? 0 : LOG_PERROR, LOG_USER);

    /* Setup communication with vdagent process(es) */
#ifdef WITH_SYSTEMD_SOCKET_ACTIVATION
    int n_fds;
    /* try to retrieve pre-configured sockets from systemd */
    n_fds = sd_listen_fds(0);
    if (n_fds > 1) {
        syslog(LOG_CRIT, "Received too many sockets from systemd (%i)", n_fds);
        return 1;
    } else if (n_fds == 1) {
        server = udscs_create_server_for_fd(SD_LISTEN_FDS_START, agent_connect,
                                            agent_read_complete,
                                            agent_disconnect,
                                            vdagentd_messages,
                                            VDAGENTD_NO_MESSAGES, debug);
        own_socket = FALSE;
    } else
    /* systemd socket activation not enabled, create our own */
#endif /* WITH_SYSTEMD_SOCKET_ACTIVATION */
    {
        server = udscs_create_server(vdagentd_socket, agent_connect,
                                     agent_read_complete, agent_disconnect,
                                     vdagentd_messages, VDAGENTD_NO_MESSAGES,
                                     debug);
    }

    if (!server) {
        if (errno == EADDRINUSE) {
            syslog(LOG_CRIT, "Fatal the server socket %s exists already. Delete it?",
                   vdagentd_socket);
        } else if (errno == ENOMEM) {
            syslog(LOG_CRIT, "Fatal could not allocate memory for udscs server");
        } else {
            syslog(LOG_CRIT, "Fatal could not create the server socket %s: %m",
                   vdagentd_socket);
        }
        return 1;
    }

    /* no need to set permissions on a socket that was provided by systemd */
    if (own_socket) {
        if (chmod(vdagentd_socket, 0666)) {
            syslog(LOG_CRIT, "Fatal could not change permissions on %s: %m",
                   vdagentd_socket);
            udscs_destroy_server(server);
            return 1;
        }
    }

    if (do_daemonize)
        daemonize();

#ifdef WITH_STATIC_UINPUT
    uinput = vdagentd_uinput_create(uinput_device, 1024, 768, NULL, 0,
                                    debug > 1, uinput_fake);
    if (!uinput) {
        udscs_destroy_server(server);
        return 1;
    }
#endif

    if (want_session_info)
        session_info = session_info_create(debug);
    if (!session_info)
        syslog(LOG_WARNING, "no session info, max 1 session agent allowed");

    active_xfers = g_hash_table_new(g_direct_hash, g_direct_equal);
    main_loop();

    release_clipboards();

    vdagentd_uinput_destroy(&uinput);
    vdagent_virtio_port_flush(&virtio_port);
    vdagent_virtio_port_destroy(&virtio_port);
    session_info_destroy(session_info);
    udscs_destroy_server(server);

    /* leave the socket around if it was provided by systemd */
    if (own_socket) {
        if (unlink(vdagentd_socket) != 0)
            syslog(LOG_ERR, "unlink %s: %s", vdagentd_socket, strerror(errno));
    }
    syslog(LOG_INFO, "vdagentd quitting, returning status %d", retval);

    if (do_daemonize)
        unlink(pidfilename);

    return retval;
}

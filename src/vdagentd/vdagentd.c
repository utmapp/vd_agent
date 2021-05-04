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
#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <sys/stat.h>
#include <spice/vd_agent.h>
#include <glib-unix.h>

#ifdef WITH_SYSTEMD_SOCKET_ACTIVATION
#include <systemd/sd-daemon.h>
#endif /* WITH_SYSTEMD_SOCKET_ACTIVATION */

#include "udscs.h"
#include "vdagentd-proto.h"
#include "uinput.h"
#include "xorg-conf.h"
#include "virtio-port.h"
#include "session-info.h"

#define DEFAULT_UINPUT_DEVICE "/dev/uinput"

// Maximum number of transfers active at any time.
// Avoid DoS from client.
// As each transfer could likely end up taking a file descriptor
// it is good to have a limit less than the number of file descriptors
// in the process (by default 1024). The daemon do not open file
// descriptors for the transfers but the agents do.
#define MAX_ACTIVE_TRANSFERS 128

struct agent_data {
    char *session;
    int width;
    int height;
    struct vdagentd_guest_xorg_resolution *screen_info;
    int screen_count;
};

static const char pidfilename[] = "/run/spice-vdagentd/spice-vdagentd.pid";

/* variables */
static gchar *portdev = NULL;
static gchar *vdagentd_socket = NULL;
static gchar *uinput_device = NULL;
static int debug = 0;
static gboolean uinput_fake = FALSE;
static gboolean only_once = FALSE;
static gboolean do_daemonize = TRUE;
static gboolean want_session_info = TRUE;

static struct udscs_server *server = NULL;
static VirtioPort *virtio_port = NULL;
static GHashTable *active_xfers = NULL;
static struct session_info *session_info = NULL;
static struct vdagentd_uinput *uinput = NULL;
static VDAgentMonitorsConfig *mon_config = NULL;
static uint32_t *capabilities = NULL;
static int capabilities_size = 0;
static const char *active_session = NULL;
static unsigned int session_count = 0;
static UdscsConnection *active_session_conn = NULL;
static bool agent_owns_clipboard[256] = { false, };
static int retval = 0;
static bool client_connected = false;
static int max_clipboard = -1;
static uint32_t clipboard_serial[256];

static GMainLoop *loop;

static void agent_data_destroy(struct agent_data *agent_data)
{
    g_free(agent_data->session);
    g_free(agent_data->screen_info);
    g_free(agent_data);
}

static void vdagentd_quit(gint exit_code)
{
    retval = exit_code;
    g_main_loop_quit(loop);
}

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
static void send_capabilities(VirtioPort *vport,
    uint32_t request)
{
    VDAgentAnnounceCapabilities *caps;
    uint32_t size;

    size = sizeof(*caps) + VD_AGENT_CAPS_BYTES;
    caps = g_malloc0(size);
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
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_GRAPHICS_DEVICE_INFO);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_NO_RELEASE_ON_REGRAB);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_GRAB_SERIAL);
    virtio_msg_uint32_to_le((uint8_t *)caps, size, 0);

    vdagent_virtio_port_write(vport, VDP_CLIENT_PORT,
                              VD_AGENT_ANNOUNCE_CAPABILITIES, 0,
                              (uint8_t *)caps, size);
    g_free(caps);
}

static void do_client_disconnect(void)
{
    g_hash_table_remove_all(active_xfers);
    if (client_connected) {
        udscs_server_write_all(server, VDAGENTD_CLIENT_DISCONNECTED, 0, 0,
                               NULL, 0);
        client_connected = false;
    }
}

void do_client_mouse(struct vdagentd_uinput **uinputp, VDAgentMouseState *mouse)
{
    vdagentd_uinput_do_mouse(uinputp, mouse);
    if (!*uinputp) {
        /* Try to re-open the tablet */
        if (active_session_conn) {
            const struct agent_data *agent_data =
            g_object_get_data(G_OBJECT(active_session_conn), "agent_data");
            *uinputp = vdagentd_uinput_create(uinput_device,
                                              agent_data->width,
                                              agent_data->height,
                                              agent_data->screen_info,
                                              agent_data->screen_count,
                                              debug > 1,
                                              uinput_fake);
        }
        if (!*uinputp) {
            syslog(LOG_CRIT, "Fatal uinput error");
            vdagentd_quit(1);
        }
    }
}

static void do_client_monitors(VirtioPort *vport, int port_nr,
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

    g_free(mon_config);
    mon_config = g_memdup2(new_monitors, size);

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

static void do_client_volume_sync(VirtioPort *vport, int port_nr,
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

static void do_client_capabilities(VirtioPort *vport,
    VDAgentMessage *message_header,
    VDAgentAnnounceCapabilities *caps)
{
    capabilities_size = VD_AGENT_CAPS_SIZE_FROM_MSG_SIZE(message_header->size);
    g_free(capabilities);
    capabilities = g_memdup2(caps->caps, capabilities_size * sizeof(uint32_t));

    if (caps->request) {
        /* Report the previous client has disconnected. */
        do_client_disconnect();
        if (debug)
            syslog(LOG_DEBUG, "New client connected");
        client_connected = true;
        memset(clipboard_serial, 0, sizeof(clipboard_serial));
        send_capabilities(vport, 0);
    }
}

static void do_client_clipboard(VirtioPort *vport,
    VDAgentMessage *message_header, uint8_t *data)
{
    uint32_t msg_type = 0, data_type = 0, size = message_header->size;
    uint8_t selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
    uint32_t serial;

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
        if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                    VD_AGENT_CAP_CLIPBOARD_GRAB_SERIAL)) {
            serial = *(guint32 *)data;
            data += 4;
            size -= 4;

            if (serial == clipboard_serial[selection] - 1) {
                g_debug("client grab wins");
            } else if (serial == clipboard_serial[selection]) {
                clipboard_serial[selection]++;
            } else {
                g_debug("grab discard, serial %u != session serial %u",
                        serial, clipboard_serial[selection]);
                return;
            }
        }

        msg_type = VDAGENTD_CLIPBOARD_GRAB;
        agent_owns_clipboard[selection] = false;
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
static void send_file_xfer_status(VirtioPort *vport,
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

    status = g_malloc(sizeof(*status) + data_size);
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

    g_free(status);
}

static void do_client_file_xfer(VirtioPort *vport,
                                VDAgentMessage *message_header,
                                uint8_t *data)
{
    uint32_t msg_type, id;
    UdscsConnection *conn;

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
        } else if (g_hash_table_size(active_xfers) >= MAX_ACTIVE_TRANSFERS) {
            VDAgentFileXferStatusError error = {
                GUINT32_TO_LE(VD_AGENT_FILE_XFER_STATUS_ERROR_GLIB_IO),
                GUINT32_TO_LE(G_IO_ERROR_TOO_MANY_OPEN_FILES),
            };
            size_t detail_size = sizeof(error);
            if (!VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                         VD_AGENT_CAP_FILE_XFER_DETAILED_ERRORS)) {
                detail_size = 0;
            }
            send_file_xfer_status(vport,
               "Too many transfers ongoing. "
               "Cancelling client file-xfer request %u",
               s->id, VD_AGENT_FILE_XFER_STATUS_ERROR, (void*) &error, detail_size);
            return;
        } else if (g_hash_table_lookup(active_xfers, GUINT_TO_POINTER(s->id)) != NULL) {
            // id is already used -- client is confused
            send_file_xfer_status(vport,
               "File transfer ID is already used. "
               "Cancelling client file-xfer request %u",
               s->id, VD_AGENT_FILE_XFER_STATUS_ERROR, NULL, 0);
            return;
        }
        msg_type = VDAGENTD_FILE_XFER_START;
        id = s->id;
        // associate the id with the active connection
        g_hash_table_insert(active_xfers, GUINT_TO_POINTER(id), active_session_conn);
        break;
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

    // client told that transfer is ended, agents too stop the transfer
    // and release resources
    if (message_header->type == VD_AGENT_FILE_XFER_STATUS) {
        g_hash_table_remove(active_xfers, GUINT_TO_POINTER(id));
    }
}

static void forward_data_to_session_agent(uint32_t type, uint8_t *data, size_t size)
{
    if (active_session_conn == NULL) {
        syslog(LOG_DEBUG, "No active session, can't forward message (type %u)", type);
        return;
    }

    udscs_write(active_session_conn, type, 0, 0, data, size);
}

static const gsize vdagent_message_min_size[] =
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
    sizeof(VDAgentGraphicsDeviceInfo), /* VD_AGENT_GRAPHICS_DEVICE_INFO */
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

    if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                VD_AGENT_CAP_CLIPBOARD_GRAB_SERIAL)
        && message_header->type == VD_AGENT_CLIPBOARD_GRAB) {
        min_size += 4;
    }

    switch (message_header->type) {
    case VD_AGENT_MONITORS_CONFIG:
    case VD_AGENT_FILE_XFER_START:
    case VD_AGENT_FILE_XFER_DATA:
    case VD_AGENT_CLIPBOARD:
    case VD_AGENT_CLIPBOARD_GRAB:
    case VD_AGENT_AUDIO_VOLUME_SYNC:
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
    case VD_AGENT_GRAPHICS_DEVICE_INFO:
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

static VDAgentGraphicsDeviceInfo *device_info = NULL;
static size_t device_info_size = 0;
static void virtio_port_read_complete(
        VirtioPort *vport,
        int port_nr,
        VDAgentMessage *message_header,
        uint8_t *data)
{
    if (!vdagent_message_check_size(message_header))
        return;

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
    case VD_AGENT_GRAPHICS_DEVICE_INFO: {
        // store device info for re-sending when a session agent reconnects
        g_free(device_info);
        device_info = g_memdup2(data, message_header->size);
        device_info_size = message_header->size;
        forward_data_to_session_agent(VDAGENTD_GRAPHICS_DEVICE_INFO, data, message_header->size);
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
}

static void virtio_port_error_cb(VDAgentConnection *conn, GError *err)
{
    bool old_client_connected = client_connected;
    syslog(LOG_CRIT, "AIIEEE lost spice client connection, reconnecting (err: %s)",
                     err ? err->message : "");
    g_clear_error(&err);

    vdagent_connection_destroy(virtio_port);
    virtio_port = vdagent_virtio_port_create(portdev,
                                             virtio_port_read_complete,
                                             virtio_port_error_cb);
    if (virtio_port == NULL) {
        syslog(LOG_CRIT, "Fatal error opening vdagent virtio channel");
        vdagentd_quit(1);
        return;
    }
    do_client_disconnect();
    client_connected = old_client_connected;
}

static void virtio_write_clipboard(uint8_t selection, uint32_t msg_type,
    uint32_t data_type, uint8_t *data, uint32_t data_size)
{
    uint32_t size = data_size;

    if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
        size += 4;
    }
    if (msg_type == VD_AGENT_CLIPBOARD_GRAB
        && VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                   VD_AGENT_CAP_CLIPBOARD_GRAB_SERIAL)) {
        size += sizeof(uint32_t);
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

    if (msg_type == VD_AGENT_CLIPBOARD_GRAB) {
        if (VD_AGENT_HAS_CAPABILITY(capabilities, capabilities_size,
                                    VD_AGENT_CAP_CLIPBOARD_GRAB_SERIAL)) {
            uint32_t serial = GUINT32_TO_LE(clipboard_serial[selection]);
            clipboard_serial[selection]++;
            vdagent_virtio_port_write_append(virtio_port, (uint8_t*)&serial, sizeof(serial));
        }
        virtio_msg_uint32_to_le(data, data_size, 0);
    }
    vdagent_virtio_port_write_append(virtio_port, data, data_size);
}

/* vdagentd <-> vdagent communication handling */
static void do_agent_clipboard(UdscsConnection *conn,
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
        agent_owns_clipboard[selection] = true;
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
            return;
        }
        break;
    case VDAGENTD_CLIPBOARD_RELEASE:
        msg_type = VD_AGENT_CLIPBOARD_RELEASE;
        size = 0;
        agent_owns_clipboard[selection] = false;
        break;
    default:
        syslog(LOG_WARNING, "unexpected clipboard message type");
        goto error;
    }

    if (size != header->size) {
        syslog(LOG_ERR,
               "unexpected extra data in clipboard msg, disconnecting agent");
        udscs_server_destroy_connection(server, conn);
        return;
    }

    virtio_write_clipboard(selection, msg_type, data_type, data, header->size);

    return;

error:
    if (header->type == VDAGENTD_CLIPBOARD_REQUEST) {
        /* Let the agent know no answer is coming */
        udscs_write(conn, VDAGENTD_CLIPBOARD_DATA,
                    selection, VD_AGENT_CLIPBOARD_NONE, NULL, 0);
    }
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
    const struct agent_data *agent_data = NULL;
    if (active_session_conn)
        agent_data = g_object_get_data(G_OBJECT(active_session_conn), "agent_data");

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
            vdagentd_quit(1);
            return;
        }

        if (!virtio_port) {
            syslog(LOG_INFO, "opening vdagent virtio channel");
            virtio_port = vdagent_virtio_port_create(portdev,
                                                     virtio_port_read_complete,
                                                     virtio_port_error_cb);
            if (!virtio_port) {
                syslog(LOG_CRIT, "Fatal error opening vdagent virtio channel");
                vdagentd_quit(1);
                return;
            }
            send_capabilities(virtio_port, 1);
        }
    } else {
#ifndef WITH_STATIC_UINPUT
        vdagentd_uinput_destroy(&uinput);
#endif
        if (virtio_port) {
            if (only_once) {
                syslog(LOG_INFO, "Exiting after one client session.");
                vdagentd_quit(0);
                return;
            }
            vdagent_connection_flush(VDAGENT_CONNECTION(virtio_port));
            g_clear_pointer(&virtio_port, vdagent_connection_destroy);
            syslog(LOG_INFO, "closed vdagent virtio channel");
        }
    }
}

static int connection_matches_active_session(UdscsConnection *conn,
    void *priv)
{
    UdscsConnection **conn_ret = (UdscsConnection **)priv;
    const struct agent_data *agent_data = g_object_get_data(G_OBJECT(conn), "agent_data");

    /* Check if this connection matches the currently active session */
    if (!agent_data->session || !active_session)
        return 0;
    if (strcmp(agent_data->session, active_session))
        return 0;

    *conn_ret = conn;
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
        agent_owns_clipboard[sel] = false;
    }
}

static void update_active_session_connection(UdscsConnection *new_conn)
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

/* Check if this connection matches the passed session */
static int connection_matches_session(UdscsConnection *conn, void *priv)
{
    const char *session = priv;
    const struct agent_data *agent_data = g_object_get_data(G_OBJECT(conn), "agent_data");

    if (!agent_data || !agent_data->session ||
        strcmp(agent_data->session, session) != 0) {
        return 0;
    }

    return 1;
}

/* Check a given process has a given UID */
static bool check_uid_of_pid(pid_t pid, uid_t uid)
{
    char fn[128];
    struct stat st;

    snprintf(fn, sizeof(fn), "/proc/%u/status", (unsigned) pid);
    if (stat(fn, &st) != 0 || st.st_uid != uid) {
        return false;
    }
    return true;
}

static void agent_connect(UdscsConnection *conn)
{
    struct agent_data *agent_data;
    agent_data = g_new0(struct agent_data, 1);
    GError *err = NULL;

    if (session_info) {
        PidUid pid_uid = vdagent_connection_get_peer_pid_uid(VDAGENT_CONNECTION(conn), &err);
        if (err || pid_uid.pid <= 0) {
            static const char msg[] = "Could not get peer PID, disconnecting new client";
            if (err) {
                syslog(LOG_ERR, "%s: %s", msg, err->message);
                g_error_free(err);
            } else {
                syslog(LOG_ERR, "%s", msg);
            }
            agent_data_destroy(agent_data);
            udscs_server_destroy_connection(server, conn);
            return;
        }

        agent_data->session = session_info_session_for_pid(session_info, pid_uid.pid);

        uid_t session_uid = session_info_uid_for_session(session_info, agent_data->session);

        /* Check that the UID of the PID did not change, this should be done after
         * computing the session to avoid race conditions.
         * This can happen as vdagent_connection_get_peer_pid_uid get information
         * from the time of creating the socket, but the process in the meantime
         * have been replaced */
        if (!check_uid_of_pid(pid_uid.pid, pid_uid.uid) ||
            /* Check that the user launching the Agent is the same as session one
             * or root user.
             * This prevents session hijacks from other users. */
            (pid_uid.uid != 0 && pid_uid.uid != session_uid)) {
            syslog(LOG_ERR, "UID mismatch: UID=%u PID=%u suid=%u", pid_uid.uid,
                   pid_uid.pid, session_uid);
            agent_data_destroy(agent_data);
            udscs_server_destroy_connection(server, conn);
            return;
        }

        // Check there are no other connection for this session
        // Note that "conn" is not counted as "agent_data" is still not attached to it
        if (udscs_server_for_all_clients(server, connection_matches_session,
                                         agent_data->session) > 0) {
            syslog(LOG_ERR, "An agent is already connected for this session");
            agent_data_destroy(agent_data);
            udscs_server_destroy_connection(server, conn);
            return;
        }
    }

    g_object_set_data_full(G_OBJECT(conn), "agent_data", agent_data,
                           (GDestroyNotify) agent_data_destroy);
    udscs_write(conn, VDAGENTD_VERSION, 0, 0,
                (uint8_t *)VERSION, strlen(VERSION) + 1);
    update_active_session_connection(conn);

    if (device_info) {
        forward_data_to_session_agent(VDAGENTD_GRAPHICS_DEVICE_INFO,
                                      (uint8_t *) device_info, device_info_size);
    }
}

static void agent_disconnect(VDAgentConnection *conn, GError *err)
{
    g_hash_table_foreach_remove(active_xfers, remove_active_xfers, conn);

    if (err) {
        syslog(LOG_ERR, "%s", err->message);
        g_error_free(err);
    }
    udscs_server_destroy_connection(server, UDSCS_CONNECTION(conn));

    update_active_session_connection(NULL);
}

static void do_agent_xorg_resolution(UdscsConnection             *conn,
                                     struct udscs_message_header *header,
                                     guint8                      *data)
{
    struct agent_data *agent_data = g_object_get_data(G_OBJECT(conn), "agent_data");
    guint res_size = sizeof(struct vdagentd_guest_xorg_resolution);
    guint n = header->size / res_size;

    /* Detect older version session agent, but don't disconnect, as
     * that stops it from getting the VDAGENTD_VERSION message, and then
     * it will never re-exec the new version... */
    if (header->arg1 == 0 && header->arg2 == 0) {
        syslog(LOG_INFO, "got old session agent xorg resolution message, "
                         "ignoring");
        return;
    }

    if (header->size != n * res_size) {
        syslog(LOG_ERR, "guest xorg resolution message has wrong size, "
                        "disconnecting agent");
        udscs_server_destroy_connection(server, conn);
        return;
    }

    g_free(agent_data->screen_info);
    agent_data->screen_info = g_memdup2(data, header->size);
    agent_data->width  = header->arg1;
    agent_data->height = header->arg2;
    agent_data->screen_count = n;

    check_xorg_resolution();
}

static void do_agent_file_xfer_status(UdscsConnection             *conn,
                                      struct udscs_message_header *header,
                                      guint8                      *data)
{
    gpointer task_id = GUINT_TO_POINTER(GUINT32_TO_LE(header->arg1));
    const gchar *log_msg = NULL;
    guint data_size = 0;

    UdscsConnection *task_conn = g_hash_table_lookup(active_xfers, task_id);
    if (task_conn == NULL || task_conn != conn) {
        // Protect against misbehaving agent.
        // Ignore the message, but do not disconnect the agent, to protect against
        // a misbehaving client that tries to disconnect a good agent
        // e.g. by sending a new task and immediately cancelling it.
        return;
    }

    /* header->arg1 = file xfer task id, header->arg2 = file xfer status */
    switch (header->arg2) {
        case VD_AGENT_FILE_XFER_STATUS_NOT_ENOUGH_SPACE:
            *((guint64 *)data) = GUINT64_TO_LE(*((guint64 *)data));
            log_msg = "Not enough free space. Cancelling file-xfer %u";
            data_size = sizeof(guint64);
            break;
        case VD_AGENT_FILE_XFER_STATUS_DISABLED:
            log_msg = "File-xfer is disabled. Cancelling file-xfer %u";
            break;
    }
    send_file_xfer_status(virtio_port, log_msg, header->arg1, header->arg2,
                          data, data_size);

    if (header->arg2 != VD_AGENT_FILE_XFER_STATUS_CAN_SEND_DATA) {
        g_hash_table_remove(active_xfers, task_id);
    }
}

static void agent_read_complete(UdscsConnection *conn,
    struct udscs_message_header *header, uint8_t *data)
{
    switch (header->type) {
    case VDAGENTD_GUEST_XORG_RESOLUTION:
        do_agent_xorg_resolution(conn, header, data);
        break;
    case VDAGENTD_CLIPBOARD_GRAB:
    case VDAGENTD_CLIPBOARD_REQUEST:
    case VDAGENTD_CLIPBOARD_DATA:
    case VDAGENTD_CLIPBOARD_RELEASE:
        do_agent_clipboard(conn, header, data);
        break;
    case VDAGENTD_FILE_XFER_STATUS:
        do_agent_file_xfer_status(conn, header, data);
        break;

    default:
        syslog(LOG_ERR, "unknown message from vdagent: %u, ignoring",
               header->type);
    }
}

static gboolean si_io_channel_cb(GIOChannel  *source,
                                 GIOCondition condition,
                                 gpointer     data)
{
    active_session = session_info_get_active_session(session_info);
    update_active_session_connection(NULL);
    return G_SOURCE_CONTINUE;
}

/* main */

static void daemonize(void)
{
    FILE *pidfile;

    /* detach from terminal */
    switch (fork()) {
    case 0:
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        setsid();
        // coverity[leaked_handle] just opening standard file descriptors
        if (open("/dev/null", O_RDWR) != STDIN_FILENO) {
            exit(1);
        }
        // coverity[leaked_handle] just opening standard file descriptors
        if (dup(STDIN_FILENO) != STDOUT_FILENO) {
            exit(1);
        }
        // coverity[leaked_handle] just opening standard file descriptors
        if (dup(STDOUT_FILENO) != STDERR_FILENO) {
            exit(1);
        }
        pidfile = fopen(pidfilename, "w");
        if (pidfile) {
            fprintf(pidfile, "%d\n", (int)getpid());
            fclose(pidfile);
        }
        break;
    case -1:
        syslog(LOG_ERR, "fork: %m");
        retval = 1;
        // fall through
    default:
        udscs_destroy_server(server);
        exit(retval);
    }
}

static gboolean signal_handler(gpointer user_data)
{
    vdagentd_quit(0);
    return G_SOURCE_REMOVE;
}

static gboolean parse_debug_level_cb(const gchar *option_name,
                                     const gchar *value,
                                     gpointer     data,
                                     GError     **error)
{
    debug++;
    return TRUE;
}

static GOptionEntry cmd_entries[] = {
    { "debug", 'd', G_OPTION_FLAG_NO_ARG,
      G_OPTION_ARG_CALLBACK, parse_debug_level_cb,
      "Log debug messages (use twice for extra info)", NULL },

    { "virtio-serial-port-path", 's', 0,
      G_OPTION_ARG_STRING, &portdev,
      "Set virtio-serial path (" DEFAULT_VIRTIO_PORT_PATH ")", NULL },

    { "vdagentd-socket", 'S', 0,
      G_OPTION_ARG_STRING, &vdagentd_socket,
       "Set spice-vdagentd socket (" VDAGENTD_SOCKET ")", NULL },

    { "uinput-device", 'u', 0,
      G_OPTION_ARG_STRING, &uinput_device,
      "Set uinput device (" DEFAULT_UINPUT_DEVICE ")", NULL },

    { "fake-uinput", 'f', 0,
      G_OPTION_ARG_NONE, &uinput_fake,
      "Treat uinput device as fake; no ioctls", NULL },

    { "foreground", 'x', G_OPTION_FLAG_REVERSE,
      G_OPTION_ARG_NONE, &do_daemonize,
      "Do not daemonize the agent", NULL},

    { "one-session", 'o', 0,
      G_OPTION_ARG_NONE, &only_once,
      "Only handle one virtio serial session", NULL },

#if defined(HAVE_CONSOLE_KIT) || defined (HAVE_LIBSYSTEMD_LOGIN)
    { "disable-session-integration", 'X', G_OPTION_FLAG_REVERSE,
      G_OPTION_ARG_NONE, &want_session_info,
      "Disable console kit and systemd-logind integration", NULL },
#endif

    { NULL }
};

int main(int argc, char *argv[])
{
    GOptionContext *context;
    GError *err = NULL;
    gboolean own_socket = TRUE;
    GIOChannel *si_io_channel = NULL;
    guint si_watch_id = 0;

    context = g_option_context_new(NULL);
    g_option_context_add_main_entries(context, cmd_entries, NULL);
    g_option_context_set_summary(context,
        "Spice guest agent daemon, version " VERSION);
    g_option_context_parse(context, &argc, &argv, &err);
    g_option_context_free(context);

    if (err) {
        g_printerr("Invalid arguments, %s\n", err->message);
        g_error_free(err);
        return 1;
    }

    if (portdev == NULL) {
        portdev = g_strdup(DEFAULT_VIRTIO_PORT_PATH);
    }
    if (vdagentd_socket == NULL) {
        vdagentd_socket = g_strdup(VDAGENTD_SOCKET);
    }
    if (uinput_device == NULL) {
        uinput_device = g_strdup(DEFAULT_UINPUT_DEVICE);
    }

    openlog("spice-vdagentd", do_daemonize ? 0 : LOG_PERROR, LOG_USER);

    /* Setup communication with vdagent process(es) */
    server = udscs_server_new(agent_connect, agent_read_complete,
                              agent_disconnect, debug);
#ifdef WITH_SYSTEMD_SOCKET_ACTIVATION
    int n_fds;
    /* try to retrieve pre-configured sockets from systemd */
    n_fds = sd_listen_fds(0);
    if (n_fds > 1) {
        syslog(LOG_CRIT, "Received too many sockets from systemd (%i)", n_fds);
        return 1;
    } else if (n_fds == 1) {
        udscs_server_listen_to_socket(server, SD_LISTEN_FDS_START, &err);
        own_socket = FALSE;
    } else
    /* systemd socket activation not enabled, create our own */
#endif /* WITH_SYSTEMD_SOCKET_ACTIVATION */
    {
        mode_t mode = umask(0111);
        udscs_server_listen_to_address(server, vdagentd_socket, &err);
        umask(mode);
    }

    if (err) {
        syslog(LOG_CRIT, "Fatal could not create the server socket %s: %s",
                         vdagentd_socket, err->message);
        g_error_free(err);
        udscs_destroy_server(server);
        return 1;
    }

#ifdef WITH_STATIC_UINPUT
    uinput = vdagentd_uinput_create(uinput_device, 1024, 768, NULL, 0,
                                    debug > 1, uinput_fake);
    if (!uinput) {
        udscs_destroy_server(server);
        return 1;
    }
#endif

    if (do_daemonize)
        daemonize();

    g_unix_signal_add(SIGINT, signal_handler, NULL);
    g_unix_signal_add(SIGHUP, signal_handler, NULL);
    g_unix_signal_add(SIGTERM, signal_handler, NULL);

    if (want_session_info)
        session_info = session_info_create(debug);
    if (session_info) {
        si_io_channel = g_io_channel_unix_new(session_info_get_fd(session_info));
        si_watch_id = g_io_add_watch(si_io_channel, G_IO_IN, si_io_channel_cb, NULL);
        g_io_channel_unref(si_io_channel);
    } else {
        syslog(LOG_WARNING, "no session info, max 1 session agent allowed");
    }

    active_xfers = g_hash_table_new(g_direct_hash, g_direct_equal);

    udscs_server_start(server);
    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    release_clipboards();

    vdagentd_uinput_destroy(&uinput);
    if (si_watch_id > 0) {
        g_source_remove(si_watch_id);
    }
    g_clear_pointer(&session_info, session_info_destroy);
    g_clear_pointer(&server, udscs_destroy_server);
    if (virtio_port) {
        vdagent_connection_flush(VDAGENT_CONNECTION(virtio_port));
        g_clear_pointer(&virtio_port, vdagent_connection_destroy);
    }

    /* allow the VDAgentConnection(s) to finalize properly */
    g_main_context_iteration(NULL, FALSE);

    g_main_loop_unref(loop);

    /* leave the socket around if it was provided by systemd */
    if (own_socket) {
        if (unlink(vdagentd_socket) != 0)
            syslog(LOG_ERR, "unlink %s: %s", vdagentd_socket, strerror(errno));
    }
    syslog(LOG_INFO, "vdagentd quitting, returning status %d", retval);

    if (do_daemonize)
        unlink(pidfilename);

    g_free(portdev);
    g_free(vdagentd_socket);
    g_free(uinput_device);

    return retval;
}

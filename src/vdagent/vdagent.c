/*  vdagent.c xorg-client to vdagentd (daemon).

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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <spice/vd_agent.h>
#include <poll.h>
#include <glib-unix.h>

#include "udscs.h"
#include "vdagentd-proto.h"
#include "vdagentd-proto-strings.h"
#include "audio.h"
#include "x11.h"
#include "file-xfers.h"

typedef struct VDAgent {
    struct vdagent_x11 *x11;
    struct vdagent_file_xfers *xfers;
    struct udscs_connection *conn;
    GIOChannel *x11_channel;

    GMainLoop *loop;
} VDAgent;

static int quit = 0;
static int parent_socket = -1;
static int version_mismatch = 0;

/* Command line options */
static gboolean debug = FALSE;
static gboolean x11_sync = FALSE;
static gboolean do_daemonize = TRUE;
static gint fx_open_dir = -1;
static gchar *fx_dir = NULL;
static gchar *portdev = NULL;
static gchar *vdagentd_socket = NULL;

static GOptionEntry entries[] = {
    { "debug", 'd', 0,
       G_OPTION_ARG_NONE, &debug,
       "Enable debug", NULL },
    { "virtio-serial-port-path", 's', 0,
      G_OPTION_ARG_STRING, &portdev,
      "Set virtio-serial path ("  DEFAULT_VIRTIO_PORT_PATH ")", NULL },
    { "vdagentd-socket", 'S', 0, G_OPTION_ARG_STRING,
       &vdagentd_socket,
       "Set spice-vdagentd socket (" VDAGENTD_SOCKET ")", NULL },
    { "foreground", 'x', G_OPTION_FLAG_REVERSE,
       G_OPTION_ARG_NONE, &do_daemonize,
       "Do not daemonize the agent", NULL },
    { "file-xfer-save-dir", 'f', 0,
      G_OPTION_ARG_STRING, &fx_dir,
      "Set directory to file transfers files", "<dir|xdg-desktop|xdg-download>"},
    { "file-xfer-open-dir", 'o', 0,
       G_OPTION_ARG_INT, &fx_open_dir,
       "Open directory after completing file transfer", "<0|1>" },
    { "x11-abort-on-error", 'y', G_OPTION_FLAG_HIDDEN,
      G_OPTION_ARG_NONE, &x11_sync,
      "Aborts on errors from X11", NULL },
    { NULL }
};

/**
 * xfer_get_download_directory
 *
 * Return path where transferred files should be stored.
 * Returned path should not be freed or modified.
 **/
static const gchar *xfer_get_download_directory(VDAgent *agent)
{
    if (fx_dir != NULL) {
        if (!strcmp(fx_dir, "xdg-desktop"))
            return g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
        if (!strcmp(fx_dir, "xdg-download"))
            return g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);

        return fx_dir;
    }

    return g_get_user_special_dir(vdagent_x11_has_icons_on_desktop(agent->x11) ?
                                  G_USER_DIRECTORY_DESKTOP :
                                  G_USER_DIRECTORY_DOWNLOAD);
}

/**
 * vdagent_init_file_xfer
 *
 * Initialize handler for file xfer,
 * return TRUE on success (agent->xfers is not NULL).
 **/
static gboolean vdagent_init_file_xfer(VDAgent *agent)
{
    const gchar *xfer_dir;

    if (agent->xfers != NULL) {
        syslog(LOG_DEBUG, "File-xfer already initialized");
        return TRUE;
    }

    xfer_dir = xfer_get_download_directory(agent);
    if (xfer_dir == NULL) {
        syslog(LOG_WARNING,
               "warning could not get file xfer save dir, "
               "file transfers will be disabled");
        return FALSE;
    }

    if (fx_open_dir == -1)
        fx_open_dir = !vdagent_x11_has_icons_on_desktop(agent->x11);

    agent->xfers = vdagent_file_xfers_create(agent->conn, xfer_dir,
                                             fx_open_dir, debug);
    return (agent->xfers != NULL);
}

static gboolean vdagent_finalize_file_xfer(VDAgent *agent)
{
    if (agent->xfers == NULL)
        return FALSE;

    g_clear_pointer(&agent->xfers, vdagent_file_xfers_destroy);
    return TRUE;
}

static void daemon_read_complete(struct udscs_connection **connp,
    struct udscs_message_header *header, uint8_t *data)
{
    VDAgent *agent = udscs_get_user_data(*connp);

    switch (header->type) {
    case VDAGENTD_MONITORS_CONFIG:
        vdagent_x11_set_monitor_config(agent->x11, (VDAgentMonitorsConfig *)data, 0);
        break;
    case VDAGENTD_CLIPBOARD_REQUEST:
        vdagent_x11_clipboard_request(agent->x11, header->arg1, header->arg2);
        break;
    case VDAGENTD_CLIPBOARD_GRAB:
        vdagent_x11_clipboard_grab(agent->x11, header->arg1, (uint32_t *)data,
                                   header->size / sizeof(uint32_t));
        break;
    case VDAGENTD_CLIPBOARD_DATA:
        vdagent_x11_clipboard_data(agent->x11, header->arg1, header->arg2,
                                   data, header->size);
        break;
    case VDAGENTD_CLIPBOARD_RELEASE:
        vdagent_x11_clipboard_release(agent->x11, header->arg1);
        break;
    case VDAGENTD_VERSION:
        if (strcmp((char *)data, VERSION) != 0) {
            syslog(LOG_INFO, "vdagentd version mismatch: got %s expected %s",
                   data, VERSION);
            g_main_loop_quit(agent->loop);
            version_mismatch = 1;
        }
        break;
    case VDAGENTD_FILE_XFER_START:
        if (agent->xfers != NULL) {
            vdagent_file_xfers_start(agent->xfers,
                                     (VDAgentFileXferStartMessage *)data);
        } else {
            vdagent_file_xfers_error_disabled(*connp,
                                              ((VDAgentFileXferStartMessage *)data)->id);
        }
        break;
    case VDAGENTD_FILE_XFER_STATUS:
        if (agent->xfers != NULL) {
            vdagent_file_xfers_status(agent->xfers,
                                      (VDAgentFileXferStatusMessage *)data);
        } else {
            vdagent_file_xfers_error_disabled(*connp,
                                              ((VDAgentFileXferStatusMessage *)data)->id);
        }
        break;
    case VDAGENTD_FILE_XFER_DISABLE:
        if (debug)
            syslog(LOG_DEBUG, "Disabling file-xfers");

        vdagent_finalize_file_xfer(agent);
        break;
    case VDAGENTD_AUDIO_VOLUME_SYNC: {
        VDAgentAudioVolumeSync *avs = (VDAgentAudioVolumeSync *)data;
        if (avs->is_playback) {
            vdagent_audio_playback_sync(avs->mute, avs->nchannels, avs->volume);
        } else {
            vdagent_audio_record_sync(avs->mute, avs->nchannels, avs->volume);
        }
        break;
    }
    case VDAGENTD_FILE_XFER_DATA:
        if (agent->xfers != NULL) {
            vdagent_file_xfers_data(agent->xfers,
                                    (VDAgentFileXferDataMessage *)data);
        } else {
            vdagent_file_xfers_error_disabled(*connp,
                                              ((VDAgentFileXferDataMessage *)data)->id);
        }
        break;
    case VDAGENTD_CLIENT_DISCONNECTED:
        vdagent_x11_client_disconnected(agent->x11);
        if (vdagent_finalize_file_xfer(agent)) {
            vdagent_init_file_xfer(agent);
        }
        break;
    default:
        syslog(LOG_ERR, "Unknown message from vdagentd type: %d, ignoring",
               header->type);
    }
}

static void daemon_disconnect_cb(struct udscs_connection *conn)
{
    VDAgent *agent = udscs_get_user_data(conn);
    agent->conn = NULL;
    if (agent->loop)
        g_main_loop_quit(agent->loop);
}

/* When we daemonize, it is useful to have the main process
   wait to make sure the X connection worked.  We wait up
   to 10 seconds to get an 'all clear' from the child
   before we exit.  If we don't, we're able to exit with a
   status that indicates an error occurred */
static void wait_and_exit(int s)
{
    char buf[4];
    struct pollfd p;
    p.fd = s;
    p.events = POLLIN;

    if (poll(&p, 1, 10000) > 0)
        if (read(s, buf, sizeof(buf)) > 0)
            exit(0);

    exit(1);
}

static int daemonize(void)
{
    int x;
    int fd[2];

    if (socketpair(PF_LOCAL, SOCK_STREAM, 0, fd)) {
        syslog(LOG_ERR, "socketpair : %s", strerror(errno));
        exit(1);
    }

    /* detach from terminal */
    switch (fork()) {
    case 0:
        close(0); close(1); close(2);
        setsid();
        x = open("/dev/null", O_RDWR); x = dup(x); x = dup(x);
        close(fd[0]);
        return fd[1];
    case -1:
        syslog(LOG_ERR, "fork: %s", strerror(errno));
        exit(1);
    default:
        close(fd[1]);
        wait_and_exit(fd[0]);
    }

    return 0;
}

static int file_test(const char *path)
{
    struct stat buffer;

    return stat(path, &buffer);
}

static gboolean x11_io_channel_cb(GIOChannel *source,
                                  GIOCondition condition,
                                  gpointer data)
{
    VDAgent *agent = data;
    vdagent_x11_do_read(agent->x11);

    return G_SOURCE_CONTINUE;
}

gboolean vdagent_signal_handler(gpointer user_data)
{
    VDAgent *agent = user_data;
    quit = TRUE;
    g_main_loop_quit(agent->loop);
    return G_SOURCE_REMOVE;
}

static VDAgent *vdagent_new(void)
{
    VDAgent *agent = g_new0(VDAgent, 1);

    agent->loop = g_main_loop_new(NULL, FALSE);

    g_unix_signal_add(SIGINT, vdagent_signal_handler, agent);
    g_unix_signal_add(SIGHUP, vdagent_signal_handler, agent);
    g_unix_signal_add(SIGTERM, vdagent_signal_handler, agent);

    return agent;
}

static void vdagent_destroy(VDAgent *agent)
{
    vdagent_finalize_file_xfer(agent);
    vdagent_x11_destroy(agent->x11, agent->conn == NULL);
    udscs_destroy_connection(&agent->conn);

    while (g_source_remove_by_user_data(agent))
        continue;

    g_clear_pointer(&agent->x11_channel, g_io_channel_unref);
    g_clear_pointer(&agent->loop, g_main_loop_unref);
    g_free(agent);
}

static gboolean vdagent_init_async_cb(gpointer user_data)
{
    VDAgent *agent = user_data;

    agent->conn = udscs_connect(vdagentd_socket,
                                daemon_read_complete, daemon_disconnect_cb,
                                vdagentd_messages, VDAGENTD_NO_MESSAGES, debug);
    if (agent->conn == NULL) {
        g_timeout_add_seconds(1, vdagent_init_async_cb, agent);
        return G_SOURCE_REMOVE;
    }
    udscs_set_user_data(agent->conn, agent);

    agent->x11 = vdagent_x11_create(agent->conn, debug, x11_sync);
    if (agent->x11 == NULL)
        goto err_init;
    agent->x11_channel = g_io_channel_unix_new(vdagent_x11_get_fd(agent->x11));
    if (agent->x11_channel == NULL)
        goto err_init;

    g_io_add_watch(agent->x11_channel,
                   G_IO_IN,
                   x11_io_channel_cb,
                   agent);

    if (!vdagent_init_file_xfer(agent))
        syslog(LOG_WARNING, "File transfer is disabled");

    if (parent_socket != -1) {
        if (write(parent_socket, "OK", 2) != 2)
            syslog(LOG_WARNING, "Parent already gone.");
        close(parent_socket);
        parent_socket = -1;
    }

    return G_SOURCE_REMOVE;

err_init:
    g_main_loop_quit(agent->loop);
    quit = TRUE;
    return G_SOURCE_REMOVE;
}

int main(int argc, char *argv[])
{
    GOptionContext *context;
    GError *error = NULL;
    VDAgent *agent;

    context = g_option_context_new(NULL);
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_set_summary(context,
                                 "\tSpice session guest agent: X11\n"
                                 "\tVersion: " VERSION);
    g_option_context_parse(context, &argc, &argv, &error);
    g_option_context_free(context);

    if (error != NULL) {
        g_printerr("Invalid arguments, %s\n", error->message);
        g_clear_error(&error);
        return -1;
    }

    /* Set default path value if none was set */
    if (portdev == NULL)
        portdev = g_strdup(DEFAULT_VIRTIO_PORT_PATH);

    if (vdagentd_socket == NULL)
        vdagentd_socket = g_strdup(VDAGENTD_SOCKET);

    openlog("spice-vdagent", do_daemonize ? LOG_PID : (LOG_PID | LOG_PERROR),
            LOG_USER);

    if (file_test(portdev) != 0) {
        syslog(LOG_ERR, "Cannot access vdagent virtio channel %s", portdev);
        return 1;
    }

    if (do_daemonize)
        parent_socket = daemonize();

reconnect:
    if (version_mismatch) {
        syslog(LOG_INFO, "Version mismatch, restarting");
        sleep(1);
        execvp(argv[0], argv);
    }

    agent = vdagent_new();

    g_timeout_add(0, vdagent_init_async_cb, agent);

    g_main_loop_run(agent->loop);

    vdagent_destroy(agent);
    agent = NULL;

    if (!quit && do_daemonize)
        goto reconnect;

    g_free(fx_dir);
    g_free(portdev);
    g_free(vdagentd_socket);

    return 0;
}

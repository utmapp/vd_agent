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
#include <glib.h>
#include <poll.h>

#include "udscs.h"
#include "vdagentd-proto.h"
#include "vdagentd-proto-strings.h"
#include "audio.h"
#include "x11.h"
#include "file-xfers.h"

static struct vdagent_x11 *x11 = NULL;
static struct vdagent_file_xfers *vdagent_file_xfers = NULL;
static struct udscs_connection *client = NULL;
static int quit = 0;
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
static const gchar *xfer_get_download_directory(void)
{
    if (fx_dir != NULL) {
        if (!strcmp(fx_dir, "xdg-desktop"))
            return g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
        if (!strcmp(fx_dir, "xdg-download"))
            return g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);

        return fx_dir;
    }

    return g_get_user_special_dir(vdagent_x11_has_icons_on_desktop(x11) ?
                                  G_USER_DIRECTORY_DESKTOP :
                                  G_USER_DIRECTORY_DOWNLOAD);
}

/**
 * vdagent_init_file_xfer
 *
 * Initialize handler for file xfer,
 * return TRUE on success (vdagent_file_xfers is not NULL).
 **/
static gboolean vdagent_init_file_xfer(void)
{
    const gchar *xfer_dir;

    if (vdagent_file_xfers != NULL) {
        syslog(LOG_DEBUG, "File-xfer already initialized");
        return TRUE;
    }

    xfer_dir = xfer_get_download_directory();
    if (xfer_dir == NULL) {
        syslog(LOG_WARNING,
               "warning could not get file xfer save dir, "
               "file transfers will be disabled");
        vdagent_file_xfers = NULL;
        return FALSE;
    }

    if (fx_open_dir == -1)
        fx_open_dir = !vdagent_x11_has_icons_on_desktop(x11);

    vdagent_file_xfers = vdagent_file_xfers_create(client, xfer_dir,
                                                   fx_open_dir, debug);
    return (vdagent_file_xfers != NULL);
}

static gboolean vdagent_finalize_file_xfer(void)
{
    if (vdagent_file_xfers == NULL)
        return FALSE;

    g_clear_pointer(&vdagent_file_xfers, vdagent_file_xfers_destroy);
    return TRUE;
}

static void daemon_read_complete(struct udscs_connection **connp,
    struct udscs_message_header *header, uint8_t *data)
{
    switch (header->type) {
    case VDAGENTD_MONITORS_CONFIG:
        vdagent_x11_set_monitor_config(x11, (VDAgentMonitorsConfig *)data, 0);
        break;
    case VDAGENTD_CLIPBOARD_REQUEST:
        vdagent_x11_clipboard_request(x11, header->arg1, header->arg2);
        break;
    case VDAGENTD_CLIPBOARD_GRAB:
        vdagent_x11_clipboard_grab(x11, header->arg1, (uint32_t *)data,
                                   header->size / sizeof(uint32_t));
        break;
    case VDAGENTD_CLIPBOARD_DATA:
        vdagent_x11_clipboard_data(x11, header->arg1, header->arg2,
                                   data, header->size);
        break;
    case VDAGENTD_CLIPBOARD_RELEASE:
        vdagent_x11_clipboard_release(x11, header->arg1);
        break;
    case VDAGENTD_VERSION:
        if (strcmp((char *)data, VERSION) != 0) {
            syslog(LOG_INFO, "vdagentd version mismatch: got %s expected %s",
                   data, VERSION);
            udscs_destroy_connection(connp);
            version_mismatch = 1;
        }
        break;
    case VDAGENTD_FILE_XFER_START:
        if (vdagent_file_xfers != NULL) {
            vdagent_file_xfers_start(vdagent_file_xfers,
                                     (VDAgentFileXferStartMessage *)data);
        } else {
            vdagent_file_xfers_error_disabled(*connp,
                                              ((VDAgentFileXferStartMessage *)data)->id);
        }
        break;
    case VDAGENTD_FILE_XFER_STATUS:
        if (vdagent_file_xfers != NULL) {
            vdagent_file_xfers_status(vdagent_file_xfers,
                                      (VDAgentFileXferStatusMessage *)data);
        } else {
            vdagent_file_xfers_error_disabled(*connp,
                                              ((VDAgentFileXferStatusMessage *)data)->id);
        }
        break;
    case VDAGENTD_FILE_XFER_DISABLE:
        if (debug)
            syslog(LOG_DEBUG, "Disabling file-xfers");

        vdagent_finalize_file_xfer();
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
        if (vdagent_file_xfers != NULL) {
            vdagent_file_xfers_data(vdagent_file_xfers,
                                    (VDAgentFileXferDataMessage *)data);
        } else {
            vdagent_file_xfers_error_disabled(*connp,
                                              ((VDAgentFileXferDataMessage *)data)->id);
        }
        break;
    case VDAGENTD_CLIENT_DISCONNECTED:
        vdagent_x11_client_disconnected(x11);
        if (vdagent_finalize_file_xfer()) {
            vdagent_init_file_xfer();
        }
        break;
    default:
        syslog(LOG_ERR, "Unknown message from vdagentd type: %d, ignoring",
               header->type);
    }
}

static struct udscs_connection *client_setup_sync(void)
{
    struct udscs_connection *conn = NULL;

    while (!quit) {
        conn = udscs_connect(vdagentd_socket, daemon_read_complete, NULL,
                             vdagentd_messages, VDAGENTD_NO_MESSAGES,
                             debug);
        if (conn || !do_daemonize || quit) {
            break;
        }
        sleep(1);
    }
    return conn;
}

static void quit_handler(int sig)
{
    quit = 1;
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

int main(int argc, char *argv[])
{
    fd_set readfds, writefds;
    int n, nfds, x11_fd;
    int parent_socket = 0;
    struct sigaction act;
    GOptionContext *context;
    GError *error = NULL;

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

    memset(&act, 0, sizeof(act));
    act.sa_flags = SA_RESTART;
    act.sa_handler = quit_handler;
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGQUIT, &act, NULL);

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

    client = client_setup_sync();
    if (client == NULL) {
        return 1;
    }

    x11 = vdagent_x11_create(client, debug, x11_sync);
    if (!x11) {
        udscs_destroy_connection(&client);
        return 1;
    }

    if (!vdagent_init_file_xfer())
        syslog(LOG_WARNING, "File transfer is disabled");

    if (parent_socket) {
        if (write(parent_socket, "OK", 2) != 2)
            syslog(LOG_WARNING, "Parent already gone.");
        close(parent_socket);
        parent_socket = 0;
    }

    while (client && !quit) {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        nfds = udscs_client_fill_fds(client, &readfds, &writefds);
        x11_fd = vdagent_x11_get_fd(x11);
        FD_SET(x11_fd, &readfds);
        if (x11_fd >= nfds)
            nfds = x11_fd + 1;

        n = select(nfds, &readfds, &writefds, NULL, NULL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "Fatal error select: %s", strerror(errno));
            break;
        }

        if (FD_ISSET(x11_fd, &readfds))
            vdagent_x11_do_read(x11);
        udscs_client_handle_fds(&client, &readfds, &writefds);
    }

    vdagent_finalize_file_xfer();
    vdagent_x11_destroy(x11, client == NULL);
    udscs_destroy_connection(&client);
    if (!quit && do_daemonize)
        goto reconnect;

    g_free(fx_dir);
    g_free(portdev);
    g_free(vdagentd_socket);

    return 0;
}

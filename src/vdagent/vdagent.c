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
#include "vdagent-audio.h"
#include "vdagent-x11.h"
#include "vdagent-file-xfers.h"

static const char *portdev = "/dev/virtio-ports/com.redhat.spice.0";
static const char *vdagentd_socket = VDAGENTD_SOCKET;
static int debug = 0;
static const char *fx_dir = NULL;
static int fx_open_dir = -1;
static struct vdagent_x11 *x11 = NULL;
static struct vdagent_file_xfers *vdagent_file_xfers = NULL;
static struct udscs_connection *client = NULL;
static int quit = 0;
static int version_mismatch = 0;

static void daemon_read_complete(struct udscs_connection **connp,
    struct udscs_message_header *header, uint8_t *data)
{
    switch (header->type) {
    case VDAGENTD_MONITORS_CONFIG:
        vdagent_x11_set_monitor_config(x11, (VDAgentMonitorsConfig *)data, 0);
        free(data);
        break;
    case VDAGENTD_CLIPBOARD_REQUEST:
        vdagent_x11_clipboard_request(x11, header->arg1, header->arg2);
        free(data);
        break;
    case VDAGENTD_CLIPBOARD_GRAB:
        vdagent_x11_clipboard_grab(x11, header->arg1, (uint32_t *)data,
                                   header->size / sizeof(uint32_t));
        free(data);
        break;
    case VDAGENTD_CLIPBOARD_DATA:
        vdagent_x11_clipboard_data(x11, header->arg1, header->arg2,
                                   data, header->size);
        /* vdagent_x11_clipboard_data takes ownership of the data (or frees
           it immediately) */
        break;
    case VDAGENTD_CLIPBOARD_RELEASE:
        vdagent_x11_clipboard_release(x11, header->arg1);
        free(data);
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
            vdagent_file_xfers_error(*connp,
                                     ((VDAgentFileXferStartMessage *)data)->id);
        }
        free(data);
        break;
    case VDAGENTD_FILE_XFER_STATUS:
        if (vdagent_file_xfers != NULL) {
            vdagent_file_xfers_status(vdagent_file_xfers,
                                      (VDAgentFileXferStatusMessage *)data);
        } else {
            vdagent_file_xfers_error(*connp,
                                     ((VDAgentFileXferStatusMessage *)data)->id);
        }
        free(data);
        break;
    case VDAGENTD_FILE_XFER_DISABLE:
        if (debug)
            syslog(LOG_DEBUG, "Disabling file-xfers");

        if (vdagent_file_xfers != NULL) {
            vdagent_file_xfers_destroy(vdagent_file_xfers);
            vdagent_file_xfers = NULL;
        }
        break;
    case VDAGENTD_AUDIO_VOLUME_SYNC: {
        VDAgentAudioVolumeSync *avs = (VDAgentAudioVolumeSync *)data;
        if (avs->is_playback) {
            vdagent_audio_playback_sync(avs->mute, avs->nchannels, avs->volume);
        } else {
            vdagent_audio_record_sync(avs->mute, avs->nchannels, avs->volume);
        }
        free(data);
        break;
    }
    case VDAGENTD_FILE_XFER_DATA:
        if (vdagent_file_xfers != NULL) {
            vdagent_file_xfers_data(vdagent_file_xfers,
                                    (VDAgentFileXferDataMessage *)data);
        } else {
            vdagent_file_xfers_error(*connp,
                                     ((VDAgentFileXferDataMessage *)data)->id);
        }
        free(data);
        break;
    case VDAGENTD_CLIENT_DISCONNECTED:
        vdagent_x11_client_disconnected(x11);
        if (vdagent_file_xfers != NULL) {
            vdagent_file_xfers_destroy(vdagent_file_xfers);
            vdagent_file_xfers = vdagent_file_xfers_create(client, fx_dir,
                                                           fx_open_dir, debug);
        }
        break;
    default:
        syslog(LOG_ERR, "Unknown message from vdagentd type: %d, ignoring",
               header->type);
        free(data);
    }
}

static int client_setup(int reconnect)
{
    while (!quit) {
        client = udscs_connect(vdagentd_socket, daemon_read_complete, NULL,
                               vdagentd_messages, VDAGENTD_NO_MESSAGES,
                               debug);
        if (client || !reconnect || quit) {
            break;
        }
        sleep(1);
    }
    return client == NULL;
}

static void usage(FILE *fp)
{
    fprintf(fp,
      "Usage: spice-vdagent [OPTIONS]\n\n"
      "Spice guest agent X11 session agent, version %s.\n\n"
      "Options:\n"
      "  -h                                print this text\n"
      "  -d                                log debug messages\n"
      "  -s <port>                         set virtio serial port\n"
      "  -S <filename>                     set udcs socket\n"
      "  -x                                don't daemonize\n"
      "  -f <dir|xdg-desktop|xdg-download> file xfer save dir\n"
      "  -o <0|1>                          open dir on file xfer completion\n",
      VERSION);
}

static void quit_handler(int sig)
{
    quit = 1;
}

/* When we daemonize, it is useful to have the main process
   wait to make sure the X connection worked.  We wait up
   to 10 seconds to get an 'all clear' from the child
   before we exit.  If we don't, we're able to exit with a
   status that indicates an error occured */
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
    int c, n, nfds, x11_fd;
    int do_daemonize = 1;
    int parent_socket = 0;
    int x11_sync = 0;
    struct sigaction act;

    for (;;) {
        if (-1 == (c = getopt(argc, argv, "-dxhys:f:o:S:")))
            break;
        switch (c) {
        case 'd':
            debug++;
            break;
        case 's':
            portdev = optarg;
            break;
        case 'x':
            do_daemonize = 0;
            break;
        case 'y':
            x11_sync = 1;
            break;
        case 'h':
            usage(stdout);
            return 0;
        case 'f':
            fx_dir = optarg;
            break;
        case 'o':
            fx_open_dir = atoi(optarg);
            break;
        case 'S':
            vdagentd_socket = optarg;
            break;
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

    if (client_setup(do_daemonize)) {
        return 1;
    }

    x11 = vdagent_x11_create(client, debug, x11_sync);
    if (!x11) {
        udscs_destroy_connection(&client);
        return 1;
    }

    if (!fx_dir) {
        if (vdagent_x11_has_icons_on_desktop(x11))
            fx_dir = "xdg-desktop";
        else
            fx_dir = "xdg-download";
    }
    if (fx_open_dir == -1)
        fx_open_dir = !vdagent_x11_has_icons_on_desktop(x11);
    if (!strcmp(fx_dir, "xdg-desktop"))
        fx_dir = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    else if (!strcmp(fx_dir, "xdg-download"))
        fx_dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    if (fx_dir) {
        vdagent_file_xfers = vdagent_file_xfers_create(client, fx_dir,
                                                       fx_open_dir, debug);
    } else {
        syslog(LOG_WARNING,
               "warning could not get file xfer save dir, file transfers will be disabled");
        vdagent_file_xfers = NULL;
    }

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

    if (vdagent_file_xfers != NULL) {
        vdagent_file_xfers_destroy(vdagent_file_xfers);
    }
    vdagent_x11_destroy(x11, client == NULL);
    udscs_destroy_connection(&client);
    if (!quit && do_daemonize)
        goto reconnect;

    return 0;
}

/*  vdagent file xfers code

    Copyright 2013 - 2016 Red Hat, Inc.

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
#include <inttypes.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <spice/vd_agent.h>
#include <glib.h>

#include "vdagentd-proto.h"
#include "file-xfers.h"

struct vdagent_file_xfers {
    GHashTable *xfers;
    struct udscs_connection *vdagentd;
    char *save_dir;
    int open_save_dir;
    int debug;
};

typedef struct AgentFileXferTask {
    uint32_t                       id;
    int                            file_fd;
    uint64_t                       read_bytes;
    char                           *file_name;
    uint64_t                       file_size;
    int                            file_xfer_nr;
    int                            file_xfer_total;
    int                            debug;
} AgentFileXferTask;

static void vdagent_file_xfer_task_free(gpointer data)
{
    AgentFileXferTask *task = data;

    g_return_if_fail(task != NULL);

    if (task->file_fd > 0) {
        syslog(LOG_ERR, "file-xfer: Removing task %u and file %s due to error",
               task->id, task->file_name);
        close(task->file_fd);
        unlink(task->file_name);
    } else if (task->debug)
        syslog(LOG_DEBUG, "file-xfer: Removing task %u %s",
               task->id, task->file_name);

    g_free(task->file_name);
    g_free(task);
}

struct vdagent_file_xfers *vdagent_file_xfers_create(
    struct udscs_connection *vdagentd, const char *save_dir,
    int open_save_dir, int debug)
{
    struct vdagent_file_xfers *xfers;

    xfers = g_malloc(sizeof(*xfers));
    xfers->xfers = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                         NULL, vdagent_file_xfer_task_free);
    xfers->vdagentd = vdagentd;
    xfers->save_dir = g_strdup(save_dir);
    xfers->open_save_dir = open_save_dir;
    xfers->debug = debug;

    return xfers;
}

void vdagent_file_xfers_destroy(struct vdagent_file_xfers *xfers)
{
    g_return_if_fail(xfers != NULL);

    g_hash_table_destroy(xfers->xfers);
    g_free(xfers->save_dir);
    g_free(xfers);
}

static AgentFileXferTask *vdagent_file_xfers_get_task(
    struct vdagent_file_xfers *xfers, uint32_t id)
{
    AgentFileXferTask *task;

    g_return_val_if_fail(xfers != NULL, NULL);

    task = g_hash_table_lookup(xfers->xfers, GUINT_TO_POINTER(id));
    if (task == NULL)
        syslog(LOG_ERR, "file-xfer: error cannot find task %u", id);

    return task;
}

/* Parse start message then create a new file xfer task */
static AgentFileXferTask *vdagent_parse_start_msg(
    VDAgentFileXferStartMessage *msg)
{
    GKeyFile *keyfile = NULL;
    AgentFileXferTask *task = NULL;
    GError *error = NULL;

    keyfile = g_key_file_new();
    if (g_key_file_load_from_data(keyfile,
                                  (const gchar *)msg->data,
                                  -1,
                                  G_KEY_FILE_NONE, &error) == FALSE) {
        syslog(LOG_ERR, "file-xfer: failed to load keyfile: %s",
               error->message);
        goto error;
    }
    task = g_new0(AgentFileXferTask, 1);
    task->id = msg->id;
    task->file_name = g_key_file_get_string(
        keyfile, "vdagent-file-xfer", "name", &error);
    if (error) {
        syslog(LOG_ERR, "file-xfer: failed to parse filename: %s",
               error->message);
        goto error;
    }
    task->file_size = g_key_file_get_uint64(
        keyfile, "vdagent-file-xfer", "size", &error);
    if (error) {
        syslog(LOG_ERR, "file-xfer: failed to parse filesize: %s",
               error->message);
        goto error;
    }
    /* These are set for xfers which are part of a multi-file xfer */
    task->file_xfer_nr = g_key_file_get_integer(
        keyfile, "vdagent-file-xfer", "file-xfer-nr", NULL);
    task->file_xfer_total = g_key_file_get_integer(
        keyfile, "vdagent-file-xfer", "file-xfer-total", NULL);

    g_key_file_free(keyfile);
    return task;

error:
    g_clear_error(&error);
    if (task)
        vdagent_file_xfer_task_free(task);
    if (keyfile)
        g_key_file_free(keyfile);
    return NULL;
}

static uint64_t get_free_space_available(const char *path)
{
    struct statvfs stat;
    if (statvfs(path, &stat) != 0) {
        syslog(LOG_WARNING, "file-xfer: failed to get free space, statvfs error: %s",
               strerror(errno));
        return G_MAXUINT64;
    }
    return stat.f_bsize * stat.f_bavail;
}

void vdagent_file_xfers_start(struct vdagent_file_xfers *xfers,
    VDAgentFileXferStartMessage *msg)
{
    AgentFileXferTask *task;
    char *dir = NULL, *path = NULL, *file_path = NULL;
    struct stat st;
    int i;
    uint64_t free_space;

    g_return_if_fail(xfers != NULL);

    if (g_hash_table_lookup(xfers->xfers, GUINT_TO_POINTER(msg->id))) {
        syslog(LOG_ERR, "file-xfer: error id %u already exists, ignoring!",
               msg->id);
        return;
    }

    task = vdagent_parse_start_msg(msg);
    if (task == NULL) {
        goto error;
    }

    task->debug = xfers->debug;

    file_path = g_build_filename(xfers->save_dir, task->file_name, NULL);

    free_space = get_free_space_available(xfers->save_dir);
    if (task->file_size > free_space) {
        gchar *free_space_str, *file_size_str;
#if GLIB_CHECK_VERSION(2, 30, 0)
        free_space_str = g_format_size(free_space);
        file_size_str = g_format_size(task->file_size);
#else
        free_space_str = g_format_size_for_display(free_space);
        file_size_str = g_format_size_for_display(task->file_size);
#endif
        syslog(LOG_ERR, "file-xfer: not enough free space (%s to copy, %s free)",
               file_size_str, free_space_str);
        g_free(free_space_str);
        g_free(file_size_str);

        udscs_write(xfers->vdagentd,
                    VDAGENTD_FILE_XFER_STATUS,
                    msg->id,
                    VD_AGENT_FILE_XFER_STATUS_NOT_ENOUGH_SPACE,
                    (uint8_t *)&free_space,
                    sizeof(free_space));
        vdagent_file_xfer_task_free(task);
        g_free(file_path);
        g_free(dir);
        return;
    }

    dir = g_path_get_dirname(file_path);
    if (g_mkdir_with_parents(dir, S_IRWXU) == -1) {
        syslog(LOG_ERR, "file-xfer: Failed to create dir %s", dir);
        goto error;
    }

    path = g_strdup(file_path);
    for (i = 0; i < 64 && (stat(path, &st) == 0 || errno != ENOENT); i++) {
        g_free(path);
        char *extension = strrchr(file_path, '.');
        int basename_len = extension != NULL ? extension - file_path : strlen(file_path);
        path = g_strdup_printf("%.*s (%i)%s", basename_len, file_path,
                               i + 1, extension ? extension : "");
    }
    g_free(task->file_name);
    task->file_name = path;
    if (i == 64) {
        syslog(LOG_ERR, "file-xfer: more than 63 copies of %s exist?",
               file_path);
        goto error;
    }

    task->file_fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (task->file_fd == -1) {
        syslog(LOG_ERR, "file-xfer: failed to create file %s: %s",
               path, strerror(errno));
        goto error;
    }

    if (ftruncate(task->file_fd, task->file_size) < 0) {
        syslog(LOG_ERR, "file-xfer: err reserving %"PRIu64" bytes for %s: %s",
               task->file_size, path, strerror(errno));
        goto error;
    }

    g_hash_table_insert(xfers->xfers, GUINT_TO_POINTER(msg->id), task);

    if (xfers->debug)
        syslog(LOG_DEBUG, "file-xfer: Adding task %u %s %"PRIu64" bytes",
               task->id, path, task->file_size);

    udscs_write(xfers->vdagentd, VDAGENTD_FILE_XFER_STATUS,
                msg->id, VD_AGENT_FILE_XFER_STATUS_CAN_SEND_DATA, NULL, 0);
    g_free(file_path);
    g_free(dir);
    return ;

error:
    udscs_write(xfers->vdagentd, VDAGENTD_FILE_XFER_STATUS,
                msg->id, VD_AGENT_FILE_XFER_STATUS_ERROR, NULL, 0);
    if (task)
        vdagent_file_xfer_task_free(task);
    g_free(file_path);
    g_free(dir);
}

void vdagent_file_xfers_status(struct vdagent_file_xfers *xfers,
    VDAgentFileXferStatusMessage *msg)
{
    AgentFileXferTask *task;

    g_return_if_fail(xfers != NULL);

    task = vdagent_file_xfers_get_task(xfers, msg->id);
    if (!task)
        return;

    switch (msg->result) {
    case VD_AGENT_FILE_XFER_STATUS_CAN_SEND_DATA:
        syslog(LOG_ERR, "file-xfer: task %u %s received unexpected 0 response",
               task->id, task->file_name);
        break;
    default:
        /* Cancel or Error, remove this task */
        g_hash_table_remove(xfers->xfers, GUINT_TO_POINTER(msg->id));
    }
}

void vdagent_file_xfers_data(struct vdagent_file_xfers *xfers,
    VDAgentFileXferDataMessage *msg)
{
    AgentFileXferTask *task;
    int len, status = -1;

    g_return_if_fail(xfers != NULL);

    task = vdagent_file_xfers_get_task(xfers, msg->id);
    if (!task)
        return;

    len = write(task->file_fd, msg->data, msg->size);
    if (len == msg->size) {
        task->read_bytes += msg->size;
        if (task->read_bytes >= task->file_size) {
            if (task->read_bytes == task->file_size) {
                if (xfers->debug)
                    syslog(LOG_DEBUG, "file-xfer: task %u %s has completed",
                           task->id, task->file_name);
                close(task->file_fd);
                task->file_fd = -1;
                if (xfers->open_save_dir &&
                        task->file_xfer_nr == task->file_xfer_total &&
                        g_hash_table_size(xfers->xfers) == 1) {
                    char buf[PATH_MAX];
                    snprintf(buf, PATH_MAX, "xdg-open '%s'&", xfers->save_dir);
                    status = system(buf);
                }
                status = VD_AGENT_FILE_XFER_STATUS_SUCCESS;
            } else {
                syslog(LOG_ERR, "file-xfer: error received too much data");
                status = VD_AGENT_FILE_XFER_STATUS_ERROR;
            }
        }
    } else {
        syslog(LOG_ERR, "file-xfer: error writing %s: %s", task->file_name,
               strerror(errno));
        status = VD_AGENT_FILE_XFER_STATUS_ERROR;
    }

    if (status != -1) {
        udscs_write(xfers->vdagentd, VDAGENTD_FILE_XFER_STATUS,
                    msg->id, status, NULL, 0);
        g_hash_table_remove(xfers->xfers, GUINT_TO_POINTER(msg->id));
    }
}

void vdagent_file_xfers_error_disabled(struct udscs_connection *vdagentd, uint32_t msg_id)
{
    g_return_if_fail(vdagentd != NULL);

    udscs_write(vdagentd, VDAGENTD_FILE_XFER_STATUS,
                msg_id, VD_AGENT_FILE_XFER_STATUS_DISABLED, NULL, 0);
}

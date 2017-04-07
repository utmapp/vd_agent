/*  console-kit.c vdagentd ConsoleKit integration code

    Copyright 2010-2012 Red Hat, Inc.

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

#include "session-info.h"
#include <dbus/dbus.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <glib.h>

struct session_info {
    DBusConnection *connection;
    int fd;
    char *seat;
    char *active_session;
    int verbose;
    gchar *match_seat_signals;
    gchar *match_session_signals;
    gboolean session_is_locked;
    gboolean session_idle_hint;
};

#define INTERFACE_CONSOLE_KIT "org.freedesktop.ConsoleKit"
#define OBJ_PATH_CONSOLE_KIT  "/org/freedesktop/ConsoleKit"

#define INTERFACE_CONSOLE_KIT_MANAGER    INTERFACE_CONSOLE_KIT ".Manager"
#define OBJ_PATH_CONSOLE_KIT_MANAGER     OBJ_PATH_CONSOLE_KIT "/Manager"

#define INTERFACE_CONSOLE_KIT_SEAT       INTERFACE_CONSOLE_KIT ".Seat"

#define INTERFACE_CONSOLE_KIT_SESSION    INTERFACE_CONSOLE_KIT ".Session"
#define OBJ_PATH_CONSOLE_KIT_SESSION     OBJ_PATH_CONSOLE_KIT "/Session"

#define SEAT_SIGNAL_ACTIVE_SESSION_CHANGED       "ActiveSessionChanged"

#define SESSION_SIGNAL_LOCK                      "Lock"
#define SESSION_SIGNAL_UNLOCK                    "Unlock"
#define SESSION_SIGNAL_IDLE_HINT_CHANGED         "IdleHintChanged"

static char *console_kit_get_first_seat(struct session_info *info);
static char *console_kit_check_active_session_change(struct session_info *info);

static void si_dbus_match_remove(struct session_info *info)
{
    DBusError error;
    if (info->match_seat_signals != NULL) {
        dbus_error_init(&error);
        dbus_bus_remove_match(info->connection,
                              info->match_seat_signals,
                              &error);
        if (info->verbose)
            syslog(LOG_DEBUG, "(console-kit) seat match removed: %s",
                   info->match_seat_signals);
        g_free(info->match_seat_signals);
        info->match_seat_signals = NULL;
    }

    if (info->match_session_signals != NULL) {
        dbus_error_init(&error);
        dbus_bus_remove_match(info->connection,
                              info->match_session_signals,
                              &error);

        if (info->verbose)
            syslog(LOG_DEBUG, "(console-kit) session match removed: %s",
                   info->match_session_signals);
        g_free(info->match_session_signals);
        info->match_session_signals = NULL;
    }
}

static void si_dbus_match_rule_update(struct session_info *info)
{
    DBusError error;

    if (info->connection == NULL)
        return;

    si_dbus_match_remove(info);

    /* Seat signals */
    if (info->seat != NULL) {
        info->match_seat_signals =
            g_strdup_printf ("type='signal',interface='%s',path='%s',"
                             "member='ActiveSessionChanged'",
                             INTERFACE_CONSOLE_KIT_SEAT,
                             info->seat);
        if (info->verbose)
            syslog(LOG_DEBUG, "(console-kit) seat match: %s",
                   info->match_seat_signals);

        dbus_error_init(&error);
        dbus_bus_add_match(info->connection,
                           info->match_seat_signals,
                           &error);
        if (dbus_error_is_set(&error)) {
            syslog(LOG_WARNING, "Unable to add dbus rule match: %s",
                   error.message);
            dbus_error_free(&error);
            g_free(info->match_seat_signals);
            info->match_seat_signals = NULL;
        }
    }

    /* Session signals */
    if (info->active_session != NULL) {
        info->match_session_signals =
            g_strdup_printf ("type='signal',interface='%s',path='%s'",
                             INTERFACE_CONSOLE_KIT_SESSION,
                             info->active_session);
        if (info->verbose)
            syslog(LOG_DEBUG, "(console-kit) session match: %s",
                   info->match_session_signals);

        dbus_error_init(&error);
        dbus_bus_add_match(info->connection,
                           info->match_session_signals,
                           &error);
        if (dbus_error_is_set(&error)) {
            syslog(LOG_WARNING, "Unable to add dbus rule match: %s",
                   error.message);
            dbus_error_free(&error);
            g_free(info->match_session_signals);
            info->match_session_signals = NULL;
        }
    }
}

static void
si_dbus_read_signals(struct session_info *info)
{
    DBusMessage *message = NULL;

    dbus_connection_read_write(info->connection, 0);
    message = dbus_connection_pop_message(info->connection);
    while (message != NULL) {
        const char *member;

        member = dbus_message_get_member (message);
        if (g_strcmp0(member, SEAT_SIGNAL_ACTIVE_SESSION_CHANGED) == 0) {
            DBusMessageIter iter;
            gint type;
            gchar *session;

            free(info->active_session);
            info->active_session = NULL;

            dbus_message_iter_init(message, &iter);
            type = dbus_message_iter_get_arg_type(&iter);
            /* Session should be an object path, but there is a bug in
               ConsoleKit where it sends a string rather than an object_path
               accept object_path too in case the bug ever gets fixed */
            if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
                dbus_message_iter_get_basic(&iter, &session);
                if (session != NULL && session[0] != '\0') {
                    info->active_session = g_strdup(session);
                    si_dbus_match_rule_update(info);
                } else {
                    syslog(LOG_WARNING, "(console-kit) received invalid session. "
                           "No active-session at the moment");
                }
            } else {
                syslog(LOG_ERR,
                       "ActiveSessionChanged message has unexpected type: '%c'",
                       type);
            }
        } else if (g_strcmp0(member, SESSION_SIGNAL_LOCK) == 0) {
            info->session_is_locked = TRUE;
        } else if (g_strcmp0(member, SESSION_SIGNAL_UNLOCK) == 0) {
            info->session_is_locked = FALSE;
        } else if (g_strcmp0(member, SESSION_SIGNAL_IDLE_HINT_CHANGED) == 0) {
            DBusMessageIter iter;
            gint type;
            dbus_bool_t idle_hint;

            dbus_message_iter_init(message, &iter);
            type = dbus_message_iter_get_arg_type(&iter);
            if (type == DBUS_TYPE_BOOLEAN) {
                dbus_message_iter_get_basic(&iter, &idle_hint);
                info->session_idle_hint = (idle_hint);
            } else {
                syslog(LOG_ERR,
                       "(console-kit) IdleHintChanged has unexpected type: '%c'",
                       type);
            }
        } else {
            if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL) {
                syslog(LOG_WARNING, "(console-kit) received non signal message");
            } else if (info->verbose) {
                syslog(LOG_DEBUG, "(console-kit) Signal not handled: %s", member);
            }
        }

        dbus_message_unref(message);
        dbus_connection_read_write(info->connection, 0);
        message = dbus_connection_pop_message(info->connection);
    }
}

struct session_info *session_info_create(int verbose)
{
    struct session_info *info;
    DBusError error;

    info = calloc(1, sizeof(*info));
    if (!info)
        return NULL;

    info->verbose = verbose;
    info->session_is_locked = FALSE;
    info->session_idle_hint = FALSE;

    dbus_error_init(&error);
    info->connection = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
    if (info->connection == NULL || dbus_error_is_set(&error)) {
        if (dbus_error_is_set(&error)) {
             syslog(LOG_ERR, "Unable to connect to system bus: %s",
                    error.message);
             dbus_error_free(&error);
        } else
             syslog(LOG_ERR, "Unable to connect to system bus");
        free(info);
        return NULL;
    }

    if (!dbus_connection_get_unix_fd(info->connection, &info->fd)) {
        syslog(LOG_ERR, "Unable to get connection fd");
        session_info_destroy(info);
        return NULL;
    }

    if (!console_kit_get_first_seat(info)) {
        session_info_destroy(info);
        return NULL;
    }

    si_dbus_match_rule_update(info);
    return info;
}

void session_info_destroy(struct session_info *info)
{
    if (!info)
        return;

    si_dbus_match_remove(info);
    dbus_connection_close(info->connection);
    free(info->seat);
    free(info->active_session);
    free(info);
}

int session_info_get_fd(struct session_info *info)
{
    return info->fd;
}

static char *console_kit_get_first_seat(struct session_info *info)
{
    DBusError error;
    DBusMessage *message = NULL;
    DBusMessage *reply = NULL;
    DBusMessageIter iter, subiter;
    int type;
    char *seat = NULL;


    message = dbus_message_new_method_call(INTERFACE_CONSOLE_KIT,
                                           OBJ_PATH_CONSOLE_KIT_MANAGER,
                                           INTERFACE_CONSOLE_KIT_MANAGER,
                                           "GetSeats");
    if (message == NULL) {
        syslog(LOG_ERR, "Unable to create dbus message");
        goto exit;
    }

    dbus_error_init(&error);
    reply = dbus_connection_send_with_reply_and_block(info->connection,
                                                      message,
                                                      -1,
                                                      &error);
    if (reply == NULL || dbus_error_is_set(&error)) {
        if (dbus_error_is_set(&error)) {
            syslog(LOG_ERR, "GetSeats failed: %s", error.message);
            dbus_error_free(&error);
        } else
            syslog(LOG_ERR, "GetSeats failed");
        goto exit;
    }

    dbus_message_iter_init(reply, &iter);
    type = dbus_message_iter_get_arg_type(&iter);
    if (type != DBUS_TYPE_ARRAY) {
        syslog(LOG_ERR,
               "expected an array return value, got a '%c' instead", type);
        goto exit;
    }

    dbus_message_iter_recurse(&iter, &subiter);
    type = dbus_message_iter_get_arg_type(&subiter);
    if (type != DBUS_TYPE_OBJECT_PATH) {
        syslog(LOG_ERR,
               "expected an object path element, got a '%c' instead", type);
        goto exit;
    }

    dbus_message_iter_get_basic(&subiter, &seat);
    info->seat = strdup(seat);

exit:
    if (reply != NULL) {
            dbus_message_unref(reply);
    }

    if (message != NULL) {
            dbus_message_unref(message);
    }

    syslog(LOG_INFO, "(console-kit) seat: %s", info->seat);
    return info->seat;
}

const char *session_info_get_active_session(struct session_info *info)
{
    DBusError error;
    DBusMessage *message = NULL;
    DBusMessage *reply = NULL;
    char *session = NULL;

    if (!info)
        return NULL;

    if (info->active_session)
        return console_kit_check_active_session_change(info);

    message = dbus_message_new_method_call(INTERFACE_CONSOLE_KIT,
                                           info->seat,
                                           INTERFACE_CONSOLE_KIT_SEAT,
                                           "GetActiveSession");
    if (message == NULL) {
        syslog(LOG_ERR, "Unable to create dbus message");
        goto exit;
    }

    dbus_error_init(&error);
    reply = dbus_connection_send_with_reply_and_block(info->connection,
                                                      message,
                                                      -1,
                                                      &error);
    if (reply == NULL || dbus_error_is_set(&error)) {
        if (dbus_error_is_set(&error)) {
            syslog(LOG_ERR, "GetActiveSession failed: %s", error.message);
            dbus_error_free(&error);
        } else
            syslog(LOG_ERR, "GetActiveSession failed");
        goto exit;
    }

    dbus_error_init(&error);
    if (!dbus_message_get_args(reply,
                               &error,
                               DBUS_TYPE_OBJECT_PATH, &session,
                               DBUS_TYPE_INVALID)) {
        if (dbus_error_is_set(&error)) {
            syslog(LOG_ERR, "error get ssid from reply: %s", error.message);
            dbus_error_free(&error);
        } else
            syslog(LOG_ERR, "error getting ssid from reply");
        session = NULL;
        goto exit;
    }

    info->active_session = strdup(session);
    si_dbus_match_rule_update(info);

exit:
    if (reply != NULL) {
            dbus_message_unref(reply);
    }

    if (message != NULL) {
            dbus_message_unref(message);
    }

    /* In case the session was changed while we were running */
    return console_kit_check_active_session_change(info);
}

char *session_info_session_for_pid(struct session_info *info, uint32_t pid)
{
    DBusError error;
    DBusMessage *message = NULL;
    DBusMessage *reply = NULL;
    DBusMessageIter args;
    char *ssid = NULL;

    if (!info)
        return NULL;

    message = dbus_message_new_method_call(INTERFACE_CONSOLE_KIT,
                                           OBJ_PATH_CONSOLE_KIT_MANAGER,
                                           INTERFACE_CONSOLE_KIT_MANAGER,
                                           "GetSessionForUnixProcess");
    if (message == NULL) {
        syslog(LOG_ERR, "Unable to create dbus message");
        goto exit;
    }

    dbus_message_iter_init_append(message, &args);
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &pid)) {
        syslog(LOG_ERR, "Unable to append dbus message args");
        goto exit;
    }

    dbus_error_init(&error);
    reply = dbus_connection_send_with_reply_and_block(info->connection,
                                                      message,
                                                      -1,
                                                      &error);
    if (reply == NULL || dbus_error_is_set(&error)) {
        if (dbus_error_is_set(&error)) {
            syslog(LOG_ERR, "GetSessionForUnixProcess failed: %s",
                   error.message);
            dbus_error_free(&error);
        } else
            syslog(LOG_ERR, "GetSessionForUnixProcess failed");
        goto exit;
    }

    dbus_error_init(&error);
    if (!dbus_message_get_args(reply,
                               &error,
                               DBUS_TYPE_OBJECT_PATH, &ssid,
                               DBUS_TYPE_INVALID)) {
        if (dbus_error_is_set(&error)) {
            syslog(LOG_ERR, "error get ssid from reply: %s", error.message);
            dbus_error_free(&error);
        } else
            syslog(LOG_ERR, "error getting ssid from reply");
        ssid = NULL;
        goto exit;
    }

    ssid = strdup(ssid);

exit:
    if (reply != NULL) {
            dbus_message_unref(reply);
    }

    if (message != NULL) {
            dbus_message_unref(message);
    }

    return ssid;
}

static char *console_kit_check_active_session_change(struct session_info *info)
{
    si_dbus_read_signals(info);
    if (info->verbose)
        syslog(LOG_DEBUG, "(console-kit) active-session: '%s'",
               (info->active_session ? info->active_session : "None"));

    return info->active_session;
}

gboolean session_info_session_is_locked(struct session_info *info)
{
    gboolean locked;

    g_return_val_if_fail (info != NULL, FALSE);

    /* Not every system does emit Lock and Unlock signals (for instance, such
     * is the case for RHEL6) but most of the systems seems to emit the
     * IdleHintChanged. So use the IdleHint value.
     * systemd-login uses locked-hint which is not implemented in ConsoleKit,
     * see https://github.com/ConsoleKit2/ConsoleKit2/issues/89 */
    si_dbus_read_signals(info);
    locked = info->session_idle_hint;
    if (info->verbose) {
        syslog(LOG_DEBUG, "(console-kit) session is locked: %s",
               locked ? "yes" : "no");
    }
    return locked;
}

/* This function should only be called after session_info_get_active_session
 * in order to verify if active session belongs to user (non greeter) */
gboolean session_info_is_user(struct session_info *info)
{
    DBusError error;
    DBusMessage *message = NULL;
    DBusMessage *reply = NULL;
    gchar *session_type = NULL;
    gboolean ret = TRUE;

    g_return_val_if_fail (info != NULL, TRUE);
    g_return_val_if_fail (info->connection != NULL, TRUE);
    g_return_val_if_fail (info->active_session != NULL, TRUE);

    message = dbus_message_new_method_call(INTERFACE_CONSOLE_KIT,
                                           info->active_session,
                                           INTERFACE_CONSOLE_KIT_SESSION,
                                           "GetSessionType");
    if (message == NULL) {
        syslog(LOG_ERR,
               "(console-kit) Unable to create dbus message for GetSessionType");
        return TRUE;
    }

    dbus_error_init(&error);
    reply = dbus_connection_send_with_reply_and_block(info->connection,
                                                      message,
                                                      -1,
                                                      &error);
    if (reply == NULL || dbus_error_is_set(&error)) {
        if (dbus_error_is_set(&error)) {
            syslog(LOG_ERR, "GetSessionType failed: %s", error.message);
            dbus_error_free(&error);
        } else
            syslog(LOG_ERR, "GetSessionType failed");
        goto exit;
    }

    dbus_error_init(&error);
    if (!dbus_message_get_args(reply,
                               &error,
                               DBUS_TYPE_STRING, &session_type,
                               DBUS_TYPE_INVALID)) {
        if (dbus_error_is_set(&error)) {
            syslog(LOG_ERR,
                   "(console-kit) fail to get session-type from reply: %s",
                   error.message);
            dbus_error_free(&error);
        } else {
            syslog(LOG_ERR, "(console-kit) fail to get session-type from reply");
        }
        session_type = NULL;
        goto exit;
    }

    /* Empty session_type means user */
    if (info->verbose)
        syslog(LOG_DEBUG, "(console-kit) session-type is '%s'", session_type);

    ret = (g_strcmp0 (session_type, "LoginWindow") != 0);

exit:
    if (reply != NULL) {
        dbus_message_unref(reply);
    }
    if (message != NULL) {
        dbus_message_unref(message);
    }
    return ret;
}

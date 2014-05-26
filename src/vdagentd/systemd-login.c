/*  systemd-login.c vdagentd libsystemd-login integration code

    Copyright 2012 Red Hat, Inc.

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
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <systemd/sd-login.h>
#include <dbus/dbus.h>

struct session_info {
    int verbose;
    sd_login_monitor *mon;
    char *session;
    struct {
        DBusConnection *system_connection;
        char *match_session_signals;
    } dbus;
    gboolean session_is_locked;
    gboolean session_locked_hint;
};

#define LOGIND_INTERFACE            "org.freedesktop.login1"

#define LOGIND_SESSION_INTERFACE    "org.freedesktop.login1.Session"
#define LOGIND_SESSION_OBJ_TEMPLATE "/org/freedesktop/login1/session/_3%s"

#define DBUS_PROPERTIES_INTERFACE   "org.freedesktop.DBus.Properties"

#define SESSION_SIGNAL_LOCK         "Lock"
#define SESSION_SIGNAL_UNLOCK       "Unlock"

#define SESSION_PROP_LOCKED_HINT    "LockedHint"

/* dbus related */
static DBusConnection *si_dbus_get_system_bus(void)
{
    DBusConnection *connection;
    DBusError error;

    dbus_error_init(&error);
    connection = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
    if (connection == NULL || dbus_error_is_set(&error)) {
        if (dbus_error_is_set(&error)) {
            syslog(LOG_WARNING, "Unable to connect to system bus: %s",
                   error.message);
            dbus_error_free(&error);
        } else {
            syslog(LOG_WARNING, "Unable to connect to system bus");
        }
        return NULL;
    }
    return connection;
}

static void si_dbus_match_remove(struct session_info *si)
{
    DBusError error;
    if (si->dbus.match_session_signals == NULL)
        return;

    dbus_error_init(&error);
    dbus_bus_remove_match(si->dbus.system_connection,
                          si->dbus.match_session_signals,
                          &error);

    g_free(si->dbus.match_session_signals);
    si->dbus.match_session_signals = NULL;
}

static void si_dbus_match_rule_update(struct session_info *si)
{
    DBusError error;

    if (si->dbus.system_connection == NULL ||
            si->session == NULL)
        return;

    si_dbus_match_remove(si);

    si->dbus.match_session_signals =
        g_strdup_printf ("type='signal',interface='%s',path='"
                         LOGIND_SESSION_OBJ_TEMPLATE"'",
                         LOGIND_SESSION_INTERFACE,
                         si->session);
    if (si->verbose)
        syslog(LOG_DEBUG, "logind match: %s", si->dbus.match_session_signals);

    dbus_error_init(&error);
    dbus_bus_add_match(si->dbus.system_connection,
                       si->dbus.match_session_signals,
                       &error);
    if (dbus_error_is_set(&error)) {
        syslog(LOG_WARNING, "Unable to add dbus rule match: %s",
               error.message);
        dbus_error_free(&error);
        g_free(si->dbus.match_session_signals);
        si->dbus.match_session_signals = NULL;
    }
}

static void
si_dbus_read_properties(struct session_info *si)
{
    dbus_bool_t locked_hint, ret;
    DBusMessageIter iter, iter_variant;
    gint type;
    DBusError error;
    DBusMessage *message = NULL;
    DBusMessage *reply = NULL;
    gchar *session_object;
    const gchar *interface, *property;

    if (si->session == NULL)
        return;

    session_object = g_strdup_printf(LOGIND_SESSION_OBJ_TEMPLATE, si->session);
    message = dbus_message_new_method_call(LOGIND_INTERFACE,
                                           session_object,
                                           DBUS_PROPERTIES_INTERFACE,
                                           "Get");
    g_free (session_object);
    if (message == NULL) {
        syslog(LOG_ERR, "Unable to create dbus message");
        goto exit;
    }

    interface = LOGIND_SESSION_INTERFACE;
    property = SESSION_PROP_LOCKED_HINT;
    ret = dbus_message_append_args(message,
                                   DBUS_TYPE_STRING, &interface,
                                   DBUS_TYPE_STRING, &property,
                                   DBUS_TYPE_INVALID);
    if (!ret) {
        syslog(LOG_ERR, "Unable to request locked-hint");
        goto exit;
    }

    dbus_error_init(&error);
    reply = dbus_connection_send_with_reply_and_block(si->dbus.system_connection,
                                                      message,
                                                      -1,
                                                      &error);
    if (reply == NULL) {
        if (dbus_error_is_set(&error)) {
            syslog(LOG_ERR, "Properties.Get failed (locked-hint) due %s", error.message);
            dbus_error_free(&error);
        } else {
            syslog(LOG_ERR, "Properties.Get failed (locked-hint)");
        }
        goto exit;
    }

    dbus_message_iter_init(reply, &iter);
    type = dbus_message_iter_get_arg_type(&iter);
    if (type != DBUS_TYPE_VARIANT) {
        syslog(LOG_ERR, "expected a variant, got a '%c' instead", type);
        goto exit;
    }

    dbus_message_iter_recurse(&iter, &iter_variant);
    type = dbus_message_iter_get_arg_type(&iter_variant);
    if (type != DBUS_TYPE_BOOLEAN) {
        syslog(LOG_ERR, "expected a boolean, got a '%c' instead", type);
        goto exit;
    }
    dbus_message_iter_get_basic(&iter_variant, &locked_hint);

    si->session_locked_hint = (locked_hint) ? TRUE : FALSE;
exit:
    if (reply != NULL) {
        dbus_message_unref(reply);
    }

    if (message != NULL) {
        dbus_message_unref(message);
    }
}

static void
si_dbus_read_signals(struct session_info *si)
{
    DBusMessage *message = NULL;

    dbus_connection_read_write(si->dbus.system_connection, 0);
    message = dbus_connection_pop_message(si->dbus.system_connection);
    while (message != NULL) {
        const char *member;

        member = dbus_message_get_member (message);
        if (g_strcmp0(member, SESSION_SIGNAL_LOCK) == 0) {
            si->session_is_locked = TRUE;
        } else if (g_strcmp0(member, SESSION_SIGNAL_UNLOCK) == 0) {
            si->session_is_locked = FALSE;
        } else {
            if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL) {
                syslog(LOG_WARNING, "(systemd-login) received non signal message");
            } else if (si->verbose) {
                syslog(LOG_DEBUG, "(systemd-login) Signal not handled: %s", member);
            }
        }

        dbus_message_unref(message);
        dbus_connection_read_write(si->dbus.system_connection, 0);
        message = dbus_connection_pop_message(si->dbus.system_connection);
    }
}

struct session_info *session_info_create(int verbose)
{
    struct session_info *si;
    int r;

    si = calloc(1, sizeof(*si));
    if (!si)
        return NULL;

    si->verbose = verbose;
    si->session_is_locked = FALSE;

    r = sd_login_monitor_new("session", &si->mon);
    if (r < 0) {
        syslog(LOG_ERR, "Error creating login monitor: %s", strerror(-r));
        free(si);
        return NULL;
    }

    si->dbus.system_connection = si_dbus_get_system_bus();
    return si;
}

void session_info_destroy(struct session_info *si)
{
    if (!si)
        return;

    si_dbus_match_remove(si);
    dbus_connection_close(si->dbus.system_connection);
    sd_login_monitor_unref(si->mon);
    free(si->session);
    free(si);
}

int session_info_get_fd(struct session_info *si)
{
    return sd_login_monitor_get_fd(si->mon);
}

const char *session_info_get_active_session(struct session_info *si)
{
    int r;
    char *old_session = si->session;

    si->session = NULL;
    r = sd_seat_get_active("seat0", &si->session, NULL);
    /* ENOENT happens when a seat is switching from one session to another */
    if (r < 0 && r != -ENOENT)
        syslog(LOG_ERR, "Error getting active session: %s",
                strerror(-r));

    if (si->verbose && si->session &&
            (!old_session || strcmp(old_session, si->session)))
        syslog(LOG_INFO, "Active session: %s", si->session);

    sd_login_monitor_flush(si->mon);
    free(old_session);

    si_dbus_match_rule_update(si);
    return si->session;
}

char *session_info_session_for_pid(struct session_info *si, uint32_t pid)
{
    int r;
    char *session = NULL;

    r = sd_pid_get_session(pid, &session);
    if (r < 0)
        syslog(LOG_ERR, "Error getting session for pid %u: %s",
                pid, strerror(-r));
    else if (si->verbose)
        syslog(LOG_INFO, "Session for pid %u: %s", pid, session);

    return session;
}

gboolean session_info_session_is_locked(struct session_info *si)
{
    gboolean locked;

    g_return_val_if_fail (si != NULL, FALSE);

    si_dbus_read_signals(si);
    si_dbus_read_properties(si);

    locked = (si->session_is_locked || si->session_locked_hint);
    if (si->verbose) {
        syslog(LOG_DEBUG, "(systemd-login) session is locked: %s",
               locked ? "yes" : "no");
    }
    return locked;
}

/* This function should only be called after session_info_get_active_session
 * in order to verify if active session belongs to user (non greeter) */
gboolean session_info_is_user(struct session_info *si)
{
    gchar *class = NULL;
    gboolean ret;

    g_return_val_if_fail (si != NULL, TRUE);
    g_return_val_if_fail (si->session != NULL, TRUE);

    if (sd_session_get_class(si->session, &class) != 0) {
        syslog(LOG_WARNING, "Unable to get class from session: %s",
               si->session);
        return TRUE;
    }

    if (si->verbose)
        syslog(LOG_DEBUG, "(systemd-login) class for %s is %s",
               si->session, class);

    ret = (g_strcmp0(class, "user") == 0);
    g_free(class);

    return ret;
}

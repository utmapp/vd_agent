/* mutter.c - implements the DBUS interface to mutter

 Copyright 2020 Red Hat, Inc.

 Red Hat Authors:
 Julien Ropé <jrope@redhat.com>

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

#include <glib.h>
#include <gio/gio.h>

#include <syslog.h>

#include "vdagentd-proto.h"
#include "mutter.h"

// MUTTER DBUS FORMAT STRINGS
#define MODE_BASE_FORMAT "siiddad"
#define MODE_FORMAT "(" MODE_BASE_FORMAT "a{sv})"
#define MODES_FORMAT "a" MODE_FORMAT
#define MONITOR_SPEC_FORMAT "(ssss)"
#define MONITOR_FORMAT "(" MONITOR_SPEC_FORMAT MODES_FORMAT "a{sv})"
#define MONITORS_FORMAT "a" MONITOR_FORMAT

#define LOGICAL_MONITOR_MONITORS_FORMAT "a" MONITOR_SPEC_FORMAT
#define LOGICAL_MONITOR_FORMAT "(iidub" LOGICAL_MONITOR_MONITORS_FORMAT "a{sv})"
#define LOGICAL_MONITORS_FORMAT "a" LOGICAL_MONITOR_FORMAT

#define CURRENT_STATE_FORMAT "(u" MONITORS_FORMAT LOGICAL_MONITORS_FORMAT "a{sv})"


struct VDAgentMutterDBus {
    GDBusProxy *dbus_proxy;
    GHashTable *connector_mapping;
};

/**
 * Initialise a communication to Mutter through its DBUS interface.
 *
 * Errors can indicate that another compositor is used. This is not a blocker, and we should default
 * to use a different API then.
 *
 * Returns:
 * An initialise VDAgentMutterDBus structure if successful.
 * NULL if an error occured.
 */
VDAgentMutterDBus *vdagent_mutter_create(GHashTable *connector_mapping)
{
    GError *error = NULL;
    VDAgentMutterDBus *mutter = g_new0(VDAgentMutterDBus, 1);

    mutter->connector_mapping = g_hash_table_ref(connector_mapping);

    GDBusProxyFlags flags = (G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START
                            | G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES
                            | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS);

    mutter->dbus_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
                                                       flags,
                                                       NULL,
                                                       "org.gnome.Mutter.DisplayConfig",
                                                       "/org/gnome/Mutter/DisplayConfig",
                                                       "org.gnome.Mutter.DisplayConfig",
                                                       NULL,
                                                       &error);
    if (!mutter->dbus_proxy) {
        syslog(LOG_WARNING, "display: failed to create dbus proxy: %s", error->message);
        g_clear_error(&error);
        vdagent_mutter_destroy(mutter);
        return NULL;
    }

    return mutter;
}


void vdagent_mutter_destroy(VDAgentMutterDBus *mutter)
{
    g_clear_object(&mutter->dbus_proxy);
    g_hash_table_unref(mutter->connector_mapping);
    g_free(mutter);
}

/** Look through a list of logical monitor to find the one provided.
 *  Returns the corresponding x and y position of the monitor on the desktop.
 *  This function is a helper to vdagent_mutter_get_resolution().
 *
 *  Parameters:
 *  logical_monitor: initialized GVariant iterator. It will be copied to look through the items
 *                   so that its original position is not modified.
 *  connector: name of the connector that must be found
 *  x and y: will received the found position
 *
 */
static void vdagent_mutter_get_monitor_position(GVariantIter *logical_monitors,
                                                const gchar *connector, int *x, int *y)
{
    GVariant *logical_monitor = NULL;
    GVariantIter *logical_monitor_iterator = g_variant_iter_copy(logical_monitors);
    while (g_variant_iter_next(logical_monitor_iterator, "@"LOGICAL_MONITOR_FORMAT,
                               &logical_monitor)) {
        GVariantIter *tmp_monitors = NULL;

        g_variant_get_child(logical_monitor, 0, "i", x);
        g_variant_get_child(logical_monitor, 1, "i", y);
        g_variant_get_child(logical_monitor, 5, LOGICAL_MONITOR_MONITORS_FORMAT, &tmp_monitors);

        g_variant_unref(logical_monitor);

        GVariant *tmp_monitor = NULL;
        gboolean found = FALSE;
        while (!found && g_variant_iter_next(tmp_monitors, "@"MONITOR_SPEC_FORMAT, &tmp_monitor)) {
            const gchar *tmp_connector;

            g_variant_get_child(tmp_monitor, 0, "&s", &tmp_connector);

            if (g_strcmp0(connector, tmp_connector) == 0) {
                found = TRUE;
            }
            g_variant_unref(tmp_monitor);
        }

        g_variant_iter_free(tmp_monitors);

        if (found) {
            break;
        }
        *x = *y = 0;
    }
    g_variant_iter_free(logical_monitor_iterator);
}

GArray *vdagent_mutter_get_resolutions(VDAgentMutterDBus *mutter,
                                       int *desktop_width, int *desktop_height, int *screen_count)
{
    GError *error = NULL;
    GArray *res_array = NULL;

    // keep track of monitors we find and are not mapped to SPICE displays
    // we will map them back later (assuming display ID == monitor index)
    // this prevents the need from looping twice on all DBUS items
    GArray *not_found_array = NULL;

    if (!mutter) {
        return res_array;
    }

    GVariant *values = g_dbus_proxy_call_sync(mutter->dbus_proxy,
                                              "GetCurrentState",
                                              NULL,
                                              G_DBUS_CALL_FLAGS_NONE,
                                              -1,   // use proxy default timeout
                                              NULL,
                                              &error);
    if (!values) {
        syslog(LOG_WARNING, "display: failed to call GetCurrentState from mutter over DBUS");
        if (error != NULL) {
            syslog(LOG_WARNING, "   error message: %s", error->message);
            g_clear_error(&error);
        }
        return res_array;
    }

    res_array = g_array_new(FALSE, FALSE, sizeof(struct vdagentd_guest_xorg_resolution));
    not_found_array = g_array_new(FALSE, FALSE, sizeof(struct vdagentd_guest_xorg_resolution));

    GVariantIter *monitors = NULL;
    GVariantIter *logical_monitors = NULL;

    g_variant_get_child(values, 1, MONITORS_FORMAT, &monitors);
    g_variant_get_child(values, 2, LOGICAL_MONITORS_FORMAT, &logical_monitors);

    // list monitors
    GVariant *monitor = NULL;
    *screen_count = g_variant_iter_n_children(monitors);

    while (g_variant_iter_next(monitors, "@"MONITOR_FORMAT, &monitor)) {

        const gchar *connector = NULL;
        GVariantIter *modes = NULL;
        GVariant *monitor_specs = NULL;

        g_variant_get_child(monitor, 0, "@"MONITOR_SPEC_FORMAT, &monitor_specs);
        g_variant_get_child(monitor_specs, 0, "&s", &connector);
        g_variant_get_child(monitor, 1, MODES_FORMAT, &modes);

        g_variant_unref(monitor_specs);
        g_variant_unref(monitor);

        // list modes
        GVariant *mode = NULL;
        while (g_variant_iter_next(modes, "@"MODE_FORMAT, &mode)) {
            GVariant *properties = NULL;
            gboolean is_current;

            g_variant_get_child(mode, 6, "@a{sv}", &properties);
            if (!g_variant_lookup(properties, "is-current", "b", &is_current)) {
                is_current = FALSE;
            }
            g_variant_unref(properties);

            if (!is_current) {
                g_variant_unref(mode);
                continue;
            }

            struct vdagentd_guest_xorg_resolution curr;
            vdagent_mutter_get_monitor_position(logical_monitors, connector, &curr.x, &curr.y);
            g_variant_get_child(mode, 1, "i", &curr.width);
            g_variant_get_child(mode, 2, "i", &curr.height);
            g_variant_unref(mode);

            // compute the size of the desktop based on the dimension of the monitors
            if (curr.x + curr.width > *desktop_width) {
                *desktop_width = curr.x + curr.width;
            }
            if (curr.y + curr.height > *desktop_height) {
                *desktop_height = curr.y + curr.height;
            }

            gpointer value;
            if (g_hash_table_lookup_extended(mutter->connector_mapping, connector, NULL, &value)) {
                curr.display_id = GPOINTER_TO_UINT(value);
                syslog(LOG_DEBUG,
                       "Found monitor %s with geometry %dx%d+%d-%d - associating it to SPICE display #%d",
                       connector, curr.width, curr.height, curr.x, curr.y, curr.display_id);
                g_array_append_val(res_array, curr);
            } else {
                syslog(LOG_DEBUG, "No SPICE display found for connector %s", connector);
                g_array_append_val(not_found_array, curr);
            }

            break;
        }
        g_variant_iter_free(modes);
    }

    g_variant_iter_free(logical_monitors);
    g_variant_iter_free(monitors);

    int i;

    if (res_array->len == 0) {
        syslog(LOG_DEBUG, "%s: No Spice display ID matching - assuming display ID == Monitor index",
                __FUNCTION__);
        g_array_free(res_array, TRUE);
        res_array = not_found_array;

        struct vdagentd_guest_xorg_resolution *res;
        res = (struct vdagentd_guest_xorg_resolution*)res_array->data;
        for (i = 0; i < res_array->len; i++) {
            res[i].display_id = i;
        }
    }
    else {
        g_array_free(not_found_array, TRUE);
    }

    g_variant_unref(values);
    return res_array;
}

/*  display.c vdagent display source code

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
#ifdef WITH_GTK
#include <gdk/gdk.h>
#include <gtk/gtk.h>    // for GTK_CHECK_VERSION
#if GTK_CHECK_VERSION(3, 98, 0)
    #include <gdk/wayland/gdkwayland.h>
    #include <gdk/x11/gdkx.h>
#else
    #ifdef GDK_WINDOWING_X11
         #include <gdk/gdkx.h>
    #endif
#endif
#endif
#include <syslog.h>
#include "x11.h"
#include "x11-priv.h"

#include "device-info.h"
#include "vdagentd-proto.h"

#include "mutter.h"
#include "display.h"

/**
 * VDAgentDisplay and the vdagent_display_*() functions are used as wrappers for display-related
 * operations.
 * They allow vdagent code to call generic display functions that are independent from the underlying
 * API (X11/GTK/etc).
 *
 * The display.c file contains the actual implementation and chooses what API will be called.
 * The x11.c and x11-randr.c files contains the x11-specific functions.
 */
struct VDAgentDisplay {
    // association between SPICE display ID and expected connector name
    GHashTable *connector_mapping;
    struct vdagent_x11 *x11;
    UdscsConnection *vdagentd;
    int debug;
    GIOChannel *x11_channel;
    VDAgentMutterDBus *mutter;
};

static gint vdagent_guest_xorg_resolution_compare(gconstpointer a, gconstpointer b)
{
    struct vdagentd_guest_xorg_resolution *ptr_a, *ptr_b;

    ptr_a = (struct vdagentd_guest_xorg_resolution *)a;
    ptr_b = (struct vdagentd_guest_xorg_resolution *)b;

    return ptr_a->display_id - ptr_b->display_id;
}

static GArray *vdagent_gtk_get_resolutions(VDAgentDisplay *display,
                                           int *width, int *height, int *screen_count)
{
#ifdef USE_GTK_FOR_MONITORS
    if (!GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default())) {
        return NULL;
    }

    GArray *res_array = g_array_new(FALSE, FALSE, sizeof(struct vdagentd_guest_xorg_resolution));
    int i;
    GListModel *monitors = NULL;

    GdkDisplay *gdk_display = gdk_display_get_default();

    // Make sure GDK is aware of the changes we want to send.
    // TODO: This may be removed if we get a notification of change from GDK itself,
    // but with X11 notification, we end up sending obsolete information.
    gdk_display_sync(gdk_display);

    monitors = gdk_display_get_monitors(gdk_display);
    *screen_count = g_list_model_get_n_items(monitors);

    for (i = 0; i < *screen_count; i++) {
        struct vdagentd_guest_xorg_resolution curr;

        GdkMonitor *monitor = (GdkMonitor*)g_list_model_get_item(monitors, i);
        GdkRectangle geometry;

        gdk_monitor_get_geometry(monitor, &geometry);
        curr.x = geometry.x;
        curr.y = geometry.y;
        curr.height = geometry.height;
        curr.width = geometry.width;

        // compute the size of the desktop based on the dimension of the monitors
        // TODO: check for a specific API giving us that information (not found in GTK ?)
        if (curr.x + curr.width > *width) {
            *width = curr.x + curr.width;
        }
        if (curr.y + curr.height > *height) {
            *height = curr.y + curr.height;
        }

        // retrieve the Spice Display ID based on the connector name
        const char *name = gdk_monitor_get_connector(monitor);
        if (!name) {
            syslog(LOG_WARNING, "Unknown connector for monitor %d", i);
            continue;
        }

        gpointer value;
        if (g_hash_table_lookup_extended(display->connector_mapping, name, NULL, &value)) {
            curr.display_id = GPOINTER_TO_UINT(value);
            syslog(LOG_DEBUG, "Found monitor %s with geometry %dx%d+%d-%d - associating it to SPICE display #%d",
                   name, curr.width, curr.height, curr.x, curr.y, curr.display_id);
            g_array_append_val(res_array, curr);
        } else {
            syslog(LOG_DEBUG, "No SPICE display found for connector %s", name);
        }
    }

    if (res_array->len == 0) {
        syslog(LOG_DEBUG, "No Spice display ID matching - assuming display ID == Monitor index");
        for (i = 0; i < *screen_count; i++) {
            struct vdagentd_guest_xorg_resolution res;
            GdkMonitor *monitor = (GdkMonitor*)g_list_model_get_item(monitors, i);
            GdkRectangle geometry;

            gdk_monitor_get_geometry(monitor, &geometry);
            res.x = geometry.x;
            res.y = geometry.y;
            res.height = geometry.height;
            res.width = geometry.width;
            res.display_id = i;

            g_array_append_val(res_array, res);
        }
    }

    return res_array;
#else
    return NULL;
#endif
}

void vdagent_display_send_daemon_guest_res(VDAgentDisplay *display, gboolean update)
{
    GArray *res_array;
    int width = 0, height = 0, screen_count = 0;

    // Try various backends one after the other.
    // We try Mutter first, because it has a bigger probability of being available.
    // Second GTK, because if/when we build with GTK4, this is the one that will work best.
    // Finally we try X11. This is the default, and should work OK in most circumstances.
    res_array = vdagent_mutter_get_resolutions(display->mutter, &width, &height, &screen_count);

    if (res_array == NULL) {
        res_array = vdagent_gtk_get_resolutions(display, &width, &height, &screen_count);
    }

    if (res_array == NULL) {
        res_array = vdagent_x11_get_resolutions(display->x11, update, &width, &height, &screen_count);
    }

    if (res_array == NULL) {
        return;
    }

    if (res_array->len < g_hash_table_size(display->connector_mapping)) {
        // Complete the array with disabled displays.
        // We need to send 0x0 resolution to let the daemon know the display is not there anymore.

        syslog(LOG_DEBUG, "%d/%d displays found - completing with disabled displays.",
               res_array->len, g_hash_table_size(display->connector_mapping));

        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, display->connector_mapping);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            bool found = false;
            int display_id = GPOINTER_TO_INT(value);
            for (int i = 0; i < res_array->len; i++) {
                struct vdagentd_guest_xorg_resolution *res =
                    (struct vdagentd_guest_xorg_resolution*)res_array->data;
                if (res[i].display_id == display_id) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                struct vdagentd_guest_xorg_resolution res;

                res.x = 0;
                res.y = 0;
                res.height = 0;
                res.width = 0;
                res.display_id = display_id;

                g_array_append_val(res_array, res);
            }
        }
    }

    // sort the list to make sure we send them in the display_id order
    g_array_sort(res_array, vdagent_guest_xorg_resolution_compare);

    if (display->debug) {
        syslog(LOG_DEBUG, "Sending guest screen resolutions to vdagentd:");
        if (res_array->len > screen_count) {
            syslog(LOG_DEBUG, "(NOTE: list may contain overlapping areas when "
                              "multiple spice displays show the same guest output)");
        }
        struct vdagentd_guest_xorg_resolution *res =
            (struct vdagentd_guest_xorg_resolution*)res_array->data;
        for (int i = 0; i < res_array->len; i++) {
            syslog(LOG_DEBUG, "   display_id=%d - %dx%d%+d%+d",
                   res[i].display_id, res[i].width, res[i].height, res[i].x, res[i].y);
        }
    }

    udscs_write(display->vdagentd, VDAGENTD_GUEST_XORG_RESOLUTION, width, height,
                (uint8_t *)res_array->data,
                res_array->len * sizeof(struct vdagentd_guest_xorg_resolution));
    g_array_free(res_array, TRUE);
}

static gchar *vdagent_display_get_wm_name(VDAgentDisplay *display)
{
#ifdef GDK_WINDOWING_X11
    // With GTK4, screen have disappear, and with it the access to the window manager name
    // Use the X11 call instead.
#if ! GTK_CHECK_VERSION(3, 98, 0)
    GdkDisplay *gdk_display = gdk_display_get_default();
    if (GDK_IS_X11_DISPLAY(gdk_display))
        return g_strdup(gdk_x11_screen_get_window_manager_name(
            gdk_display_get_default_screen(gdk_display)));
    return g_strdup("unsupported");
#endif
#endif
    return vdagent_x11_get_wm_name(display->x11);
}


struct vdagent_x11* vdagent_display_get_x11(VDAgentDisplay *display)
{
    return display->x11;
}

static gboolean x11_io_channel_cb(GIOChannel *source, GIOCondition condition, gpointer data)
{
    VDAgentDisplay *display = data;
    vdagent_x11_do_read(display->x11);

    return G_SOURCE_CONTINUE;
}

VDAgentDisplay* vdagent_display_create(UdscsConnection *vdagentd, int debug, int sync)
{
    VDAgentDisplay *display;
    gchar *net_wm_name = NULL;

    display = g_new0(VDAgentDisplay, 1);
    display->vdagentd = vdagentd;
    display->debug = debug;

    display->x11 = vdagent_x11_create(vdagentd, debug, sync);
    if (display->x11 == NULL) {
        g_free(display);
        return NULL;
    }

    display->x11->vdagent_display = display;
    display->connector_mapping = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    display->mutter = vdagent_mutter_create(display->connector_mapping);

    display->x11_channel = g_io_channel_unix_new(vdagent_x11_get_fd(display->x11));
    if (display->x11_channel == NULL) {
        vdagent_x11_destroy(display->x11, TRUE);
        g_free(display);
        return NULL;
    }

    g_io_add_watch(display->x11_channel, G_IO_IN, x11_io_channel_cb, display);


    /* Since we are started at the same time as the wm,
       sometimes we need to wait a bit for the _NET_WM_NAME to show up. */
    for (int i = 0; i < 9; i++) {
        g_free(net_wm_name);
        net_wm_name = vdagent_display_get_wm_name(display);
        if (strcmp(net_wm_name, "unknown"))
            break;
        usleep(100000);
    }
    if (display->debug)
        syslog(LOG_DEBUG, "%s: net_wm_name=\"%s\", has icons=%d",
               __func__, net_wm_name, vdagent_display_has_icons_on_desktop(display));
    g_free(net_wm_name);

    vdagent_display_send_daemon_guest_res(display, TRUE);
    return display;
}

void vdagent_display_destroy(VDAgentDisplay *display, int vdagentd_disconnected)
{
    if (!display) {
        return;
    }


    g_clear_pointer(&display->x11_channel, g_io_channel_unref);
    vdagent_x11_destroy(display->x11, vdagentd_disconnected);

    vdagent_mutter_destroy(display->mutter);

    g_hash_table_destroy(display->connector_mapping);
    g_free(display);
}

/* Function used to determine the default location to save file-xfers,
   xdg desktop dir or xdg download dir. We err on the safe side and use a
   whitelist approach, so any unknown desktop will end up with saving
   file-xfers to the xdg download dir, and opening the xdg download dir with
   xdg-open when the file-xfer completes. */
gboolean vdagent_display_has_icons_on_desktop(VDAgentDisplay *display)
{
    static const char * const wms_with_icons_on_desktop[] = {
        "Metacity", /* GNOME-2 or GNOME-3 fallback */
        "Xfwm4",    /* Xfce */
        "Marco",    /* Mate */
        "Metacity (Marco)", /* Mate, newer */
        NULL
    };
    gchar *net_wm_name = vdagent_display_get_wm_name(display);
    int i;

    for (i = 0; wms_with_icons_on_desktop[i]; i++)
        if (!strcmp(net_wm_name, wms_with_icons_on_desktop[i])) {
            g_free(net_wm_name);
            return TRUE;
        }

    g_free(net_wm_name);
    return FALSE;
}

static bool has_zero_based_display_id(VDAgentDisplay *display)
{
    // Older QXL drivers numbered their outputs starting with
    // 0. This contrasts with most drivers who start numbering
    // outputs with 1.  In this case, the expected drm connector
    // name will need to be decremented before comparing to the
    // display manager output name
    bool ret = false;
#ifdef USE_GTK_FOR_MONITORS
    GdkDisplay *gdk_display = gdk_display_get_default();
    if (GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
        gdk_display_sync(gdk_display);

        GListModel *monitors = gdk_display_get_monitors(gdk_display);
        int screen_count = g_list_model_get_n_items(monitors);
        for (int i = 0; i < screen_count; i++) {
            GdkMonitor *monitor = (GdkMonitor *)g_list_model_get_item(monitors, i);
            const char *name = gdk_monitor_get_connector(monitor);
            if (!name) {
                continue;
            }

            if (strcmp(name, "Virtual-0") == 0) {
                ret = true;
                break;
            }
        }
    }
    else // otherwise, use the X11 code (below)
#endif
    {
        XRRScreenResources *xres = display->x11->randr.res;
        Display *xdisplay = display->x11->display;
        for (int i = 0; i < xres->noutput; ++i) {
            XRROutputInfo *oinfo = XRRGetOutputInfo(xdisplay, xres, xres->outputs[i]);
            if (!oinfo) {
                syslog(LOG_WARNING, "Unable to lookup XRandr output info for output %li",
                       xres->outputs[i]);
                return false;
            }
            if (strcmp(oinfo->name, "Virtual-0") == 0) {
                ret = true;
                XRRFreeOutputInfo(oinfo);
                break;
            }
            XRRFreeOutputInfo(oinfo);
        }
    }
    return ret;
}

// handle the device info message from the server. This will allow us to
// maintain a mapping from spice display id to xrandr output
void vdagent_display_handle_graphics_device_info(VDAgentDisplay *display, uint8_t *data,
        size_t size)
{
    VDAgentGraphicsDeviceInfo *graphics_device_info = (VDAgentGraphicsDeviceInfo *)data;
    VDAgentDeviceDisplayInfo *device_display_info = graphics_device_info->display_info;

    void *buffer_end = data + size;
    bool decrement_id = has_zero_based_display_id(display);

    syslog(LOG_INFO, "Received Graphics Device Info:");

    for (size_t i = 0; i < graphics_device_info->count; ++i) {
        if ((void*) device_display_info > buffer_end ||
                (void*) (&device_display_info->device_address +
                    device_display_info->device_address_len) > buffer_end) {
            syslog(LOG_ERR, "Malformed graphics_display_info message, "
                   "extends beyond the end of the buffer");
            break;
        }

        // make sure the string is terminated:
        if (device_display_info->device_address_len > 0) {
            device_display_info->device_address[device_display_info->device_address_len - 1] = '\0';
        } else {
            syslog(LOG_WARNING, "Zero length device_address received for channel_id: %u, monitor_id: %u",
                   device_display_info->channel_id, device_display_info->monitor_id);
        }

        // Get the expected connector name from hardware info. Store it with the SPICE display ID.
        char expected_name[100];
        int ret = get_connector_name_for_device_info(device_display_info, expected_name,
                                                     sizeof(expected_name), decrement_id);
        if (ret == 0) {
            g_hash_table_insert(display->connector_mapping,
                                g_strdup(expected_name),
                                GUINT_TO_POINTER(device_display_info->channel_id + device_display_info->monitor_id));
            syslog(LOG_DEBUG, "Mapping connector %s to display #%d", expected_name,
                   (device_display_info->channel_id + device_display_info->monitor_id));
        }

        // Also map the SPICE display ID to the corresponding X server object.
        vdagent_x11_handle_device_display_info(display->x11, device_display_info, decrement_id);

        device_display_info = (VDAgentDeviceDisplayInfo*) ((char*) device_display_info +
            sizeof(VDAgentDeviceDisplayInfo) + device_display_info->device_address_len);
    }

    // make sure daemon is up-to-date with (possibly updated) device IDs
    vdagent_display_send_daemon_guest_res(display, TRUE);
}

/*
 * Set monitor configuration according to client request.
 *
 * On exit send current configuration to client, regardless of error.
 *
 * Errors:
 *  screen size too large for driver to handle. (we set the largest/smallest possible)
 *  no randr support in X server.
 *  invalid configuration request from client.
 */
void vdagent_display_set_monitor_config(VDAgentDisplay *display, VDAgentMonitorsConfig *mon_config,
        int fallback)
{
#ifdef USE_GTK_FOR_MONITORS
    if (GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default())) {
        // FIXME: there is no equivalent call to set the monitor config under wayland
        // Send the configuration back - the client need to know the resolution was not taken into account.
        vdagent_display_send_daemon_guest_res(display, TRUE);
        return;
    }
#endif
    vdagent_x11_set_monitor_config(display->x11, mon_config, fallback);
}

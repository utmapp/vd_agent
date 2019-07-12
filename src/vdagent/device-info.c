/*  device-info.c utility function for looking up the xrandr output id for a
 *  given device address and display id
 *
 * Copyright 2018 Red Hat, Inc.
 *
 * Red Hat Authors:
 * Jonathon Jongsma <jjongsma@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <unistd.h>
#include <X11/extensions/Xrandr.h>
#include <glib.h>

#include "device-info.h"

#define PCI_VENDOR_ID_REDHAT 0x1b36
#define PCI_VENDOR_ID_REDHAT_QUMRANET 0x1af4 // virtio-gpu
#define PCI_VENDOR_ID_INTEL 0x8086
#define PCI_VENDOR_ID_NVIDIA 0x10de

#define PCI_DEVICE_ID_QXL 0x0100
#define PCI_DEVICE_ID_VIRTIO_GPU 0x1050

typedef struct PciDevice {
    int domain;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
} PciDevice;

typedef struct PciAddress {
    int domain;
    GList *devices; /* PciDevice */
} PciAddress;

static PciAddress* pci_address_new()
{
    return g_new0(PciAddress, 1);
}

static void pci_address_free(PciAddress *addr)
{
    g_list_free_full(addr->devices, g_free);
    g_free(addr);
}


static int read_next_hex_number(const char *input, char delim, char **endptr)
{
    assert(input != NULL);
    assert(endptr != NULL);

    const char *pos = strchr(input, delim);
    int n;
    if (!pos) {
        *endptr = NULL;
        return 0;
    }

    char *endpos;
    n = strtol(input, &endpos, 16);

    // check if we read all characters until the delimiter
    if (endpos != pos) {
        endpos = NULL;
    }

    *endptr = endpos;
    return n;
}

// the device should be specified in BDF notation (e.g. 0000:00:02.0)
// see https://wiki.xen.org/wiki/Bus:Device.Function_(BDF)_Notation
static bool parse_pci_device(const char *bdf, const char *end, PciDevice *device)
{
    if (!end) {
        end = strchr(bdf, 0);
    }

    int endpos = -1;
    int domain, bus, slot, function;
    sscanf(bdf, "%x:%x:%x.%x%n", &domain, &bus, &slot, &function, &endpos);
    if (!device || endpos < 0 || bdf + endpos != end) {
        return false;
    }
    if (domain < 0 || bus < 0 || slot < 0 || function < 0) {
        return false;
    }

    device->domain = domain;
    device->bus = bus;
    device->slot = slot;
    device->function = function;
    return true;
}

// We need to extract the pci address of the device from the sysfs entry for the device like so:
// $ readlink /sys/class/drm/card0
// This should give you a path such as this for cards on the root bus:
// /sys/devices/pci0000:00/0000:00:02.0/drm/card0
// or something like this if there is a pci bridge:
// /sys/devices/pci0000:00/0000:00:03.0/0000:01:01.0/0000:02:03.0/virtio2/drm/card0
static PciAddress* parse_pci_address_from_sysfs_path(const char* addr)
{
    char *pos = strstr(addr, "/pci");
    if (!pos) {
        return NULL;
    }

    // advance to the numbers in pci0000:00
    pos += 4;
    int domain = read_next_hex_number(pos, ':', &pos);
    if (!pos) {
        return NULL;
    }

    // not used right now.
    G_GNUC_UNUSED uint8_t bus = read_next_hex_number(pos + 1, '/', &pos);
    if (!pos) {
        return NULL;
    }

    PciAddress *address = pci_address_new();
    address->domain = domain;
    // now read all of the devices
    while (pos) {
        PciDevice *dev = g_new0(PciDevice, 1);
        char *next = strchr(pos + 1, '/');
        if (!parse_pci_device(pos + 1, next, dev)) {
            g_free(dev);
            break;
        }
        address->devices = g_list_append(address->devices, dev);
        pos = next;
    }
    return address;
}

// format should be something like pci/$domain/$slot.$fn/$slot.$fn
static PciAddress* parse_pci_address_from_spice(char *input)
{
    static const char prefix[] = "pci/";
    if (strncmp(input, prefix, strlen(prefix)) != 0) {
        return NULL;
    }

    char *pos = input + strlen(prefix);
    int domain = read_next_hex_number(pos, '/', &pos);
    if (!pos) {
        return NULL;
    }

    PciAddress *address = pci_address_new();
    address->domain = domain;
    // now read all of the devices
    for (int n = 0; ; n++) {
        PciDevice *dev = g_new0(PciDevice, 1);
        char *next = strchr(pos + 1, '/');

        dev->slot = read_next_hex_number(pos + 1, '.', &pos);
        if (!pos) {
            g_free(dev);
            break;
        }

        dev->function = strtol(pos + 1, &pos, 16);
        if (!pos || (next != NULL && next != pos)) {
            g_free(dev);
            break;
        }

        address->devices = g_list_append(address->devices, dev);
        pos = next;
        if (!pos) {
            break;
        }
    }
    return address;
}

static bool compare_addresses(PciAddress *a, PciAddress *b)
{
    // only check domain, slot, and function
    if (a->domain != b->domain) {
        return false;
    }

    const GList *la, *lb;
    for (la = a->devices, lb = b->devices;
         la != NULL && lb != NULL;
         la = la->next, lb = lb->next) {
        PciDevice *deva = la->data;
        PciDevice *devb = lb->data;

        if (deva->slot != devb->slot
            || deva->function != devb->function) {
            return false;
        }
    }

    /* True only if both have the same length */
    return (la == NULL && lb == NULL);
}

// Connector type names from xorg modesetting driver
static const char * const modesetting_output_names[] = {
    [DRM_MODE_CONNECTOR_Unknown] = "None" ,
    [DRM_MODE_CONNECTOR_VGA] = "VGA" ,
    [DRM_MODE_CONNECTOR_DVII] = "DVI-I" ,
    [DRM_MODE_CONNECTOR_DVID] = "DVI-D" ,
    [DRM_MODE_CONNECTOR_DVIA] = "DVI-A" ,
    [DRM_MODE_CONNECTOR_Composite] = "Composite" ,
    [DRM_MODE_CONNECTOR_SVIDEO] = "SVIDEO" ,
    [DRM_MODE_CONNECTOR_LVDS] = "LVDS" ,
    [DRM_MODE_CONNECTOR_Component] = "Component" ,
    [DRM_MODE_CONNECTOR_9PinDIN] = "DIN" ,
    [DRM_MODE_CONNECTOR_DisplayPort] = "DP" ,
    [DRM_MODE_CONNECTOR_HDMIA] = "HDMI" ,
    [DRM_MODE_CONNECTOR_HDMIB] = "HDMI-B" ,
    [DRM_MODE_CONNECTOR_TV] = "TV" ,
    [DRM_MODE_CONNECTOR_eDP] = "eDP" ,
    [DRM_MODE_CONNECTOR_VIRTUAL] = "Virtual" ,
    [DRM_MODE_CONNECTOR_DSI] = "DSI" ,
    [DRM_MODE_CONNECTOR_DPI] = "DPI" ,
};
// Connector type names from qxl driver
static const char * const qxl_output_names[] = {
    [DRM_MODE_CONNECTOR_Unknown] = "None" ,
    [DRM_MODE_CONNECTOR_VGA] = "VGA" ,
    [DRM_MODE_CONNECTOR_DVII] = "DVI" ,
    [DRM_MODE_CONNECTOR_DVID] = "DVI" ,
    [DRM_MODE_CONNECTOR_DVIA] = "DVI" ,
    [DRM_MODE_CONNECTOR_Composite] = "Composite" ,
    [DRM_MODE_CONNECTOR_SVIDEO] = "S-video" ,
    [DRM_MODE_CONNECTOR_LVDS] = "LVDS" ,
    [DRM_MODE_CONNECTOR_Component] = "CTV" ,
    [DRM_MODE_CONNECTOR_9PinDIN] = "DIN" ,
    [DRM_MODE_CONNECTOR_DisplayPort] = "DisplayPort" ,
    [DRM_MODE_CONNECTOR_HDMIA] = "HDMI" ,
    [DRM_MODE_CONNECTOR_HDMIB] = "HDMI" ,
    [DRM_MODE_CONNECTOR_TV] = "TV" ,
    [DRM_MODE_CONNECTOR_eDP] = "eDP" ,
    [DRM_MODE_CONNECTOR_VIRTUAL] = "Virtual" ,
};


static void drm_conn_name_full(drmModeConnector *conn,
                               const char * const *names,
                               int nnames, char *dest,
                               size_t dlen, bool decrement_id)
{
    const char *type;

    if (conn->connector_type < nnames &&
        names[conn->connector_type]) {
        type = names[conn->connector_type];
    } else {
        type = "unknown";
    }

    uint8_t id = conn->connector_type_id;
    if (decrement_id) {
        id--;
    }
    snprintf(dest, dlen, "%s-%d", type, id);
}

static void drm_conn_name_qxl(drmModeConnector *conn, char *dest, size_t dlen, bool decrement_id)
{
    return drm_conn_name_full(conn, qxl_output_names,
                              sizeof(qxl_output_names)/sizeof(qxl_output_names[0]),
                              dest, dlen, decrement_id);
}

// NOTE: there are some cases (for example, in a Lenovo T460p laptop with
// intel graphics when attached to a docking station) where the modesetting
// driver uses a name such as DP-3-1 instead of DP-4. These outputs are not
// likely to exist in virtual machines, so they shouldn't matter much
static void drm_conn_name_modesetting(drmModeConnector *conn, char *dest, size_t dlen)
{
    return drm_conn_name_full(conn, modesetting_output_names,
                              sizeof(modesetting_output_names)/sizeof(modesetting_output_names[0]),
                              dest, dlen, false);
}

static bool read_hex_value_from_file(const char *path, int* value)
{
    if (value == NULL || path == NULL) {
        return false;
    }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }

    int endpos = -1;
    bool result = (fscanf(f, "%x\n%n", value, &endpos) > 0 && endpos >= 0);

    fclose(f);
    return result;
}

// returns a path to a drm device found at the given PCI Address. Returned
// string must be freed by caller.
static char* find_device_at_pci_address(PciAddress *pci_addr, int *vendor_id, int *device_id)
{
    g_return_val_if_fail(pci_addr != NULL, NULL);
    g_return_val_if_fail(device_id != NULL, NULL);
    g_return_val_if_fail(vendor_id != NULL, NULL);
    // Look for a device that matches the PCI address parsed above. Loop
    // through the list of cards reported by the DRM subsytem
    for (int i = 0; i < 10; ++i) {
        char dev_path[64];
        struct stat buf;

        // device node for the card is needed to access libdrm functionality
        snprintf(dev_path, sizeof(dev_path), DRM_DEV_NAME, DRM_DIR_NAME, i);
        if (stat(dev_path, &buf) != 0) {
            // no card exists, exit loop
            syslog(LOG_DEBUG, "card%i not found while listing DRM devices.", i);
            break;
        }

        // the sysfs directory for the card will allow us to determine the
        // pci address for the device
        char sys_path[64];
        snprintf(sys_path, sizeof(sys_path), "/sys/class/drm/card%d", i);

        // the file /sys/class/drm/card0 is a symlink to a file that
        // specifies the device's address. It usually points to something
        // like /sys/devices/pci0000:00/0000:00:02.0/drm/card0
        char device_link[PATH_MAX];
        if (realpath(sys_path, device_link) == NULL) {
            syslog(LOG_WARNING, "Failed to get the real path of %s", sys_path);
            break;
        }
        syslog(LOG_DEBUG, "Device %s is at %s", dev_path, device_link);

        PciAddress *drm_pci_addr = parse_pci_address_from_sysfs_path(device_link);
        if (!drm_pci_addr) {
            syslog(LOG_WARNING, "Can't determine pci address from '%s'", device_link);
            continue;
        }

        if (!compare_addresses(pci_addr, drm_pci_addr)) {
            pci_address_free(drm_pci_addr);
            continue;
        }
        pci_address_free(drm_pci_addr);
        char id_path[150];
        snprintf(id_path, sizeof(id_path), "%s/device/vendor", sys_path);
        if (!read_hex_value_from_file(id_path, vendor_id)) {
            syslog(LOG_WARNING, "Unable to read vendor ID of card: %s", strerror(errno));
        }
        snprintf(id_path, sizeof(id_path), "%s/device/device", sys_path);
        if (!read_hex_value_from_file(id_path, device_id)) {
            syslog(LOG_WARNING, "Unable to read device ID of card: %s", strerror(errno));
        }

        syslog(LOG_DEBUG, "Found card '%s' with Vendor ID %#x, Device ID %#x",
               device_link, *device_id, *vendor_id);
        return g_strdup(dev_path);
    }
    return NULL;
}

// PCI address should be in the following format:
//   pci/$domain/$slot.$fn/$slot.$fn
bool lookup_xrandr_output_for_device_info(VDAgentDeviceDisplayInfo *device_info,
                                          Display *xdisplay,
                                          XRRScreenResources *xres,
                                          RROutput *output_id)
{
    PciAddress *user_pci_addr = parse_pci_address_from_spice((char*)device_info->device_address);
    if (!user_pci_addr) {
        syslog(LOG_WARNING,
               "Couldn't parse PCI address '%s'. "
               "Address should be the form 'pci/$domain/$slot.$fn/$slot.fn...",
               device_info->device_address);
        return false;
    }

    int vendor_id = 0;
    int device_id = 0;
    char *dev_path = find_device_at_pci_address(user_pci_addr, &vendor_id, &device_id);
    pci_address_free(user_pci_addr);

    int drm_fd = open(dev_path, O_RDWR);
    if (drm_fd < 0) {
        syslog(LOG_WARNING, "Unable to open file %s", dev_path);
        return false;
    }

    drmModeResPtr res = drmModeGetResources(drm_fd);
    if (res) {
        // find the drm output that is equal to device_display_id
        if (device_info->device_display_id >= res->count_connectors) {
            syslog(LOG_WARNING,
                   "Specified display id %i is higher than the maximum display id "
                   "provided by this device (%i)",
                   device_info->device_display_id, res->count_connectors - 1);
            close(drm_fd);
            return false;
        }

        drmModeConnectorPtr conn =
            drmModeGetConnector(drm_fd, res->connectors[device_info->device_display_id]);
        drmModeFreeResources(res);
        res = NULL;
        close(drm_fd);

        if (conn == NULL) {
            syslog(LOG_WARNING, "Unable to get drm connector for display id %i",
                   device_info->device_display_id);
            return false;
        }

        bool decrement_name = false;
        if (vendor_id == PCI_VENDOR_ID_REDHAT && device_id == PCI_DEVICE_ID_QXL) {
            // Older QXL drivers numbered their outputs starting with
            // 0. This contrasts with most drivers who start numbering
            // outputs with 1.  In this case, the expected drm connector
            // name will need to be decremented before comparing to the
            // xrandr output name
            for (int i = 0; i < xres->noutput; ++i) {
                XRROutputInfo *oinfo = XRRGetOutputInfo(xdisplay, xres, xres->outputs[i]);
                if (!oinfo) {
                    syslog(LOG_WARNING, "Unable to lookup XRandr output info for output %li",
                           xres->outputs[i]);
                    return false;
                }
                if (strcmp(oinfo->name, "Virtual-0") == 0) {
                    decrement_name = true;
                    XRRFreeOutputInfo(oinfo);
                    break;
                }
                XRRFreeOutputInfo(oinfo);
            }
        }
        // Compare the name of the xrandr output against what we would
        // expect based on the drm connection type. The xrandr names
        // are driver-specific, so we need to special-case some
        // drivers.  Most hardware these days uses the 'modesetting'
        // driver, but the QXL device uses its own driver which has
        // different naming conventions
        char expected_name[100];
        if (vendor_id == PCI_VENDOR_ID_REDHAT && device_id == PCI_DEVICE_ID_QXL) {
            drm_conn_name_qxl(conn, expected_name, sizeof(expected_name), decrement_name);
        } else {
            drm_conn_name_modesetting(conn, expected_name, sizeof(expected_name));
        }

        // Loop through xrandr outputs and check whether the xrandr
        // output name matches the drm connector name
        for (int i = 0; i < xres->noutput; ++i) {
            int oid = xres->outputs[i];
            XRROutputInfo *oinfo = XRRGetOutputInfo(xdisplay, xres, oid);
            if (!oinfo) {
                syslog(LOG_WARNING, "Unable to lookup XRandr output info for output %i", oid);
                return false;
            }

            if (strcmp(oinfo->name, expected_name) == 0) {
                *output_id = oid;
                syslog(LOG_DEBUG, "Found matching X Output: name=%s id=%i",
                       oinfo->name, (int)oid);
                XRRFreeOutputInfo(oinfo);
                return true;
            }
            XRRFreeOutputInfo(oinfo);
        }
        drmModeFreeConnector(conn);
    } else {
        close(drm_fd);
        syslog(LOG_WARNING,
               "Unable to get DRM resources for card %s. "
               "Falling back to using xrandr output index.",
               dev_path);
        // This is probably a proprietary driver (e.g. Nvidia) that does
        // not provide outputs via drm, so the only thing we can do is just
        // assume that it is the only device assigned to X, and use the
        // xrandr output order to determine the proper display.
        if (device_info->device_display_id >= xres->noutput) {
            syslog(LOG_WARNING, "The device display id %i does not exist",
                   device_info->device_display_id);
            return false;
        }
        *output_id = xres->outputs[device_info->device_display_id];
        return true;
    }

    syslog(LOG_WARNING, "Couldn't find an XRandr output for the specified device");
    return false;
}

#ifndef VDAGENT_X11_PRIV
#define VDAGENT_X11_PRIV

#include <stdint.h>
#include <stdio.h>

#include <spice/vd_agent.h>

#include <X11/extensions/Xrandr.h>
#include "display.h"

#ifndef USE_GTK_FOR_CLIPBOARD

#include "webdav-cb.h"

/* Macros to print a message to the logfile prefixed by the selection */
#define SELPRINTF(format, ...) \
    syslog(LOG_ERR, "%s: " format, \
            vdagent_x11_sel_to_str(selection), ##__VA_ARGS__)

#define VSELPRINTF(format, ...) \
    do { \
        if (x11->debug) { \
            syslog(LOG_DEBUG, "%s: " format, \
                    vdagent_x11_sel_to_str(selection), ##__VA_ARGS__); \
        } \
    } while (0)

enum { owner_none, owner_guest, owner_client };

/* X11 terminology is confusing a selection request is a request from an
   app to get clipboard data from us, so iow from the spice client through
   the vdagent channel. We handle these one at a time and queue any which
   come in while we are still handling the current one. */
struct vdagent_x11_selection_request {
    XEvent event;
    uint8_t selection;
    struct vdagent_x11_selection_request *next;
};

/* A conversion request is X11 speak for asking another app to give its
   clipboard data to us, we do these on behalf of the spice client to copy
   data from the guest to the client. Like selection requests we process
   these one at a time. */
struct vdagent_x11_conversion_request {
    Atom target;
    uint8_t selection;
    struct vdagent_x11_conversion_request *next;
};

struct clipboard_format_tmpl {
    uint32_t type;
    const char *atom_names[16];
};

struct clipboard_format_info {
    uint32_t type;
    Atom atoms[16];
    int atom_count;
};

static const struct clipboard_format_tmpl clipboard_format_templates[] = {
    { VD_AGENT_CLIPBOARD_UTF8_TEXT, { "UTF8_STRING", "text/plain;charset=UTF-8",
      "text/plain;charset=utf-8", "STRING", NULL }, },
    { VD_AGENT_CLIPBOARD_IMAGE_PNG, { "image/png", NULL }, },
    { VD_AGENT_CLIPBOARD_IMAGE_BMP, { "image/bmp", "image/x-bmp",
      "image/x-MS-bmp", "image/x-win-bitmap", NULL }, },
    { VD_AGENT_CLIPBOARD_IMAGE_TIFF, { "image/tiff", NULL }, },
    { VD_AGENT_CLIPBOARD_IMAGE_JPG, { "image/jpeg", NULL }, },
    { VD_AGENT_CLIPBOARD_FILE_LIST, { "text/uri-list",
      "text/plain;charset=utf-8", "application/x-kde-cutselection",
      "x-special/gnome-copied-files", "x-special/mate-copied-files", NULL } },
};

#define clipboard_format_count (sizeof(clipboard_format_templates)/sizeof(clipboard_format_templates[0]))

#define ATOM_NAME_CACHE_SIZE 16
struct atom_name_cache_item {
    Atom atom;
    char *name;
};
#endif

#define MAX_SCREENS 16
/* Same as qxl_dev.h client_monitors_config.heads count */
#define MONITOR_SIZE_COUNT 64

struct monitor_size {
    int width;
    int height;
};

struct vdagent_x11 {
    Display *display;
#ifndef USE_GTK_FOR_CLIPBOARD
    struct clipboard_format_info clipboard_formats[clipboard_format_count];
    Atom clipboard_atom;
    Atom clipboard_primary_atom;
    Atom targets_atom;
    Atom incr_atom;
    Atom multiple_atom;
    Atom timestamp_atom;
    struct atom_name_cache_item atom_name_cache[ATOM_NAME_CACHE_SIZE];
    int atom_name_cache_next;
    Window selection_window;
    int xfixes_event_base;
    int max_prop_size;
    int expected_targets_notifies[256];
    int ignore_targets_notifies[256];
    int clipboard_owner[256];
    int clipboard_type_count[256];
    uint32_t clipboard_agent_types[256][256];
    Bool clipboard_has_files[256];
    Atom clipboard_x11_targets[256][256];
    /* Data for conversion_req which is currently being processed */
    struct vdagent_x11_conversion_request *conversion_req;
    int expect_property_notify;
    uint8_t *clipboard_data;
    uint32_t clipboard_data_size;
    uint32_t clipboard_data_space;
    /* Data for selection_req which is currently being processed */
    struct vdagent_x11_selection_request *selection_req;
    uint8_t *selection_req_data;
    uint32_t selection_req_data_pos;
    uint32_t selection_req_data_size;
    GBytes *file_list_data[256];
    Atom selection_req_atom;
#endif
    Window root_window[MAX_SCREENS];
    UdscsConnection *vdagentd;
    int debug;
    int screen_count;
    int width[MAX_SCREENS];
    int height[MAX_SCREENS];
    /* resolution change state */
    struct {
        XRRScreenResources *res;
        XRROutputInfo **outputs;
        XRRCrtcInfo **crtcs;
        int event_base;
        int min_width;
        int max_width;
        int min_height;
        int max_height;
        int num_monitors;
        struct monitor_size monitor_sizes[MONITOR_SIZE_COUNT];
        VDAgentMonitorsConfig *failed_conf;
    } randr;

    /* NB: we cache this assuming the driver isn't changed under our feet */
    int set_crtc_config_not_functional;

    int has_xrandr;
    int xrandr_major;
    int xrandr_minor;
    int has_xinerama;
    int dont_send_guest_xorg_res;
    GHashTable *guest_output_map;

    VDAgentDisplay *vdagent_display;
};

extern int (*vdagent_x11_prev_error_handler)(Display *, XErrorEvent *);
extern int vdagent_x11_caught_error;

void vdagent_x11_randr_init(struct vdagent_x11 *x11);
void vdagent_x11_randr_destroy(struct vdagent_x11 *x11);

GArray *vdagent_x11_get_resolutions(struct vdagent_x11 *x11, gboolean update,
                                    int *width, int *height, int *system_screen_count);
void vdagent_x11_randr_handle_root_size_change(struct vdagent_x11 *x11,
                                            int screen, int width, int height);
int vdagent_x11_randr_handle_event(struct vdagent_x11 *x11,
    const XEvent *event);
void vdagent_x11_set_error_handler(struct vdagent_x11 *x11,
    int (*handler)(Display *, XErrorEvent *));
int vdagent_x11_restore_error_handler(struct vdagent_x11 *x11);

#endif // VDAGENT_X11_PRIV

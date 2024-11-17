/*
 * Copyright (C) 2024 osy
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef vddagent_h
#define vddagent_h

#include <stdbool.h>

typedef struct VDAgent VDAgent;

typedef enum {
    VDAgentClipboardTypeNone,
    VDAgentClipboardTypeUTF8Text,
    VDAgentClipboardTypeImagePNG,
    VDAgentClipboardTypeImageBMP,
    VDAgentClipboardTypeImageTIFF,
    VDAgentClipboardTypeImageJPG,
} vdagent_clipboard_type_t;

typedef enum {
    VDAgentClipboardSelectClipboard,
    VDAgentClipboardSelectPrimary,
    VDAgentClipboardSelectSecondary,
} vdagent_clipboard_select_t;

typedef bool (*vdagent_clipboard_request_cb)(VDAgent *agent, void *ctx, vdagent_clipboard_select_t sel, vdagent_clipboard_type_t type);
typedef bool (*vdagent_clipboard_grab_cb)(VDAgent *agent, void *ctx, vdagent_clipboard_select_t sel,
                                          const vdagent_clipboard_type_t *types, unsigned int n_types);
typedef bool (*vdagent_clipboard_data_cb)(VDAgent *agent, void *ctx, vdagent_clipboard_select_t sel,
                                          vdagent_clipboard_type_t type, const unsigned char *data, unsigned int size);
typedef bool (*vdagent_clipboard_release_cb)(VDAgent *agent, void *ctx, vdagent_clipboard_select_t sel);
typedef void (*vdagent_clipboard_guest_update_cb)(VDAgent *agent, void *ctx);
typedef void (*vdagent_client_disconnected_cb)(VDAgent *agent, void *ctx);

typedef struct {
    vdagent_clipboard_request_cb clipboard_request;
    vdagent_clipboard_grab_cb clipboard_grab;
    vdagent_clipboard_data_cb clipboard_data;
    vdagent_clipboard_release_cb clipboard_release;
    vdagent_clipboard_guest_update_cb clipboard_guest_update;
    vdagent_client_disconnected_cb client_disconnected;
} vdagent_cb_t;

void vdagent_set_debug(int debugOption);
int vdagent_start(const char *socketPath, const vdagent_cb_t *cb, void *ctx);

bool vdagent_clipboard_request(VDAgent *agent, vdagent_clipboard_select_t sel, vdagent_clipboard_type_t type);
bool vdagent_clipboard_grab(VDAgent *agent, vdagent_clipboard_select_t sel,
                            const vdagent_clipboard_type_t *types, int n_types);
bool vdagent_clipboard_data(VDAgent *agent, vdagent_clipboard_select_t sel,
                            vdagent_clipboard_type_t type, const unsigned char *data, unsigned int size);
bool vdagent_clipboard_release(VDAgent *agent, vdagent_clipboard_select_t sel);

#endif /* vddagent_h */

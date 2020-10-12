/* mutter.h - implements the DBUS interface to mutter

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

#ifndef SRC_VDAGENT_MUTTER_H_
#define SRC_VDAGENT_MUTTER_H_

typedef struct VDAgentMutterDBus VDAgentMutterDBus;

VDAgentMutterDBus *vdagent_mutter_create(GHashTable *connector_mapping);
void vdagent_mutter_destroy(VDAgentMutterDBus *mutter);

GArray *vdagent_mutter_get_resolutions(VDAgentMutterDBus *mutter, int *width, int *height, int *screen_count);


#endif /* SRC_VDAGENT_MUTTER_H_ */

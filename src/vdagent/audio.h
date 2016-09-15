/*  audio.h vdagentd audio handling header

    Copyright 2015, 2016 Red Hat, Inc.

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
#ifndef __VDAGENT_AUDIO_H
#define __VDAGENT_AUDIO_H

#include <stdio.h>
#include <stdint.h>

void vdagent_audio_playback_sync(uint8_t mute, uint8_t nchannels, uint16_t *volume);
void vdagent_audio_record_sync(uint8_t mute, uint8_t nchannels, uint16_t *volume);

#endif

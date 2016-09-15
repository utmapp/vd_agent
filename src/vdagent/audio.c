/*  vdagent-audio.c vdagentd audio handling code

    Copyright 2015 Red Hat, Inc.

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
#include <config.h>
#endif

#include <glib.h>
#include <syslog.h>
#include <stdbool.h>
#include <alsa/asoundlib.h>
#include <alsa/mixer.h>
#include <alsa/error.h>
#include "audio.h"

#define ALSA_MUTE   0
#define ALSA_UNMUTE 1

static snd_mixer_elem_t *
get_alsa_default_mixer_by_name(snd_mixer_t **handle, const char *name)
{
    snd_mixer_selem_id_t *sid;
    int err = 0;

    if ((err = snd_mixer_open(handle, 0)) < 0)
        goto fail;

    if ((err = snd_mixer_attach(*handle, "default")) < 0)
        goto fail;

    if ((err = snd_mixer_selem_register(*handle, NULL, NULL)) < 0)
        goto fail;

    if ((err = snd_mixer_load(*handle)) < 0)
        goto fail;

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, name);
    return snd_mixer_find_selem(*handle, sid);

fail:
    syslog(LOG_WARNING, "%s fail: %s", __func__, snd_strerror(err));
    return NULL;
}

static bool set_alsa_capture(uint8_t mute, uint8_t nchannels, uint16_t *volume)
{
    snd_mixer_t *handle = NULL;
    snd_mixer_elem_t *e;
    long min, max, vol;
    bool ret = true;
    int alsa_mute;

    e = get_alsa_default_mixer_by_name (&handle, "Capture");
    if (e == NULL) {
        syslog(LOG_WARNING, "vdagent-audio: can't get default alsa mixer");
        ret = false;
        goto end;
    }

    alsa_mute = (mute) ? ALSA_MUTE : ALSA_UNMUTE;
    snd_mixer_selem_set_capture_switch_all(e, alsa_mute);

    snd_mixer_selem_get_capture_volume_range(e, &min, &max);
    switch (nchannels) {
    case 1: /* MONO */
        vol = CLAMP(volume[0], min, max);
        snd_mixer_selem_set_capture_volume(e, SND_MIXER_SCHN_MONO, vol);
        syslog(LOG_DEBUG, "vdagent-audio: (capture-mono) %lu (%%%0.2f)",
               vol, (float) (100*vol/max));
        break;
    case 2: /* LEFT-RIGHT */
        vol = CLAMP(volume[0], min, max);
        snd_mixer_selem_set_capture_volume(e, SND_MIXER_SCHN_FRONT_LEFT, vol);
        syslog(LOG_DEBUG, "vdagent-audio: (capture-left) %lu (%%%0.2f)",
               vol, (float) (100*vol/max));
        vol = CLAMP(volume[1], min, max);
        snd_mixer_selem_set_capture_volume(e, SND_MIXER_SCHN_FRONT_RIGHT, vol);
        syslog(LOG_DEBUG, "vdagent-audio: (capture-right) %lu (%%%0.2f)",
               vol, (float) (100*vol/max));
        break;
    default:
        syslog(LOG_WARNING, "vdagent-audio: number of channels not supported");
        ret = false;
    }
end:
    if (handle != NULL)
        snd_mixer_close(handle);
    return ret;
}

static bool set_alsa_playback (uint8_t mute, uint8_t nchannels, uint16_t *volume)
{
    snd_mixer_t *handle = NULL;
    snd_mixer_elem_t* e;
    long min, max, vol;
    bool ret = true;
    int alsa_mute;

    e = get_alsa_default_mixer_by_name (&handle, "Master");
    if (e == NULL) {
        syslog(LOG_WARNING, "vdagent-audio: can't get default alsa mixer");
        ret = false;
        goto end;
    }

    alsa_mute = (mute) ? ALSA_MUTE : ALSA_UNMUTE;
    snd_mixer_selem_set_playback_switch_all(e, alsa_mute);

    snd_mixer_selem_get_playback_volume_range(e, &min, &max);
    switch (nchannels) {
    case 1: /* MONO */
        vol = CLAMP(volume[0], min, max);
        snd_mixer_selem_set_playback_volume(e, SND_MIXER_SCHN_MONO, vol);
        syslog(LOG_DEBUG, "vdagent-audio: (playback-mono) %lu (%%%0.2f)",
               vol, (float) (100*vol/max));
        break;
    case 2: /* LEFT-RIGHT */
        vol = CLAMP(volume[0], min, max);
        snd_mixer_selem_set_playback_volume(e, SND_MIXER_SCHN_FRONT_LEFT, vol);
        syslog(LOG_DEBUG, "vdagent-audio: (playback-left) %lu (%%%0.2f)",
               vol, (float) (100*vol/max));
        vol = CLAMP(volume[1], min, max);
        snd_mixer_selem_set_playback_volume(e, SND_MIXER_SCHN_FRONT_RIGHT, vol);
        syslog(LOG_DEBUG, "vdagent-audio: (playback-right) %lu (%%%0.2f)",
               vol, (float) (100*vol/max));
        break;
    default:
        syslog(LOG_WARNING, "vdagent-audio: number of channels not supported");
        ret = false;
    }
end:
    if (handle != NULL)
        snd_mixer_close(handle);
    return ret;
}

void vdagent_audio_playback_sync(uint8_t mute, uint8_t nchannels, uint16_t *volume)
{
    syslog(LOG_DEBUG, "%s mute=%s nchannels=%u",
           __func__, (mute) ? "yes" : "no", nchannels);
    if (set_alsa_playback (mute, nchannels, volume) == false)
        syslog(LOG_WARNING, "Fail to sync playback volume");
}

void vdagent_audio_record_sync(uint8_t mute, uint8_t nchannels, uint16_t *volume)
{
    syslog(LOG_DEBUG, "%s mute=%s nchannels=%u",
           __func__, (mute) ? "yes" : "no", nchannels);
    if (set_alsa_capture (mute, nchannels, volume) == false)
        syslog(LOG_WARNING, "Fail to sync record volume");
}

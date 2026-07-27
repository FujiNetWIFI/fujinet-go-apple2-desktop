/*
 * apple2session audio backend: SDL3 audio device pulling mixed emulator +
 * FujiNet samples. SDL routes through PipeWire/Pulse on modern Linux and
 * ports unchanged to the future Mac/Windows frontends. Only the audio
 * subsystem is initialized here -- never SDL video, which would fight the
 * GTK/Qt display stack.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "session_internal.h"

typedef struct {
    SDL_AudioStream *stream;
} audio_state;

/* Pull model: the device asks for more and we hand back exactly that much.
 * apple2session_render_audio drains the libretro host's ring (filled by the
 * emulator thread once per frame) and overlays FujiNet's SAM speech, and is
 * safe to call from the audio thread.
 *
 * Everything here counts int16 *samples*, not frames: the stream is
 * interleaved stereo, so one frame is two samples. */
static void audio_cb(void *ud, SDL_AudioStream *stream, int additional_amount,
                     int total_amount)
{
    apple2session *s = ud;
    int16_t buf[2048];
    (void)total_amount;

    while (additional_amount > 0) {
        int want_bytes = additional_amount > (int)sizeof(buf)
                             ? (int)sizeof(buf)
                             : additional_amount;
        int want_samples = want_bytes / 2;
        apple2session_render_audio(s, buf, want_samples);
        SDL_PutAudioStreamData(stream, buf, want_samples * 2);
        additional_amount -= want_samples * 2;
    }
}

int audio_start(apple2session *s)
{
    audio_state *a;
    SDL_AudioSpec spec;

    if (s->audio) return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        session_set_error(s, "SDL audio init failed: %s", SDL_GetError());
        return -1;
    }

    a = calloc(1, sizeof(*a));
    if (!a) return -1;

    /* The Apple II speaker is mono, but the libretro core mixes it with the
     * Mockingboard/Phasor cards and emits interleaved stereo -- so the device
     * is stereo, unlike the ADAM target's. */
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = 44100;
    a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &spec, audio_cb, s);
    if (!a->stream) {
        session_set_error(s, "SDL audio device open failed: %s",
                          SDL_GetError());
        free(a);
        return -1;
    }
    SDL_ResumeAudioStreamDevice(a->stream);
    s->audio = a;
    return 0;
}

void audio_stop(apple2session *s)
{
    audio_state *a = s->audio;
    if (!a) return;
    s->audio = NULL;
    SDL_DestroyAudioStream(a->stream); /* also closes the bound device */
    free(a);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

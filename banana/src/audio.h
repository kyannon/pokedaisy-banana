// Sound: ALSA's "default" device (docs/EMULATORS.md: bananad routes it to the
// speakers, the headphones or the RG DS Plus's amplifiers), S16 stereo at
// 48 kHz - the rate bananad pins both directions of the card to. libasound is
// loaded at run time, so a system without it plays silent rather than not at all.
//
// Written without blocking: the panels' vblank paces the game, and a sound
// buffer that is full drops the few samples that don't fit.
#ifndef BANANA_AUDIO_H
#define BANANA_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_RATE 48000

bool audio_open(const char* device);
void audio_close(void);
bool audio_on(void);
// frames: stereo pairs.
void audio_write(const int16_t* samples, int frames);

#endif

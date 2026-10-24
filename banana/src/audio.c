// See audio.h. The few libasound calls used, declared here and found with
// dlsym (the same functions and values as <alsa/asoundlib.h>).
#include "audio.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>

typedef struct snd_pcm snd_pcm_t;
#define SND_PCM_STREAM_PLAYBACK 0
#define SND_PCM_FORMAT_S16_LE 2
#define SND_PCM_ACCESS_RW_INTERLEAVED 3
#define SND_PCM_NONBLOCK 0x1

static struct {
    void* lib;
    int (*open)(snd_pcm_t**, const char*, int, int);
    int (*set_params)(snd_pcm_t*, int, int, unsigned, unsigned, int, unsigned);
    long (*writei)(snd_pcm_t*, const void*, unsigned long);
    int (*recover)(snd_pcm_t*, int, int);
    int (*drop)(snd_pcm_t*);
    int (*prepare)(snd_pcm_t*);
    int (*close)(snd_pcm_t*);
    const char* (*strerror)(int);
    snd_pcm_t* pcm;
} A;

// 80 ms: room for a frame that took long (a ROM scan, a state save) without a gap
#define LATENCY_US 80000

bool audio_open(const char* device) {
    if (A.pcm) return true;
    if (!A.lib) {
        A.lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!A.lib) {
            fprintf(stderr, "pokedaisy: no libasound (%s): no sound\n", dlerror());
            return false;
        }
        *(void**) &A.open = dlsym(A.lib, "snd_pcm_open");
        *(void**) &A.set_params = dlsym(A.lib, "snd_pcm_set_params");
        *(void**) &A.writei = dlsym(A.lib, "snd_pcm_writei");
        *(void**) &A.recover = dlsym(A.lib, "snd_pcm_recover");
        *(void**) &A.drop = dlsym(A.lib, "snd_pcm_drop");
        *(void**) &A.prepare = dlsym(A.lib, "snd_pcm_prepare");
        *(void**) &A.close = dlsym(A.lib, "snd_pcm_close");
        *(void**) &A.strerror = dlsym(A.lib, "snd_strerror");
        if (!A.open || !A.set_params || !A.writei || !A.recover || !A.drop || !A.prepare || !A.close || !A.strerror) {
            fprintf(stderr, "pokedaisy: libasound is missing functions: no sound\n");
            dlclose(A.lib);
            A.lib = NULL;
            return false;
        }
    }
    int r = A.open(&A.pcm, device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (r < 0) {
        fprintf(stderr, "pokedaisy: can't open sound device %s (%s): no sound\n", device, A.strerror(r));
        A.pcm = NULL;
        return false;
    }
    r = A.set_params(A.pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, AUDIO_RATE, 1, LATENCY_US);
    if (r < 0) {
        fprintf(stderr, "pokedaisy: sound device %s refused S16 stereo %d Hz (%s): no sound\n", device, AUDIO_RATE,
                A.strerror(r));
        A.close(A.pcm);
        A.pcm = NULL;
        return false;
    }
    return true;
}

void audio_close(void) {
    if (A.pcm) {
        A.drop(A.pcm);
        A.close(A.pcm);
        A.pcm = NULL;
    }
}

bool audio_on(void) {
    return A.pcm != NULL;
}

void audio_write(const int16_t* samples, int frames) {
    if (!A.pcm) return;
    for (int tries = 0; frames > 0 && tries < 3; tries++) {
        long w = A.writei(A.pcm, samples, (unsigned long) frames);
        if (w == -EAGAIN) return; // full: these don't fit
        if (w < 0) {
            // an underrun (the game stopped for a sleep: SIGSTOP), or the card suspended: start again. After a
            // suspend (-ESTRPIPE) straight to prepare: snd_pcm_recover would wait a second at a time for a resume
            // the card may never offer, with the game stopped meanwhile
            int r = w == -ESTRPIPE ? A.prepare(A.pcm) : A.recover(A.pcm, (int) w, 1);
            if (r < 0) {
                fprintf(stderr, "pokedaisy: sound stopped (%s)\n", A.strerror((int) w));
                audio_close();
                return;
            }
            continue;
        }
        samples += w * 2;
        frames -= (int) w;
    }
}

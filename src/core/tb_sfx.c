/* tb_sfx.c - tiny additive synth over an SDL3 audio stream. */
#include "tb_sfx.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR 44100
#define MAX_OSC 32
enum { WAVE_SINE, WAVE_SQUARE, WAVE_TRI, WAVE_SAW };

typedef struct { bool on; double phase, freq, freq_end; double t, dur, gain; int wave; double glide; } osc;

struct tb_sfx_engine {
    SDL_AudioStream *stream;
    SDL_Mutex *mtx;
    osc oscs[MAX_OSC];
    /* mania music */
    bool music; int round; double beat_timer; int note_i;
    /* looping music PCM (mono); when present, used instead of the synth arpeggio */
    float *mpcm; size_t mframes; double msr, mpos;
};

static double wave_sample(int w, double ph) {
    switch (w) {
        case WAVE_SQUARE: return ph < M_PI ? 1.0 : -1.0;
        case WAVE_TRI:    return 2.0 / M_PI * asin(sin(ph));
        case WAVE_SAW:    return (ph / M_PI) - 1.0;
        default:          return sin(ph);
    }
}

static void add_osc(tb_sfx_engine *s, double freq, double dur, int wave, double gain, double freq_end) {
    SDL_LockMutex(s->mtx);
    for (int i = 0; i < MAX_OSC; i++) if (!s->oscs[i].on) {
        s->oscs[i] = (osc){ true, 0, freq, freq_end > 0 ? freq_end : freq, 0, dur, gain, wave, freq_end > 0 ? 1 : 0 };
        break;
    }
    SDL_UnlockMutex(s->mtx);
}

/* SDL pull callback: fill `len` bytes of float stereo. */
static void SDLCALL fill(void *user, SDL_AudioStream *st, int additional, int total) {
    (void)total;
    tb_sfx_engine *s = user;
    int frames = additional / (int)(2 * sizeof(float));
    if (frames <= 0) return;
    float *buf = malloc((size_t)frames * 2 * sizeof(float));
    SDL_LockMutex(s->mtx);
    for (int n = 0; n < frames; n++) {
        double mix = 0;
        /* looping background music (mono, resampled by fractional stepping) */
        if (s->music && s->mpcm && s->mframes > 1) {
            size_t i0 = (size_t)s->mpos; double fr = s->mpos - i0;
            size_t i1 = (i0 + 1) % s->mframes;
            mix += (s->mpcm[i0] * (1 - fr) + s->mpcm[i1] * fr) * 0.55;
            s->mpos += s->msr / SR;
            if (s->mpos >= s->mframes) s->mpos -= s->mframes;
        }
        for (int i = 0; i < MAX_OSC; i++) {
            osc *o = &s->oscs[i];
            if (!o->on) continue;
            double env;                 /* quick attack, exp decay (WebAudio-like) */
            double a = 0.006;
            if (o->t < a) env = o->t / a; else env = exp(-(o->t - a) / (o->dur * 0.5 + 1e-4));
            double f = o->freq;
            if (o->glide) f = o->freq + (o->freq_end - o->freq) * (o->t / o->dur);
            mix += wave_sample(o->wave, o->phase) * o->gain * env;
            o->phase += 2 * M_PI * f / SR; if (o->phase > 2 * M_PI) o->phase -= 2 * M_PI;
            o->t += 1.0 / SR;
            if (o->t >= o->dur + 0.05) o->on = false;
        }
        if (mix > 1) mix = 1; if (mix < -1) mix = -1;
        buf[n * 2] = (float)mix; buf[n * 2 + 1] = (float)mix;
    }
    SDL_UnlockMutex(s->mtx);
    SDL_PutAudioStreamData(st, buf, frames * 2 * (int)sizeof(float));
    free(buf);
}

tb_sfx_engine *tb_sfx_create(void) {
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    tb_sfx_engine *s = calloc(1, sizeof *s);
    s->mtx = SDL_CreateMutex();
    SDL_AudioSpec spec; SDL_zero(spec); spec.format = SDL_AUDIO_F32; spec.channels = 2; spec.freq = SR;
    s->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, fill, s);
    if (s->stream) SDL_ResumeAudioStreamDevice(s->stream);
    return s;
}
void tb_sfx_destroy(tb_sfx_engine *s) {
    if (!s) return;
    if (s->stream) SDL_DestroyAudioStream(s->stream);
    if (s->mtx) SDL_DestroyMutex(s->mtx);
    free(s->mpcm);
    free(s);
}

static void tone(tb_sfx_engine *s, double f, double dur, int w, double g) { add_osc(s, f, dur, w, g, 0); }
static void coin(tb_sfx_engine *s) {
    double base = 720, arp = 1.52;
    double notes[3] = { base, base * arp, base * arp * 1.22 };
    for (int i = 0; i < 3; i++) add_osc(s, notes[i], 0.18, WAVE_TRI, 0.18 * (1 - i * 0.12), 0);
}

void tb_sfx_play(tb_sfx_engine *s, tb_sfx which) {
    switch (which) {
        case TB_SFX_CORRECT:      coin(s); break;
        case TB_SFX_MANIA_ANSWER: tone(s, 880, 0.06, WAVE_SQUARE, 0.14); tone(s, 1320, 0.07, WAVE_SQUARE, 0.12); tone(s, 1760, 0.085, WAVE_TRI, 0.1); break;
        case TB_SFX_MANIA_PICKUP: tone(s, 520, 0.055, WAVE_SQUARE, 0.13); tone(s, 780, 0.07, WAVE_SQUARE, 0.11); break;
        case TB_SFX_MANIA_STEP:   tone(s, 620, 0.04, WAVE_SQUARE, 0.09); break;
        case TB_SFX_WRONG:        tone(s, 200, 0.18, WAVE_SQUARE, 0.05); tone(s, 150, 0.22, WAVE_SQUARE, 0.045); break;
        case TB_SFX_GAMEOVER:     add_osc(s, 300, 0.85, WAVE_SAW, 0.08, 60); break;
        case TB_SFX_WIN:          { double f[4] = {523,659,784,1046}; for (int i = 0; i < 4; i++) tone(s, f[i], 0.18, WAVE_TRI, 0.06); } break;
        default: break;
    }
}

void tb_sfx_set_round(tb_sfx_engine *s, int round) { s->round = round; }

/* The Qt timer should call this each frame to schedule the synthetic arpeggio. */
void tb_sfx_music(tb_sfx_engine *s, bool on, int round) {
    bool rising = on && !s->music;
    SDL_LockMutex(s->mtx);
    s->music = on; s->round = round;
    if (rising) s->mpos = 0;          /* (re)start the song from the top, e.g. after a loss */
    if (!on) { s->note_i = 0; s->beat_timer = 0; }
    SDL_UnlockMutex(s->mtx);
}

void tb_sfx_set_music(tb_sfx_engine *s, const float *mono, size_t frames, double sr) {
    SDL_LockMutex(s->mtx);
    free(s->mpcm); s->mpcm = NULL; s->mframes = 0; s->mpos = 0;
    if (mono && frames > 1 && sr > 0) {
        s->mpcm = malloc(frames * sizeof(float));
        if (s->mpcm) { memcpy(s->mpcm, mono, frames * sizeof(float)); s->mframes = frames; s->msr = sr; }
    }
    SDL_UnlockMutex(s->mtx);
}
bool tb_sfx_has_music(const tb_sfx_engine *s) { return s->mpcm && s->mframes > 1; }

void tb_sfx_tick(tb_sfx_engine *s, double now_ms) {
    if (!s->music || s->mpcm) return;   /* real music playing -> no synth arpeggio */
    if (now_ms < s->beat_timer) return;
    static const double notes[8] = {330, 392, 494, 392, 523, 494, 392, 659};
    double tempo = 170 - s->round * 3.5; if (tempo < 70) tempo = 70; if (tempo > 170) tempo = 170;
    add_osc(s, notes[s->note_i++ % 8], 0.055, WAVE_SQUARE, 0.02, 0);
    s->beat_timer = now_ms + tempo;
}

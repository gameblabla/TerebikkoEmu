#ifndef TB_WIN32_WAVEOUT_ONLY
/* MinGW-w64 does not always provide the Core Audio interface/class GUIDs
 * from libuuid. Defining INITGUID in exactly one translation unit emits
 * CLSID_MMDeviceEnumerator, IID_IMMDeviceEnumerator, IID_IAudioClient, and
 * IID_IAudioRenderClient here for the full-featured Win64 build. */
#ifndef INITGUID
#define INITGUID
#endif
#define COBJMACROS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#ifndef TB_WIN32_WAVEOUT_ONLY
#include <mmdeviceapi.h>
#include <audioclient.h>
#endif
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include "tb_win32_audio.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef TB_WIN32_WAVEOUT_ONLY
#ifndef AUDCLNT_BUFFERFLAGS_SILENT
#define AUDCLNT_BUFFERFLAGS_SILENT 0x2
#endif
#ifndef AUDCLNT_SHAREMODE_SHARED
#define AUDCLNT_SHAREMODE_SHARED 0
#endif
#ifndef AUDCLNT_SHAREMODE_EXCLUSIVE
#define AUDCLNT_SHAREMODE_EXCLUSIVE 1
#endif
#endif
#ifndef WAVE_FORMAT_IEEE_FLOAT
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#endif

#define AUDIO_RING_FRAMES (TB_WIN32_AUDIO_RATE * 5)
#define RENDER_MAX_FRAMES 4096
#define WAVE_BLOCKS 6
#define WAVE_BLOCK_FRAMES 1024
#define MAX_OSC 48

typedef struct { bool on; double phase, freq, freq_end, t, dur, gain; int wave; bool glide; } osc;
enum { WAVE_SINE, WAVE_SQUARE, WAVE_TRI, WAVE_SAW };

typedef struct WaveBlock {
    WAVEHDR hdr;
    int16_t samples[WAVE_BLOCK_FRAMES * 2];
} WaveBlock;

#ifndef TB_WIN32_WAVEOUT_ONLY
typedef struct WasapiState {
    IMMDeviceEnumerator *enumerator;
    IMMDevice *device;
    IAudioClient *client;
    IAudioRenderClient *render;
    UINT32 buffer_frames;
    int com_initialized;
    int running;
    int exclusive;
} WasapiState;
#endif

static CRITICAL_SECTION g_cs;
static int g_cs_ready;
static HANDLE g_thread;
static volatile LONG g_quit;
#ifdef TB_WIN32_WAVEOUT_ONLY
static int g_selected_backend = TB_WIN32_AUDIO_WAVEOUT;
#else
static int g_selected_backend = TB_WIN32_AUDIO_WASAPI_SHARED;
#endif
static int g_active_backend = -1;
static char g_error[512];
static bool g_media_muted;
static bool g_running = true;

static float *g_ring;
static size_t g_ring_cap = AUDIO_RING_FRAMES;
static size_t g_ring_r, g_ring_w, g_ring_count;

static osc g_osc[MAX_OSC];
static bool g_music;
static int g_round;
static double g_beat_timer;
static int g_note_i;
static float *g_music_pcm;
static size_t g_music_frames;
static double g_music_sr, g_music_pos;

static HWAVEOUT g_waveout;
static WaveBlock g_wave_blocks[WAVE_BLOCKS];
static unsigned g_wave_index;
#ifndef TB_WIN32_WAVEOUT_ONLY
static WasapiState g_wasapi;
#endif

static void lock_audio(void) { if (g_cs_ready) EnterCriticalSection(&g_cs); }
static void unlock_audio(void) { if (g_cs_ready) LeaveCriticalSection(&g_cs); }

static void set_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, ap);
    va_end(ap);
}
static void clear_error(void) { g_error[0] = 0; }
const char *tb_win32_audio_last_error(void) { return g_error[0] ? g_error : "No audio error has been recorded."; }

const char *tb_win32_audio_backend_name(int backend) {
    switch (backend) {
        case TB_WIN32_AUDIO_WAVEOUT: return "WaveOut";
#ifndef TB_WIN32_WAVEOUT_ONLY
        case TB_WIN32_AUDIO_WASAPI_SHARED: return "WASAPI shared";
        case TB_WIN32_AUDIO_WASAPI_EXCLUSIVE: return "WASAPI exclusive";
#endif
        default: return "Unknown";
    }
}
void tb_win32_audio_set_backend(int backend) {
#ifdef TB_WIN32_WAVEOUT_ONLY
    (void)backend;
    g_selected_backend = TB_WIN32_AUDIO_WAVEOUT;
#else
    if (backend < TB_WIN32_AUDIO_WAVEOUT || backend > TB_WIN32_AUDIO_WASAPI_EXCLUSIVE) backend = TB_WIN32_AUDIO_WAVEOUT;
    g_selected_backend = backend;
#endif
}
int tb_win32_audio_get_backend(void) { return g_selected_backend; }

static WAVEFORMATEX audio_format(void) {
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = TB_WIN32_AUDIO_RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(wf.nChannels * (wf.wBitsPerSample / 8));
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    return wf;
}

static double wave_sample(int w, double ph) {
    switch (w) {
        case WAVE_SQUARE: return ph < M_PI ? 1.0 : -1.0;
        case WAVE_TRI: return 2.0 / M_PI * asin(sin(ph));
        case WAVE_SAW: return ph / M_PI - 1.0;
        default: return sin(ph);
    }
}
static void add_osc(double freq, double dur, int wave, double gain, double freq_end) {
    lock_audio();
    for (int i = 0; i < MAX_OSC; i++) if (!g_osc[i].on) {
        g_osc[i].on = true; g_osc[i].phase = 0; g_osc[i].freq = freq;
        g_osc[i].freq_end = freq_end > 0 ? freq_end : freq; g_osc[i].t = 0;
        g_osc[i].dur = dur; g_osc[i].gain = gain; g_osc[i].wave = wave; g_osc[i].glide = freq_end > 0;
        break;
    }
    unlock_audio();
}
static void tone(double f, double dur, int w, double g) { add_osc(f, dur, w, g, 0); }
static void coin(void) {
    double base = 720, arp = 1.52;
    double notes[3] = { base, base * arp, base * arp * 1.22 };
    for (int i = 0; i < 3; i++) add_osc(notes[i], 0.18, WAVE_TRI, 0.18 * (1.0 - i * 0.12), 0);
}

void tb_win32_audio_sfx_play(tb_sfx which) {
    switch (which) {
        case TB_SFX_CORRECT: coin(); break;
        case TB_SFX_MANIA_ANSWER: tone(880,0.06,WAVE_SQUARE,0.14); tone(1320,0.07,WAVE_SQUARE,0.12); tone(1760,0.085,WAVE_TRI,0.10); break;
        case TB_SFX_MANIA_PICKUP: tone(520,0.055,WAVE_SQUARE,0.13); tone(780,0.07,WAVE_SQUARE,0.11); break;
        case TB_SFX_MANIA_STEP: tone(620,0.04,WAVE_SQUARE,0.09); break;
        case TB_SFX_WRONG: tone(200,0.18,WAVE_SQUARE,0.05); tone(150,0.22,WAVE_SQUARE,0.045); break;
        case TB_SFX_GAMEOVER: add_osc(300,0.85,WAVE_SAW,0.08,60); break;
        case TB_SFX_WIN: { double f[4] = {523,659,784,1046}; for (int i=0;i<4;i++) tone(f[i],0.18,WAVE_TRI,0.06); } break;
        default: break;
    }
}
void tb_win32_audio_music(bool on, int round) {
    lock_audio();
    bool rising = on && !g_music;
    g_music = on; g_round = round;
    if (rising) g_music_pos = 0;
    if (!on) { g_note_i = 0; g_beat_timer = 0; }
    unlock_audio();
}
void tb_win32_audio_set_round(int round) { lock_audio(); g_round = round; unlock_audio(); }
void tb_win32_audio_tick(double now_ms) {
    lock_audio(); bool schedule = g_music && !g_music_pcm && now_ms >= g_beat_timer; int note = g_note_i; int round = g_round;
    if (schedule) {
        double tempo = 170 - round * 3.5; if (tempo < 70) tempo = 70; if (tempo > 170) tempo = 170;
        g_beat_timer = now_ms + tempo; g_note_i++;
    }
    unlock_audio();
    if (schedule) { static const double notes[8] = {330,392,494,392,523,494,392,659}; add_osc(notes[note & 7],0.055,WAVE_SQUARE,0.02,0); }
}
void tb_win32_audio_set_music(const float *mono, size_t frames, double sr) {
    lock_audio();
    free(g_music_pcm); g_music_pcm = NULL; g_music_frames = 0; g_music_pos = 0; g_music_sr = 0;
    if (mono && frames > 1 && sr > 0) {
        g_music_pcm = (float*)malloc(frames * sizeof(float));
        if (g_music_pcm) { memcpy(g_music_pcm, mono, frames * sizeof(float)); g_music_frames = frames; g_music_sr = sr; }
    }
    unlock_audio();
}
bool tb_win32_audio_has_music(void) { return g_music_pcm && g_music_frames > 1; }

void tb_win32_audio_submit_media(const float *stereo, size_t frames) {
    if (!stereo || !frames || !g_ring) return;
    lock_audio();
    for (size_t i = 0; i < frames; i++) {
        if (g_ring_count >= g_ring_cap) {
            g_ring_r = (g_ring_r + 1) % g_ring_cap;
            g_ring_count--;
        }
        g_ring[g_ring_w * 2] = stereo[i * 2];
        g_ring[g_ring_w * 2 + 1] = stereo[i * 2 + 1];
        g_ring_w = (g_ring_w + 1) % g_ring_cap;
        g_ring_count++;
    }
    unlock_audio();
}
void tb_win32_audio_clear_media(void) { lock_audio(); g_ring_r = g_ring_w = g_ring_count = 0; unlock_audio(); }
size_t tb_win32_audio_queued_media_frames(void) { lock_audio(); size_t n = g_ring_count; unlock_audio(); return n; }
void tb_win32_audio_set_media_muted(bool muted) { lock_audio(); g_media_muted = muted; if (muted) g_ring_r = g_ring_w = g_ring_count = 0; unlock_audio(); }
void tb_win32_audio_set_running(bool running) { lock_audio(); g_running = running; if (!running) g_ring_r = g_ring_w = g_ring_count = 0; unlock_audio(); }

static void render_mix(float *dst, size_t frames) {
    memset(dst, 0, frames * 2 * sizeof(float));
    lock_audio();
    for (size_t n = 0; n < frames; n++) {
        double l = 0, r = 0;
        if (!g_media_muted && g_running && g_ring_count > 0) {
            l += g_ring[g_ring_r * 2]; r += g_ring[g_ring_r * 2 + 1];
            g_ring_r = (g_ring_r + 1) % g_ring_cap; g_ring_count--;
        }
        if (g_music && g_music_pcm && g_music_frames > 1) {
            size_t i0 = (size_t)g_music_pos; double fr = g_music_pos - i0;
            size_t i1 = (i0 + 1) % g_music_frames;
            double s = (g_music_pcm[i0] * (1.0 - fr) + g_music_pcm[i1] * fr) * 0.55;
            l += s; r += s;
            g_music_pos += g_music_sr / (double)TB_WIN32_AUDIO_RATE;
            while (g_music_pos >= (double)g_music_frames) g_music_pos -= (double)g_music_frames;
        }
        for (int i = 0; i < MAX_OSC; i++) {
            osc *o = &g_osc[i]; if (!o->on) continue;
            double a = 0.006;
            double env = (o->t < a) ? (o->t / a) : exp(-(o->t - a) / (o->dur * 0.5 + 1e-4));
            double f = o->glide ? o->freq + (o->freq_end - o->freq) * (o->t / o->dur) : o->freq;
            double s = wave_sample(o->wave, o->phase) * o->gain * env;
            l += s; r += s;
            o->phase += 2 * M_PI * f / TB_WIN32_AUDIO_RATE;
            if (o->phase > 2 * M_PI) o->phase -= 2 * M_PI;
            o->t += 1.0 / TB_WIN32_AUDIO_RATE;
            if (o->t >= o->dur + 0.05) o->on = false;
        }
        if (l > 1) l = 1; if (l < -1) l = -1; if (r > 1) r = 1; if (r < -1) r = -1;
        dst[n * 2] = (float)l; dst[n * 2 + 1] = (float)r;
    }
    unlock_audio();
}
static void float_to_s16(int16_t *out, const float *in, size_t frames) {
    for (size_t i = 0; i < frames * 2; i++) {
        float s = in[i]; if (s > 1.0f) s = 1.0f; if (s < -1.0f) s = -1.0f;
        out[i] = (int16_t)lrintf(s * 32767.0f);
    }
}

static void waveout_close(void) {
    if (g_waveout) {
        waveOutReset(g_waveout);
        for (unsigned i = 0; i < WAVE_BLOCKS; i++) if (g_wave_blocks[i].hdr.dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(g_waveout, &g_wave_blocks[i].hdr, sizeof(WAVEHDR));
        waveOutClose(g_waveout); g_waveout = NULL;
    }
}
static bool waveout_init(void) {
    WAVEFORMATEX wf = audio_format();
    MMRESULT mmr = waveOutOpen(&g_waveout, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
    if (mmr != MMSYSERR_NOERROR) { set_error("waveOutOpen failed with MMRESULT %u", (unsigned)mmr); g_waveout = NULL; return false; }
    memset(g_wave_blocks, 0, sizeof(g_wave_blocks));
    for (unsigned i = 0; i < WAVE_BLOCKS; i++) {
        g_wave_blocks[i].hdr.lpData = (LPSTR)g_wave_blocks[i].samples;
        g_wave_blocks[i].hdr.dwBufferLength = sizeof(g_wave_blocks[i].samples);
        mmr = waveOutPrepareHeader(g_waveout, &g_wave_blocks[i].hdr, sizeof(WAVEHDR));
        if (mmr != MMSYSERR_NOERROR) { set_error("waveOutPrepareHeader failed with MMRESULT %u", (unsigned)mmr); waveout_close(); return false; }
    }
    g_wave_index = 0;
    return true;
}

#ifndef TB_WIN32_WAVEOUT_ONLY
static void wasapi_release(void) {
    if (g_wasapi.client && g_wasapi.running) IAudioClient_Stop(g_wasapi.client);
    if (g_wasapi.render) { IAudioRenderClient_Release(g_wasapi.render); g_wasapi.render = NULL; }
    if (g_wasapi.client) { IAudioClient_Release(g_wasapi.client); g_wasapi.client = NULL; }
    if (g_wasapi.device) { IMMDevice_Release(g_wasapi.device); g_wasapi.device = NULL; }
    if (g_wasapi.enumerator) { IMMDeviceEnumerator_Release(g_wasapi.enumerator); g_wasapi.enumerator = NULL; }
    if (g_wasapi.com_initialized) CoUninitialize();
    memset(&g_wasapi, 0, sizeof(g_wasapi));
}
static bool wasapi_init(int exclusive) {
    HRESULT hr; WAVEFORMATEX wf = audio_format();
    memset(&g_wasapi, 0, sizeof(g_wasapi)); g_wasapi.exclusive = exclusive ? 1 : 0;
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) g_wasapi.com_initialized = 1; else if (hr != RPC_E_CHANGED_MODE) { set_error("CoInitializeEx failed: HRESULT 0x%08lx", (unsigned long)hr); return false; }
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void**)&g_wasapi.enumerator);
    if (FAILED(hr)) { set_error("MMDeviceEnumerator creation failed: HRESULT 0x%08lx", (unsigned long)hr); wasapi_release(); return false; }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(g_wasapi.enumerator, eRender, eConsole, &g_wasapi.device);
    if (FAILED(hr)) { set_error("Default WASAPI endpoint failed: HRESULT 0x%08lx", (unsigned long)hr); wasapi_release(); return false; }
    hr = IMMDevice_Activate(g_wasapi.device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&g_wasapi.client);
    if (FAILED(hr)) { set_error("IAudioClient activation failed: HRESULT 0x%08lx", (unsigned long)hr); wasapi_release(); return false; }
    hr = IAudioClient_IsFormatSupported(g_wasapi.client, exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED, &wf, NULL);
    if (hr != S_OK) { set_error("WASAPI %s does not accept 48 kHz stereo signed 16-bit PCM: HRESULT 0x%08lx", exclusive ? "exclusive" : "shared", (unsigned long)hr); wasapi_release(); return false; }
    REFERENCE_TIME hns = 10000LL * 120;
    hr = IAudioClient_Initialize(g_wasapi.client, exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED, 0, hns, 0, &wf, NULL);
    if (FAILED(hr)) { set_error("IAudioClient::Initialize failed: HRESULT 0x%08lx", (unsigned long)hr); wasapi_release(); return false; }
    hr = IAudioClient_GetBufferSize(g_wasapi.client, &g_wasapi.buffer_frames);
    if (FAILED(hr) || !g_wasapi.buffer_frames) { set_error("IAudioClient::GetBufferSize failed: HRESULT 0x%08lx", (unsigned long)hr); wasapi_release(); return false; }
    hr = IAudioClient_GetService(g_wasapi.client, &IID_IAudioRenderClient, (void**)&g_wasapi.render);
    if (FAILED(hr)) { set_error("IAudioClient::GetService failed: HRESULT 0x%08lx", (unsigned long)hr); wasapi_release(); return false; }
    hr = IAudioClient_Start(g_wasapi.client);
    if (FAILED(hr)) { set_error("IAudioClient::Start failed: HRESULT 0x%08lx", (unsigned long)hr); wasapi_release(); return false; }
    g_wasapi.running = 1;
    return true;
}
#endif

static DWORD WINAPI audio_thread_proc(void *arg) {
    (void)arg;
    float mix[RENDER_MAX_FRAMES * 2];
    int16_t s16[RENDER_MAX_FRAMES * 2];
    while (!InterlockedCompareExchange(&g_quit, 0, 0)) {
        if (g_active_backend == TB_WIN32_AUDIO_WAVEOUT) {
            if (!g_waveout) { Sleep(10); continue; }
            WaveBlock *b = &g_wave_blocks[g_wave_index];
            if (b->hdr.dwFlags & WHDR_INQUEUE) { Sleep(2); continue; }
            render_mix(mix, WAVE_BLOCK_FRAMES);
            float_to_s16(b->samples, mix, WAVE_BLOCK_FRAMES);
            b->hdr.dwBufferLength = WAVE_BLOCK_FRAMES * 2 * sizeof(int16_t);
            MMRESULT mmr = waveOutWrite(g_waveout, &b->hdr, sizeof(WAVEHDR));
            if (mmr == MMSYSERR_NOERROR) g_wave_index = (g_wave_index + 1) % WAVE_BLOCKS;
            else { set_error("waveOutWrite failed with MMRESULT %u", (unsigned)mmr); Sleep(10); }
#ifndef TB_WIN32_WAVEOUT_ONLY
        } else if (g_wasapi.client && g_wasapi.render) {
            UINT32 padding = 0;
            HRESULT hr = IAudioClient_GetCurrentPadding(g_wasapi.client, &padding);
            if (FAILED(hr) || padding >= g_wasapi.buffer_frames) { Sleep(3); continue; }
            UINT32 avail = g_wasapi.buffer_frames - padding;
            if (avail > RENDER_MAX_FRAMES) avail = RENDER_MAX_FRAMES;
            if (!avail) { Sleep(3); continue; }
            BYTE *dst = NULL;
            hr = IAudioRenderClient_GetBuffer(g_wasapi.render, avail, &dst);
            if (SUCCEEDED(hr) && dst) {
                render_mix(mix, avail);
                float_to_s16((int16_t*)dst, mix, avail);
                IAudioRenderClient_ReleaseBuffer(g_wasapi.render, avail, 0);
            } else Sleep(3);
#endif
        } else Sleep(10);
    }
    return 0;
}

bool tb_win32_audio_init(void) {
    if (g_thread) return true;
    clear_error();
    if (!g_cs_ready) { InitializeCriticalSection(&g_cs); g_cs_ready = 1; }
    if (!g_ring) {
        g_ring = (float*)calloc(g_ring_cap * 2, sizeof(float));
        if (!g_ring) { set_error("Could not allocate audio ring buffer."); return false; }
    }
    bool ok = false;
    g_active_backend = -1;
#ifdef TB_WIN32_WAVEOUT_ONLY
    g_selected_backend = TB_WIN32_AUDIO_WAVEOUT;
    ok = waveout_init();
#else
    if (g_selected_backend == TB_WIN32_AUDIO_WASAPI_SHARED) ok = wasapi_init(0);
    else if (g_selected_backend == TB_WIN32_AUDIO_WASAPI_EXCLUSIVE) ok = wasapi_init(1);
    else ok = waveout_init();
    if (!ok && g_selected_backend != TB_WIN32_AUDIO_WAVEOUT) {
        char first[512]; snprintf(first, sizeof(first), "%s", tb_win32_audio_last_error());
        if (waveout_init()) { set_error("%s  Falling back to WaveOut.", first); g_selected_backend = TB_WIN32_AUDIO_WAVEOUT; ok = true; }
    }
#endif
    if (!ok) return false;
    g_active_backend = g_selected_backend;
    InterlockedExchange(&g_quit, 0);
    g_thread = CreateThread(NULL, 0, audio_thread_proc, NULL, 0, NULL);
    if (!g_thread) { set_error("CreateThread failed for audio mixer."); tb_win32_audio_shutdown(); return false; }
    return true;
}
void tb_win32_audio_shutdown(void) {
    InterlockedExchange(&g_quit, 1);
    if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = NULL; }
    waveout_close();
#ifndef TB_WIN32_WAVEOUT_ONLY
    wasapi_release();
#endif
    g_active_backend = -1;
    free(g_ring); g_ring = NULL; g_ring_r = g_ring_w = g_ring_count = 0;
    free(g_music_pcm); g_music_pcm = NULL; g_music_frames = 0; g_music_pos = 0;
    if (g_cs_ready) { DeleteCriticalSection(&g_cs); g_cs_ready = 0; }
}

#ifndef TB_WIN32_AUDIO_H
#define TB_WIN32_AUDIO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tb_game.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TB_WIN32_AUDIO_WAVEOUT = 0
#ifndef TB_WIN32_WAVEOUT_ONLY
    , TB_WIN32_AUDIO_WASAPI_SHARED = 1
    , TB_WIN32_AUDIO_WASAPI_EXCLUSIVE = 2
#endif
};

#define TB_WIN32_AUDIO_RATE 48000
#define TB_WIN32_AUDIO_CHANNELS 2

void        tb_win32_audio_set_backend(int backend);
int         tb_win32_audio_get_backend(void);
const char *tb_win32_audio_backend_name(int backend);
const char *tb_win32_audio_last_error(void);
bool        tb_win32_audio_init(void);
void        tb_win32_audio_shutdown(void);

void        tb_win32_audio_submit_media(const float *stereo, size_t frames);
void        tb_win32_audio_clear_media(void);
size_t      tb_win32_audio_queued_media_frames(void);
void        tb_win32_audio_set_media_muted(bool muted);
void        tb_win32_audio_set_running(bool running);

void        tb_win32_audio_sfx_play(tb_sfx which);
void        tb_win32_audio_music(bool on, int round);
void        tb_win32_audio_set_round(int round);
void        tb_win32_audio_tick(double now_ms);
void        tb_win32_audio_set_music(const float *mono, size_t frames, double sr);
bool        tb_win32_audio_has_music(void);

#ifdef __cplusplus
}
#endif
#endif

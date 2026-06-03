#include "tb_sfx.h"
#include "tb_win32_audio.h"
#include <stdlib.h>

struct tb_sfx_engine { int dummy; };

tb_sfx_engine *tb_sfx_create(void) {
    tb_sfx_engine *s = (tb_sfx_engine*)calloc(1, sizeof(*s));
    tb_win32_audio_init();
    return s;
}
void tb_sfx_destroy(tb_sfx_engine *s) { free(s); }
void tb_sfx_play(tb_sfx_engine *s, tb_sfx which) { (void)s; tb_win32_audio_sfx_play(which); }
void tb_sfx_music(tb_sfx_engine *s, bool on, int round) { (void)s; tb_win32_audio_music(on, round); }
void tb_sfx_set_round(tb_sfx_engine *s, int round) { (void)s; tb_win32_audio_set_round(round); }
void tb_sfx_tick(tb_sfx_engine *s, double now_ms) { (void)s; tb_win32_audio_tick(now_ms); }
void tb_sfx_set_music(tb_sfx_engine *s, const float *mono, size_t frames, double sr) { (void)s; tb_win32_audio_set_music(mono, frames, sr); }
bool tb_sfx_has_music(const tb_sfx_engine *s) { (void)s; return tb_win32_audio_has_music(); }

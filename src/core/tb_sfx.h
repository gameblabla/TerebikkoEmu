/*
 * tb_sfx.h - answer-feedback sound effects + synthetic Mania music (SDL3, pure C).
 * Mirrors the WebAudio sfxCorrect / sfxWrong / sfxGameOver / coin tones and the
 * synthetic Mania arpeggio from the JS build.
 */
#ifndef TB_SFX_H
#define TB_SFX_H

#include <stdbool.h>
#include "tb_game.h"   /* tb_sfx enum */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tb_sfx_engine tb_sfx_engine;

tb_sfx_engine *tb_sfx_create(void);
void tb_sfx_destroy(tb_sfx_engine *s);

/* Play a feedback effect (see tb_sfx enum in tb_game.h). */
void tb_sfx_play(tb_sfx_engine *s, tb_sfx which);

/* Synthetic Mania background music. `round` raises tempo. */
void tb_sfx_music(tb_sfx_engine *s, bool on, int round);
void tb_sfx_set_round(tb_sfx_engine *s, int round);

/* Drive the Mania arpeggio scheduler; call each frame with a monotonic ms clock. */
void tb_sfx_tick(tb_sfx_engine *s, double now_ms);

/* Provide looping Mania music (mono float PCM). When set, Mania plays this instead of
 * the synthetic arpeggio. Pass NULL/0 to clear (revert to synth). Copies the data. */
void tb_sfx_set_music(tb_sfx_engine *s, const float *mono, size_t frames, double sr);
bool tb_sfx_has_music(const tb_sfx_engine *s);

#ifdef __cplusplus
}
#endif
#endif /* TB_SFX_H */

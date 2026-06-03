/*
 * tb_mania_music.h - Mania background music sources (pure C11).
 *
 *  - tb_mania_music_from_file: decode a sibling music.mp3 (or any media) to mono PCM.
 *  - tb_mania_music_detect_clip: locate the in-video song for games like Sailor Moon
 *    by anchoring on a known sound packet (from the game DB) and finding the long
 *    audible section bounded by silence (port of the HTML/JS anchor+silence logic),
 *    then return that slice as mono PCM. Nothing is hardcoded to fixed timestamps.
 *
 * Returned `mono` is malloc'd (caller frees); `sr` is its sample rate.
 */
#ifndef TB_MANIA_MUSIC_H
#define TB_MANIA_MUSIC_H

#include <stddef.h>
#include <stdbool.h>
#include "tb_events.h"

#ifdef __cplusplus
extern "C" {
#endif

bool tb_mania_music_from_file(const char *path, float **mono, size_t *frames, double *sr);

/* samples = one channel of the already-decoded analysis audio. anchor_bytes from the
 * game DB (e.g. "AC B2 55 35 FE"). Returns the detected clip as mono PCM. */
bool tb_mania_music_detect_clip(const float *samples, size_t n, double sr,
                                const tb_event_list *events, const char *anchor_bytes,
                                float **mono, size_t *frames, double *out_sr);

#ifdef __cplusplus
}
#endif
#endif /* TB_MANIA_MUSIC_H */

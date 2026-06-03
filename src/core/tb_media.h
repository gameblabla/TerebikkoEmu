/*
 * tb_media.h - ffmpeg-backed media analysis (pure C, libav* only; no Qt/SDL).
 *
 * Used at load time to (1) fully decode the audio to per-channel float PCM for the
 * 8 kHz control-tone decoder, (2) extract embedded text subtitles, and (3) probe
 * duration / dimensions. Runtime A/V playback lives in tb_player.
 */
#ifndef TB_MEDIA_H
#define TB_MEDIA_H

#include <stddef.h>
#include <stdbool.h>
#include "tb_subs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fully decoded audio, one float array per channel at native sample rate. */
typedef struct {
    int     nch;
    size_t  nsamples;     /* per channel */
    double  sample_rate;
    float **chan;         /* nch arrays of nsamples */
} tb_audio_channels;

void tb_audio_channels_free(tb_audio_channels *a);

/* Decode the whole first audio stream into per-channel float PCM. Returns false on
 * error (msg filled). progress() 0..100 if not NULL. */
typedef void (*tb_media_progress)(double pct, const char *text, void *user);
bool tb_media_decode_audio(const char *path, tb_audio_channels *out,
                           char *errbuf, size_t errsz,
                           tb_media_progress progress, void *user);

/* Probe duration (seconds) and video dimensions (0 if none). */
bool tb_media_probe(const char *path, double *duration, int *width, int *height,
                    char *errbuf, size_t errsz);

/* Extract embedded text subtitles (S_TEXT/UTF8, ASS, WebVTT, SRT) from the file's
 * default/most-populated subtitle stream into `out`. Returns true if any cues. */
bool tb_media_extract_subtitles(const char *path, tb_cue_list *out);

#ifdef __cplusplus
}
#endif
#endif /* TB_MEDIA_H */

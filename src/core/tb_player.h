/*
 * tb_player.h - runtime A/V player (ffmpeg decode + SDL3 audio output).
 *
 * A small threaded media player: a background thread demuxes + decodes; video
 * frames go to a bounded RGBA queue the frontend pulls for display; audio is fed
 * to an SDL3 audio stream. The audio device is the master clock at 1x playback;
 * Mania's fast/muted playback falls back to a wall-clock playhead.
 *
 * This object implements exactly the playback surface the game machine drives via
 * tb_host (seek/play/pause/rate/mute/get_time/duration/ended).
 */
#ifndef TB_PLAYER_H
#define TB_PLAYER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tb_player tb_player;

/* Hardware-decode preference. AUTO picks the best backend compiled in for the
 * current platform; explicit values force a specific one. Always falls back to
 * software decode if the backend/device/codec is unavailable. */
typedef enum {
    TB_HW_NONE = 0,     /* software decode (default)            */
    TB_HW_AUTO,         /* best available for this platform     */
    TB_HW_NVDEC,        /* NVIDIA CUDA / NVDEC (Win + Linux)     */
    TB_HW_VAAPI,        /* VAAPI (Linux: Intel/AMD/Mesa)         */
    TB_HW_D3D11VA,      /* Direct3D 11 Video Acceleration (Win)  */
    TB_HW_MEDIACODEC,   /* Android MediaCodec                    */
    TB_HW_VIDEOTOOLBOX  /* Apple VideoToolbox (macOS/iOS)        */
} tb_hwaccel;

const char *tb_hwaccel_name(tb_hwaccel h);     /* human label                  */
bool        tb_hwaccel_supported(tb_hwaccel h); /* compiled in for this platform */

/* One decoded video frame, RGBA8888, top-down. Borrowed; valid until the next
 * tb_player_acquire_frame call. */
typedef struct {
    const uint8_t *rgba;
    int   width, height, stride;
    double pts;
    bool  valid;
} tb_frame;

/* Open and start decoding (paused). `hw` requests a hardware decoder (with
 * automatic software fallback). Returns NULL on error (errbuf filled). */
tb_player *tb_player_open(const char *path, tb_hwaccel hw, char *errbuf, size_t errsz);
void       tb_player_close(tb_player *p);

/* The backend actually in use after open ("software", "NVDEC", "VAAPI", ...). */
const char *tb_player_active_hwaccel(const tb_player *p);

void   tb_player_play(tb_player *p);
void   tb_player_pause(tb_player *p);
bool   tb_player_is_paused(const tb_player *p);
void   tb_player_seek(tb_player *p, double seconds);
void   tb_player_set_rate(tb_player *p, double rate);
void   tb_player_set_muted(tb_player *p, bool muted);
double tb_player_time(const tb_player *p);
double tb_player_duration(const tb_player *p);
bool   tb_player_ended(const tb_player *p);
void   tb_player_dimensions(const tb_player *p, int *w, int *h);

/* Get the frame that best matches the current playhead. The returned pointer is
 * valid until the next call. */
tb_frame tb_player_acquire_frame(tb_player *p);

#ifdef __cplusplus
}
#endif
#endif /* TB_PLAYER_H */

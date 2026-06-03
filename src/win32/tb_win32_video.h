#ifndef TB_WIN32_VIDEO_H
#define TB_WIN32_VIDEO_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include "tb_player.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef TB_WIN32_GDI_ONLY
enum { TB_WIN32_VIDEO_GDI = 1 };
#else
enum { TB_WIN32_VIDEO_D3D11 = 0, TB_WIN32_VIDEO_GDI = 1 };
#endif

typedef struct tb_win32_video tb_win32_video;

tb_win32_video *tb_win32_video_create(HWND hwnd);
void            tb_win32_video_destroy(tb_win32_video *v);
void            tb_win32_video_set_backend(tb_win32_video *v, int backend);
int             tb_win32_video_get_backend(const tb_win32_video *v);
const char     *tb_win32_video_backend_name(int backend);
const char     *tb_win32_video_last_error(const tb_win32_video *v);
void            tb_win32_video_resize(tb_win32_video *v, int w, int h);
/* Draws the frame (and any active Mania push transition) into an offscreen
 * back-buffer. Call tb_win32_video_overlay_dc() to draw overlays onto the same
 * buffer, then tb_win32_video_finish() to blit it to the window in one shot
 * (flicker-free). */
bool            tb_win32_video_present(tb_win32_video *v, const RECT *dst, const tb_frame *frame, bool smooth);
HDC             tb_win32_video_overlay_dc(tb_win32_video *v);
/* Blit the composited region from the back-buffer to dst_dc (a window paint DC),
 * in one BitBlt. Pass the video region (e.g. the player's video_rect). */
void            tb_win32_video_finish(tb_win32_video *v, HDC dst_dc, const RECT *region);
/* Begin a Mania-style push transition using the last shown frame as the outgoing
 * image. dir: 0=from top, 1=from left, 2=from right, 3=from bottom. */
void            tb_win32_video_begin_push(tb_win32_video *v, int dir);
void            tb_win32_video_clear(tb_win32_video *v, const RECT *dst);

#ifdef __cplusplus
}
#endif
#endif

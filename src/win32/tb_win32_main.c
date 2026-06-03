#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <ocidl.h>
#include <olectl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#if !defined(TB_WIN32_USE_SDL_INPUT) && !defined(TB_WIN32_USE_WINMM_INPUT)
#define TB_WIN32_USE_SDL_INPUT 1
#endif
#if defined(TB_WIN32_USE_SDL_INPUT)
#include <SDL3/SDL.h>
#endif
#if defined(TB_WIN32_USE_WINMM_INPUT)
#include <mmsystem.h>
#endif

#include "tb_game.h"
#include "tb_events.h"
#include "tb_decode.h"
#include "tb_media.h"
#include "tb_subs.h"
#include "tb_player.h"
#include "tb_gamedb.h"
#include "tb_mania_music.h"
#include "tb_sfx.h"
#include "tb_input.h"
#include "tb_win32_audio.h"
#include "tb_win32_video.h"

#ifndef TB_MAX
#define TB_MAX(a,b) ((a) > (b) ? (a) : (b))
#define TB_MIN(a,b) ((a) < (b) ? (a) : (b))
#define TB_CLAMP(v,lo,hi) (TB_MIN(TB_MAX((v),(lo)),(hi)))
#endif
#define APP_CLASS L"TerebikkoEmuWin32"
#define TIMER_FRAME 1
#define ID_FILE_LOAD 100
#define ID_FILE_GALLERY 101
#define ID_FILE_EXIT 102
#define ID_STATE_SAVE 120
#define ID_STATE_LOAD 121
#define ID_VIEW_FULLSCREEN 140
#define ID_VIEW_GALLERY 141
#define ID_VIEW_SETTINGS 142
#define ID_VIDEO_D3D11 160
#define ID_VIDEO_GDI 161
#define ID_AUDIO_WAVEOUT 180
#ifndef TB_WIN32_WAVEOUT_ONLY
#define ID_AUDIO_WASAPI_SHARED 181
#define ID_AUDIO_WASAPI_EXCLUSIVE 182
#endif
#define ID_HW_SW 200
#define ID_HW_AUTO 201
#define ID_HW_D3D11VA 202
#define ID_HW_NVDEC 203
#define ID_HELP_ABOUT 900

#define ID_BTN_GALLERY 1000
#define ID_BTN_PLAY 1001
#define ID_BTN_RESTART 1002
#define ID_BTN_SAVE 1003
#define ID_BTN_LOAD 1004
#define ID_BTN_FULLSCREEN 1005
#define ID_BTN_ANS1 1011
#define ID_BTN_ANS2 1012
#define ID_BTN_ANS3 1013
#define ID_BTN_ANS4 1014
#define ID_BTN_PICKUP 1015
#define ID_GALLERY_LIST 1100
#define ID_GALLERY_HEADER 1101
#define ID_STATIC_FIRST 1200

/* ---- Qt-frontend palette (mirrors src/qt/MainWindow.cpp kDark/phoneBtnCss) ---- */
#define COL_BG        RGB(12,13,26)    /* #0c0d1a window/gallery background */
#define COL_BLACK     RGB(0,0,0)      /* fullscreen video/control gutter background */
#define COL_PANEL     RGB(22,24,43)    /* #16182b default button / card     */
#define COL_PANEL_HI  RGB(29,32,56)    /* #1d2038 selection highlight       */
#define COL_BORDER    RGB(44,48,80)    /* #2c3050                           */
#define COL_TEXT      RGB(238,241,255) /* #eef1ff                           */
#define COL_MUTE      RGB(166,172,208) /* #a6acd0 captions / status         */
#define COL_BLUE      RGB(47,109,240)  /* #2f6df0 Play                      */
#define COL_DIS_BG    RGB(25,27,41)    /* #191b29 disabled button           */
#define COL_DIS_FG    RGB(65,70,95)    /* #41465f disabled text             */
#define COL_ANS1      RGB(219,53,69)   /* #db3545 */
#define COL_ANS2      RGB(64,185,109)  /* #40b96d */
#define COL_ANS3      RGB(65,132,217)  /* #4184d9 */
#define COL_ANS4      RGB(228,204,66)  /* #e4cc42 */
#define COL_ANS4_FG   RGB(45,45,24)    /* #2d2d18 */
#define COL_PICKUP    RGB(55,217,154)  /* #37d99a */
#define COL_PICKUP_FG RGB(6,33,15)     /* #06210f */
#define COL_BADGE_BG  RGB(27,39,64)    /* #1b2740 */
#define COL_BADGE_FG  RGB(124,156,255) /* #7c9cff */

#define THUMB_W 200
#define THUMB_H 150

typedef struct GalleryItem { char path[MAX_PATH * 4]; char label[512]; } GalleryItem;

typedef struct App {
    HINSTANCE inst;
    HWND hwnd;
    HMENU menu;
    tb_win32_video *video;

    HWND gallery_list, gallery_header;
    HIMAGELIST gallery_images;
    HWND btn_gallery, btn_play, btn_restart, btn_save, btn_load, btn_fullscreen;
    HWND btn_ans[4], btn_pickup;
    HWND hud_score, hud_streak, hud_lives, hud_correct, mode_badge, status_label, time_label;
    HWND cap_score, cap_streak, cap_lives, cap_correct;

    HFONT font_ui, font_small, font_value, font_btn, font_bigbtn;
    HBRUSH br_bg, br_panel, br_badge, br_black;

    bool show_gallery;
    bool ready;
    bool loading;
    bool fullscreen;
    bool fullscreen_controls_panel;
    RECT windowed_rect;
    DWORD windowed_style;
    int side_width;
    RECT video_rect;
    RECT diff_rect[4];

    char exe_dir[MAX_PATH * 4];
    char gallery_dir[MAX_PATH * 4];
    GalleryItem *gallery_items;
    int gallery_count;

    char file_path[MAX_PATH * 4];
    char file_name[512];
    int64_t file_size;
    char game_name[256];
    char subtitle_label[256];
    double duration, carrier;

    tb_event_list events;
    tb_cue_list cues;
    tb_player *player;
    tb_game *game;
#if defined(TB_WIN32_USE_SDL_INPUT)
    tb_input *input;
#endif
#if defined(TB_WIN32_USE_WINMM_INPUT)
    UINT joy_id;
    DWORD joy_buttons;
    bool joy_present;
    int keymap[TB_ACT_COUNT];   /* remappable VK codes (Win32 has no tb_input) */
#endif
    tb_sfx_engine *sfx;
    float *music_pcm;
    size_t music_frames;
    double music_sr;
    tb_gamemode mode;
    tb_hwaccel hwaccel;
    bool mania_music;
    bool results_shown;
    LARGE_INTEGER qpc_freq, qpc_start;

    int last_trans_seq;
    /* cached control text/enable state - only push to the control when changed,
     * otherwise the per-frame SetWindowText/EnableWindow storm flickers the UI. */
    char c_score[64], c_streak[64], c_lives[64], c_correct[64], c_badge[32], c_play[16], c_time[64], c_status[512];
    int e_ans[4], e_pickup, e_play, e_save, e_load;
} App;

static App g_app;

static void app_set_status(const char *text);
static void app_layout(App *a);
static void app_update_controls(App *a);
static bool app_handle_keydown(App *a, WPARAM wp, LPARAM lp);
static void app_free_media(App *a);
static void app_choose_difficulty(App *a, const char *pending);
static void app_load_file(App *a, const char *path);
static char *safe_copy(char *dst, size_t n, const char *src);

/* ---------------- UTF-8 <-> UTF-16 helpers ----------------
 * All paths and on-screen strings are kept as UTF-8 internally (matching the
 * shared core and FFmpeg, which accepts UTF-8 paths on Windows). Conversion to
 * UTF-16 happens only at the Win32 API boundary so Japanese titles and folders
 * open, scan and render correctly. */
static WCHAR *u8tow(const char *s) {
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) n = 1;
    WCHAR *w = (WCHAR*)malloc((size_t)n * sizeof(WCHAR));
    if (w) { if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) w[0] = 0; }
    return w;
}
static char *wtou8(const WCHAR *w) {
    if (!w) w = L"";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) n = 1;
    char *s = (char*)malloc((size_t)n);
    if (s) { if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) s[0] = 0; }
    return s;
}
static void set_text_u8(HWND h, const char *s) { WCHAR *w = u8tow(s); if (w) { SetWindowTextW(h, w); free(w); } }
/* Only touch the control when the value actually changed (mirrors the Qt
 * frontend's setTextIf/setEnabledIf) - avoids per-frame repaint flicker. */
static void set_text_if(HWND h, char *cache, size_t csz, const char *val) { if (!val) val = ""; if (strncmp(cache, val, csz) != 0) { safe_copy(cache, csz, val); set_text_u8(h, val); } }
static void enable_if(HWND h, int *cache, int on) { on = !!on; if (*cache != on) { *cache = on; EnableWindow(h, on); } }
static FILE *u8fopen(const char *path, const char *mode) {
    WCHAR *wp = u8tow(path); WCHAR wm[16];
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 16);
    FILE *f = wp ? _wfopen(wp, wm) : NULL; free(wp); return f;
}

static double app_now_ms(App *a) {
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (double)(n.QuadPart - a->qpc_start.QuadPart) * 1000.0 / (double)a->qpc_freq.QuadPart;
}
static char *safe_copy(char *dst, size_t n, const char *src) { if (n) { strncpy(dst, src ? src : "", n - 1); dst[n - 1] = 0; } return dst; }
static const char *base_name(const char *p) { const char *a = strrchr(p, '\\'), *b = strrchr(p, '/'); const char *s = a > b ? a : b; return s ? s + 1 : p; }
static void get_dirname(const char *p, char *out, size_t n) { safe_copy(out,n,p); char *a=strrchr(out,'\\'), *b=strrchr(out,'/'); char *s=a>b?a:b; if(s) *s=0; else safe_copy(out,n,"."); }
static bool has_video_ext(const char *name) {
    const char *e = strrchr(name, '.'); if (!e) return false; e++;
    static const char *exts[] = {"mp4","m4v","mkv","webm","mov","avi","mpg","mpeg","ogv",NULL};
    for (int i = 0; exts[i]; i++) if (_stricmp(e, exts[i]) == 0) return true;
    return false;
}
static void fmt_time(double t, char *out, size_t n) {
    if (!(t >= 0)) { snprintf(out,n,"--:--"); return; }
    int m = (int)(t / 60.0); double s = t - m * 60.0;
    snprintf(out, n, "%02d:%04.1f", m, s);
}

static void app_set_status(const char *text) {
    if (g_app.status_label) set_text_if(g_app.status_label, g_app.c_status, sizeof(g_app.c_status), text ? text : "");
}

static void progress_cb(double pct, const char *text, void *user) {
    (void)user;
    char buf[512]; snprintf(buf, sizeof(buf), "%.0f%%  %s", pct, text ? text : "");
    app_set_status(buf);
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

/* ---- host callbacks ---- */
static void h_seek(void *u, double s) { App *a=(App*)u; if (a->player) tb_player_seek(a->player, s); }
static void h_play(void *u) { App *a=(App*)u; if (a->player) tb_player_play(a->player); }
static void h_pause(void *u) { App *a=(App*)u; if (a->player) tb_player_pause(a->player); }
static void h_rate(void *u, double r) { App *a=(App*)u; if (a->player) tb_player_set_rate(a->player, r); }
static void h_muted(void *u, bool m) { App *a=(App*)u; if (a->player) tb_player_set_muted(a->player, m); }
static double h_time(void *u) { App *a=(App*)u; return a->player ? tb_player_time(a->player) : 0; }
static double h_duration(void *u) { App *a=(App*)u; return a->player ? tb_player_duration(a->player) : 0; }
static bool h_is_paused(void *u) { App *a=(App*)u; return a->player ? tb_player_is_paused(a->player) : true; }
static void h_sfx(void *u, tb_sfx s) { App *a=(App*)u; tb_sfx_play(a->sfx, s); }
static double h_now(void *u) { return app_now_ms((App*)u); }
static double h_rand(void *u) { (void)u; return rand() / ((double)RAND_MAX + 1.0); }
static void h_mania_music(void *u, bool on) { App *a=(App*)u; a->mania_music = on; tb_sfx_music(a->sfx, on, 0); }

static void build_game(App *a) {
    if (a->game) { tb_game_destroy(a->game); a->game = NULL; }
    tb_host host = { h_seek, h_play, h_pause, h_rate, h_muted, h_time, h_duration, h_is_paused, h_sfx, h_now, h_rand, h_mania_music };
    a->game = tb_game_create(&a->events, &host, a);
    tb_game_set_mode(a->game, a->mode);
}

static void input_action(tb_action act, void *user) {
    App *a = (App*)user;
    switch (act) {
        case TB_ACT_ANS1: if (a->game) tb_game_on_answer(a->game, 1); break;
        case TB_ACT_ANS2: if (a->game) tb_game_on_answer(a->game, 2); break;
        case TB_ACT_ANS3: if (a->game) tb_game_on_answer(a->game, 3); break;
        case TB_ACT_ANS4: if (a->game) tb_game_on_answer(a->game, 4); break;
        case TB_ACT_PICKUP: if (a->game) tb_game_on_pickup(a->game); break;
        case TB_ACT_PAUSE: SendMessageW(a->hwnd, WM_COMMAND, ID_BTN_PLAY, 0); break;
        case TB_ACT_FULLSCREEN: SendMessageW(a->hwnd, WM_COMMAND, ID_VIEW_FULLSCREEN, 0); break;
        default: break;
    }
}

#if defined(TB_WIN32_USE_WINMM_INPUT)
static tb_action keyboard_action_for_vk(App *a, WPARAM vk) {
    for (int i = 0; i < TB_ACT_COUNT; i++) if (a->keymap[i] == (int)vk) return (tb_action)i;
    return TB_ACT_NONE;
}

static void winmm_input_init(App *a) {
    a->joy_present = false;
    a->joy_id = 0;
    a->joy_buttons = 0;

    UINT count = joyGetNumDevs();
    for (UINT id = 0; id < count; id++) {
        JOYCAPSA caps;
        if (joyGetDevCapsA(id, &caps, sizeof(caps)) != JOYERR_NOERROR) continue;
        JOYINFOEX ji;
        memset(&ji, 0, sizeof(ji));
        ji.dwSize = sizeof(ji);
        ji.dwFlags = JOY_RETURNBUTTONS;
        if (joyGetPosEx(id, &ji) == JOYERR_NOERROR) {
            a->joy_id = id;
            a->joy_buttons = ji.dwButtons;
            a->joy_present = true;
            break;
        }
    }
}

static tb_action winmm_button_action(unsigned button) {
    switch (button) {
        case 0: return TB_ACT_ANS1;
        case 1: return TB_ACT_ANS2;
        case 2: return TB_ACT_ANS3;
        case 3: return TB_ACT_ANS4;
        case 4:
        case 8: return TB_ACT_PICKUP;
        case 5:
        case 9: return TB_ACT_PAUSE;
        case 6:
        case 10: return TB_ACT_FULLSCREEN;
        default: return TB_ACT_NONE;
    }
}

static void winmm_input_poll(App *a) {
    if (!a->joy_present) {
        winmm_input_init(a);
        return;
    }

    JOYINFOEX ji;
    memset(&ji, 0, sizeof(ji));
    ji.dwSize = sizeof(ji);
    ji.dwFlags = JOY_RETURNBUTTONS;
    if (joyGetPosEx(a->joy_id, &ji) != JOYERR_NOERROR) {
        a->joy_present = false;
        a->joy_buttons = 0;
        return;
    }

    DWORD pressed = ji.dwButtons & ~a->joy_buttons;
    a->joy_buttons = ji.dwButtons;
    for (unsigned i = 0; i < 32; i++) {
        DWORD bit = (DWORD)(1UL << i);
        if (pressed & bit) {
            tb_action act = winmm_button_action(i);
            if (act != TB_ACT_NONE) input_action(act, a);
        }
    }
}
#endif

static bool save_allowed(App *a) { return a->ready && a->game && tb_game_mode_allows_save(tb_game_mode(a->game)); }
static void state_path(App *a, char *out, size_t n) {
    char dir[MAX_PATH * 4]; snprintf(dir, sizeof(dir), "%s\\states", a->exe_dir);
    WCHAR *wdir = u8tow(dir); if (wdir) { CreateDirectoryW(wdir, NULL); free(wdir); }
    char key[512]; safe_copy(key, sizeof(key), a->file_name);
    for (char *p = key; *p; p++) { if ((*p >= 'A' && *p <= 'Z')) *p = (char)(*p - 'A' + 'a'); else if (!((*p>='a'&&*p<='z') || (*p>='0'&&*p<='9'))) *p = '_'; }
    snprintf(out, n, "%s\\%s_%lld.tbs", dir, key, (long long)a->file_size);
}
static bool file_exists(const char *p) { WCHAR *w = u8tow(p); DWORD at = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES; free(w); return at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY); }
static bool dir_exists(const char *p) { WCHAR *w = u8tow(p); DWORD at = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES; free(w); return at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY); }
static bool has_saved_state(App *a) { char p[MAX_PATH*4]; state_path(a,p,sizeof(p)); return file_exists(p); }

static bool app_save_state(App *a) {
    if (!save_allowed(a)) return false;
    tb_savestate st; if (!tb_game_snapshot(a->game, &st)) return false;
    char path[MAX_PATH * 4]; state_path(a, path, sizeof(path));
    FILE *f = u8fopen(path, "wb"); if (!f) { tb_savestate_free(&st); return false; }
    fwrite(&st.version, sizeof(st.version), 1, f); fwrite(&st.mode, sizeof(st.mode), 1, f); fwrite(&st.time, sizeof(st.time), 1, f);
    fwrite(&st.cur_idx, sizeof(st.cur_idx), 1, f); fwrite(&st.score, sizeof(st.score), 1, f); fwrite(&st.streak, sizeof(st.streak), 1, f);
    fwrite(&st.best, sizeof(st.best), 1, f); fwrite(&st.lives, sizeof(st.lives), 1, f); fwrite(&st.correct, sizeof(st.correct), 1, f);
    fwrite(&st.wrong, sizeof(st.wrong), 1, f); fwrite(&st.pause_lock_until, sizeof(st.pause_lock_until), 1, f); fwrite(&st.n_play, sizeof(st.n_play), 1, f);
    fwrite(st.done, 1, (size_t)st.n_play, f); fwrite(st.got, sizeof(int), (size_t)st.n_play, f);
    fclose(f); tb_savestate_free(&st); return true;
}
static bool app_load_state(App *a) {
    if (!save_allowed(a) || !has_saved_state(a)) return false;
    char path[MAX_PATH * 4]; state_path(a, path, sizeof(path)); FILE *f = u8fopen(path, "rb"); if (!f) return false;
    tb_savestate st; memset(&st, 0, sizeof(st));
    fread(&st.version,sizeof(st.version),1,f); fread(&st.mode,sizeof(st.mode),1,f); fread(&st.time,sizeof(st.time),1,f);
    fread(&st.cur_idx,sizeof(st.cur_idx),1,f); fread(&st.score,sizeof(st.score),1,f); fread(&st.streak,sizeof(st.streak),1,f);
    fread(&st.best,sizeof(st.best),1,f); fread(&st.lives,sizeof(st.lives),1,f); fread(&st.correct,sizeof(st.correct),1,f);
    fread(&st.wrong,sizeof(st.wrong),1,f); fread(&st.pause_lock_until,sizeof(st.pause_lock_until),1,f); fread(&st.n_play,sizeof(st.n_play),1,f);
    if (st.n_play < 0 || st.n_play > 100000) { fclose(f); return false; }
    st.done = (unsigned char*)malloc((size_t)st.n_play + 1); st.got = (int*)malloc(((size_t)st.n_play + 1) * sizeof(int));
    fread(st.done, 1, (size_t)st.n_play, f); fread(st.got, sizeof(int), (size_t)st.n_play, f); fclose(f);
    a->mode = st.mode; bool ok = tb_game_restore(a->game, &st); tb_savestate_free(&st); return ok;
}

static void app_free_media(App *a) {
    if (a->game) { tb_game_destroy(a->game); a->game = NULL; }
    if (a->player) { tb_player_close(a->player); a->player = NULL; }
    tb_event_list_free(&a->events); tb_cue_list_free(&a->cues); memset(&a->events,0,sizeof(a->events)); memset(&a->cues,0,sizeof(a->cues));
    if (a->sfx) tb_sfx_set_music(a->sfx, NULL, 0, 0);
    free(a->music_pcm); a->music_pcm = NULL; a->music_frames = 0; a->music_sr = 0;
    a->game_name[0] = 0; a->subtitle_label[0] = 0; a->ready = false; a->mania_music = false; a->results_shown = false;
    tb_win32_audio_clear_media(); tb_win32_audio_set_media_muted(false); tb_win32_audio_music(false, 0); tb_win32_audio_set_running(false);
}

static void app_choose_difficulty(App *a, const char *pending) { if (a->game) tb_game_open_difficulty(a->game, pending); }

static void app_load_file(App *a, const char *path) {
    a->loading = true; app_free_media(a);
    safe_copy(a->file_path, sizeof(a->file_path), path); safe_copy(a->file_name, sizeof(a->file_name), base_name(path));
    WIN32_FILE_ATTRIBUTE_DATA fad; a->file_size = 0;
    WCHAR *wpath = u8tow(path);
    if (wpath && GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad)) a->file_size = ((int64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    free(wpath);
    a->show_gallery = false; app_layout(a); app_set_status("Opening media...");

    char err[512] = {0}; double dur = 0; int w = 0, h = 0;
    if (!tb_media_probe(path, &dur, &w, &h, err, sizeof(err))) { MessageBoxA(a->hwnd, err, "Load failed", MB_ICONERROR); a->loading = false; return; }
    tb_audio_channels au; memset(&au, 0, sizeof(au));
    if (!tb_media_decode_audio(path, &au, err, sizeof(err), progress_cb, a)) { MessageBoxA(a->hwnd, err, "Load failed", MB_ICONERROR); a->loading = false; return; }

    int best_ans = -1; double best_car = 8000; tb_packet_list best; tb_packet_list_init(&best);
    int maxch = au.nch < 8 ? au.nch : 8;
    for (int c = 0; c < maxch; c++) {
        tb_packet_list pk; tb_packet_list_init(&pk);
        double car = tb_decode_channel(au.chan[c], au.nsamples, au.sample_rate, 0, &pk, NULL, NULL);
        int ans = 0; for (size_t i = 0; i < pk.count; i++) if (pk.data[i].answer >= 1 && pk.data[i].answer <= 4) ans++;
        if (ans > best_ans) { best_ans = ans; best_car = car; tb_packet_list_free(&best); best = pk; }
        else tb_packet_list_free(&pk);
    }
    a->carrier = best_car; a->duration = dur;
    tb_event_list_init(&a->events); tb_build_events(&best, best_car, &a->events); tb_packet_list_free(&best);

    int game_idx = tb_gamedb_detect(&a->events);
    if (game_idx >= 0) safe_copy(a->game_name, sizeof(a->game_name), tb_gamedb_name(game_idx));
    char dir[MAX_PATH * 4]; get_dirname(path, dir, sizeof(dir));
    char music[MAX_PATH * 4]; snprintf(music, sizeof(music), "%s\\music.mp3", dir);
    if (file_exists(music)) tb_mania_music_from_file(music, &a->music_pcm, &a->music_frames, &a->music_sr);
    else if (game_idx >= 0 && tb_gamedb_mania_anchor(game_idx)) {
        int ch = au.nch > 1 ? 1 : 0;
        tb_mania_music_detect_clip(au.chan[ch], au.nsamples, au.sample_rate, &a->events, tb_gamedb_mania_anchor(game_idx), &a->music_pcm, &a->music_frames, &a->music_sr);
    }
    tb_audio_channels_free(&au);

    tb_cue_list_init(&a->cues);
    char sub[MAX_PATH * 4];
    if (tb_find_sidecar_subtitle(path, sub, sizeof(sub)) && tb_load_sidecar_subtitles(sub, &a->cues)) snprintf(a->subtitle_label, sizeof(a->subtitle_label), "Sidecar: %s", base_name(sub));
    else if (tb_media_extract_subtitles(path, &a->cues)) snprintf(a->subtitle_label, sizeof(a->subtitle_label), "Embedded subtitles");

    app_set_status("Opening playback...");
    a->player = tb_player_open(path, a->hwaccel, err, sizeof(err));
    if (!a->player) { MessageBoxA(a->hwnd, err, "Playback open failed", MB_ICONERROR); a->loading = false; return; }
    build_game(a); if (a->sfx) tb_sfx_set_music(a->sfx, a->music_pcm, a->music_frames, a->music_sr);
    a->ready = true; tb_player_seek(a->player, fmin(4.0, fmax(0.0, a->duration - 0.1)));
    char status[512]; snprintf(status, sizeof(status), "Scan complete. %.0f kHz carrier. %s%s", best_car/1000.0, a->game_name[0]?a->game_name:"Unknown game", a->subtitle_label[0]?" \xc2\xb7 subtitles":"");
    app_set_status(status); app_choose_difficulty(a, "start");
    set_text_u8(a->hwnd, a->file_name);
    a->loading = false; app_update_controls(a); InvalidateRect(a->hwnd, NULL, FALSE);
}

static void app_toggle_pause(App *a) {
    if (!a->game || !a->player) return;
    if (tb_game_pause_locked(a->game)) return;
    const tb_ui *u = tb_game_ui(a->game); if (u && u->difficulty_overlay) return;
    if (tb_player_is_paused(a->player)) {
        if (!tb_game_running(a->game) && !tb_game_ended(a->game)) tb_game_start(a->game, tb_player_time(a->player) <= 0.2);
        else tb_player_play(a->player);
    } else tb_player_pause(a->player);
}

/* ---------------- cover-art thumbnails ----------------
 * Loads sibling cover art (png/jpg/jpeg, found by tb_find_cover_art) via
 * OleLoadPicturePath and scales it, aspect-fit and letterboxed on the dark card
 * colour, into a fixed THUMB_W x THUMB_H 32-bit bitmap for the gallery image
 * list - mirroring the Qt Gallery's KeepAspectRatio cover tiles. */
static HBITMAP make_thumbnail(const char *cover_u8) {
    HDC screen = GetDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = THUMB_W; bi.bmiHeader.biHeight = -THUMB_H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HDC mdc = CreateCompatibleDC(screen);
    HGDIOBJ oldb = SelectObject(mdc, dib);
    RECT full = {0,0,THUMB_W,THUMB_H};
    HBRUSH bg = CreateSolidBrush(COL_PANEL); FillRect(mdc, &full, bg); DeleteObject(bg);

    if (cover_u8 && cover_u8[0]) {
        WCHAR *wc = u8tow(cover_u8);
        IPicture *pic = NULL;
        if (wc && OleLoadPicturePath(wc, NULL, 0, 0, &IID_IPicture, (void**)&pic) == S_OK && pic) {
            OLE_HANDLE oh = 0; IPicture_get_Handle(pic, &oh);
            HBITMAP src = (HBITMAP)(UINT_PTR)oh; BITMAP bm;
            if (src && GetObjectW(src, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
                HDC sdc = CreateCompatibleDC(screen); HGDIOBJ os = SelectObject(sdc, src);
                double sx = (double)THUMB_W / bm.bmWidth, sy = (double)THUMB_H / bm.bmHeight;
                double s = sx < sy ? sx : sy;
                int dw = (int)(bm.bmWidth * s + 0.5), dh = (int)(bm.bmHeight * s + 0.5);
                int dx = (THUMB_W - dw) / 2, dy = (THUMB_H - dh) / 2;
                SetStretchBltMode(mdc, HALFTONE); SetBrushOrgEx(mdc, 0, 0, NULL);
                StretchBlt(mdc, dx, dy, dw, dh, sdc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                SelectObject(sdc, os); DeleteDC(sdc);
            }
            IPicture_Release(pic);
        }
        free(wc);
    }
    SelectObject(mdc, oldb); DeleteDC(mdc); ReleaseDC(NULL, screen);
    return dib;
}

static void gallery_clear(App *a) {
    free(a->gallery_items); a->gallery_items = NULL; a->gallery_count = 0;
    if (a->gallery_list) ListView_DeleteAllItems(a->gallery_list);
    if (a->gallery_images) { ImageList_Destroy(a->gallery_images); a->gallery_images = NULL; }
    a->gallery_images = ImageList_Create(THUMB_W, THUMB_H, ILC_COLOR32, 0, 64);
    if (a->gallery_list) ListView_SetImageList(a->gallery_list, a->gallery_images, LVSIL_NORMAL);
}
static void gallery_add(App *a, const char *path, const char *label) {
    GalleryItem *ni = (GalleryItem*)realloc(a->gallery_items, ((size_t)a->gallery_count + 1) * sizeof(GalleryItem)); if (!ni) return;
    a->gallery_items = ni;
    safe_copy(a->gallery_items[a->gallery_count].path, sizeof(a->gallery_items[a->gallery_count].path), path);
    safe_copy(a->gallery_items[a->gallery_count].label, sizeof(a->gallery_items[a->gallery_count].label), label);

    char cover[MAX_PATH * 4]; int img = -1;
    if (tb_find_cover_art(path, cover, sizeof(cover))) {} else cover[0] = 0;
    HBITMAP thumb = make_thumbnail(cover[0] ? cover : NULL);
    if (thumb) { img = ImageList_Add(a->gallery_images, thumb, NULL); DeleteObject(thumb); }

    WCHAR *wl = u8tow(label);
    LVITEMW it; memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT | LVIF_PARAM | (img >= 0 ? LVIF_IMAGE : 0);
    it.iItem = a->gallery_count; it.pszText = wl ? wl : L""; it.iImage = img; it.lParam = a->gallery_count;
    if (a->gallery_list) SendMessageW(a->gallery_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
    free(wl);
    a->gallery_count++;
}
static void scan_dir_files(App *a, const char *dir, const char *prefix) {
    char pat[MAX_PATH * 4]; snprintf(pat, sizeof(pat), "%s\\*", dir);
    WCHAR *wpat = u8tow(pat); if (!wpat) return;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(wpat, &fd); free(wpat);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char *name = wtou8(fd.cFileName);
        if (name && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && has_video_ext(name)) {
            char path[MAX_PATH * 4]; snprintf(path, sizeof(path), "%s\\%s", dir, name);
            char label[512]; safe_copy(label, sizeof(label), name); char *dot = strrchr(label,'.'); if(dot) *dot=0;
            if (prefix && prefix[0]) { char tmp[512]; snprintf(tmp, sizeof(tmp), "%s / %s", prefix, label); safe_copy(label, sizeof(label), tmp); }
            gallery_add(a, path, label);
        }
        free(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void gallery_scan(App *a, const char *dir) {
    gallery_clear(a); safe_copy(a->gallery_dir, sizeof(a->gallery_dir), dir);
    scan_dir_files(a, dir, "");
    char pat[MAX_PATH * 4]; snprintf(pat, sizeof(pat), "%s\\*", dir);
    WCHAR *wpat = u8tow(pat);
    WIN32_FIND_DATAW fd; HANDLE h = wpat ? FindFirstFileW(wpat, &fd) : INVALID_HANDLE_VALUE; free(wpat);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char *name = wtou8(fd.cFileName);
            if (name && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strcmp(name,".") && strcmp(name,"..")) {
                char child[MAX_PATH * 4]; snprintf(child, sizeof(child), "%s\\%s", dir, name);
                scan_dir_files(a, child, name);
            }
            free(name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    char hdr[1024]; snprintf(hdr, sizeof(hdr), "Gallery: %s  (%d videos, including one folder deep) - double-click to play", dir, a->gallery_count);
    set_text_u8(a->gallery_header, hdr);
}

/* Persist the last gallery folder under HKCU\Software\TerebikkoEmu so it is
 * restored on the next launch (the Qt frontend does the same with QSettings). */

static bool reg_load_dword(const WCHAR *name, DWORD *out) {
    HKEY k; DWORD type = 0, sz = sizeof(DWORD), v = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\TerebikkoEmu", 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool ok = (RegQueryValueExW(k, name, NULL, &type, (BYTE*)&v, &sz) == ERROR_SUCCESS && type == REG_DWORD && sz == sizeof(DWORD));
    RegCloseKey(k);
    if (ok && out) *out = v;
    return ok;
}
static void reg_save_dword(const WCHAR *name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\TerebikkoEmu", 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
}
static void app_load_prefs(App *a) {
    DWORD v = 0;
    a->fullscreen_controls_panel = true;
    if (reg_load_dword(L"FullscreenControlsPanel", &v)) a->fullscreen_controls_panel = (v != 0);
}
static void app_save_prefs(App *a) {
    reg_save_dword(L"FullscreenControlsPanel", a->fullscreen_controls_panel ? 1u : 0u);
}

static void reg_save_gallery(const char *path) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\TerebikkoEmu", 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        WCHAR *w = u8tow(path);
        if (w) { RegSetValueExW(k, L"GalleryPath", 0, REG_SZ, (const BYTE*)w, (DWORD)((wcslen(w) + 1) * sizeof(WCHAR))); free(w); }
        RegCloseKey(k);
    }
}
static bool reg_load_gallery(char *out, size_t outsz) {
    HKEY k; bool ok = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\TerebikkoEmu", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        WCHAR buf[2048]; DWORD sz = sizeof(buf), type = 0;
        if (RegQueryValueExW(k, L"GalleryPath", NULL, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS && type == REG_SZ) {
            char *u = wtou8(buf); if (u) { safe_copy(out, outsz, u); free(u); ok = out[0] != 0; }
        }
        RegCloseKey(k);
    }
    return ok;
}

static void draw_text_outlined(HDC hdc, RECT rc, const char *text, UINT flags, HFONT font, COLORREF color) {
    if (!text || !text[0]) return;
    WCHAR *w = u8tow(text); if (!w) return;
    HFONT old = (HFONT)SelectObject(hdc, font); SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0,0,0));
    for (int dx=-2; dx<=2; dx++) for (int dy=-2; dy<=2; dy++) { RECT r=rc; OffsetRect(&r,dx,dy); DrawTextW(hdc,w,-1,&r,flags); }
    SetTextColor(hdc, color); DrawTextW(hdc,w,-1,&rc,flags);
    SelectObject(hdc, old);
    free(w);
}
static RECT fit_rect(RECT area, int fw, int fh) {
    if (fw <= 0 || fh <= 0) return area;
    int aw = area.right - area.left, ah = area.bottom - area.top;
    int w = aw, h = (int)((double)w * fh / fw);
    if (h > ah) { h = ah; w = (int)((double)h * fw / fh); }
    RECT r; r.left = area.left + (aw - w) / 2; r.top = area.top + (ah - h) / 2; r.right = r.left + w; r.bottom = r.top + h; return r;
}
static void current_subtitle(App *a, char *out, size_t n) {
    out[0] = 0; if (!a->cues.count || !a->game) return; const tb_ui *u = tb_game_ui(a->game); if (!u) return;
    if (!tb_game_running(a->game) && !tb_game_ended(a->game)) return;
    double t = a->player ? tb_player_time(a->player) : 0;
    for (size_t i = 0; i < a->cues.count; i++) {
        const tb_cue *c = &a->cues.data[i]; if (c->start > t) break;
        if (t >= c->start && t < c->end) { if (out[0]) strncat(out, "\n", n - strlen(out) - 1); strncat(out, c->text, n - strlen(out) - 1); }
    }
}

/* Semi-transparent solid fill (e.g. the dimmed backdrop behind PICK UP / BREAK),
 * matching the Qt frontend's translucent QColor fills. */
static void fill_alpha(HDC hdc, RECT r, COLORREF col, int alpha) {
    int w = r.right - r.left, h = r.bottom - r.top; if (w <= 0 || h <= 0) return;
    HDC mdc = CreateCompatibleDC(hdc);
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = 1; bi.bmiHeader.biHeight = 1;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL; HBITMAP bm = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bm && bits) {
        *(DWORD*)bits = ((DWORD)GetRValue(col) << 16) | ((DWORD)GetGValue(col) << 8) | GetBValue(col);
        HGDIOBJ ob = SelectObject(mdc, bm);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)alpha, 0 };
        AlphaBlend(hdc, r.left, r.top, w, h, mdc, 0, 0, 1, 1, bf);
        SelectObject(mdc, ob);
    }
    if (bm) DeleteObject(bm);
    DeleteDC(mdc);
}

static void draw_overlays(App *a, RECT vid, HDC hdc) {
    RECT area = a->video_rect;          /* full picture region (overlays cover this) */
    int H = area.bottom - area.top;
    HFONT big = CreateFontW(-TB_MAX(22, H/10),0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    HFONT med = CreateFontW(-TB_MAX(16, H/18),0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    HFONT small = CreateFontW(-TB_MAX(12, H/28),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    char sub[4096]; current_subtitle(a, sub, sizeof(sub));
    if (sub[0]) { RECT r = vid; r.left += (vid.right-vid.left)/16; r.right -= (vid.right-vid.left)/16; r.bottom -= (vid.bottom-vid.top)/18; draw_text_outlined(hdc, r, sub, DT_CENTER|DT_BOTTOM|DT_WORDBREAK, med, RGB(255,255,255)); }
    const tb_ui *u = a->game ? tb_game_ui(a->game) : NULL;
    if (u) {
        if (u->call_overlay) {
            fill_alpha(hdc, area, RGB(8,9,20), 150);   /* translucent backdrop behind PICK UP */
            RECT r = area; r.top += H/2 - 60; draw_text_outlined(hdc, r, u->call_title, DT_CENTER|DT_TOP, med, RGB(255,209,102)); r.top += H/12; draw_text_outlined(hdc, r, u->call_hint, DT_CENTER|DT_TOP, small, RGB(210,210,210));
        }
        if (u->status_overlay) {
            /* BREAK (and other neutral mania prompts) get the same translucent backdrop. */
            if (u->status_kind == TB_STATUS_WARN || _stricmp(u->status_text, "break") == 0) fill_alpha(hdc, area, RGB(8,9,20), 150);
            RECT r = area; COLORREF c = u->status_kind == TB_STATUS_GOOD ? RGB(55,217,154) : (u->status_kind == TB_STATUS_BAD ? RGB(255,93,116) : RGB(255,209,102));
            draw_text_outlined(hdc, r, u->status_text, DT_CENTER|DT_VCENTER|DT_SINGLELINE, big, c);
        }
        if (u->game_over) {
            fill_alpha(hdc, area, RGB(0,0,0), 200);
            RECT r = area; draw_text_outlined(hdc, r, u->game_over_text, DT_CENTER|DT_VCENTER|DT_SINGLELINE, big, RGB(255,255,255)); r.top += H/2+50; draw_text_outlined(hdc, r, u->game_over_sub, DT_CENTER|DT_TOP, small, RGB(210,210,210));
        }
        if (u->results) {
            fill_alpha(hdc, area, RGB(6,7,16), 230);
            RECT r = area; r.top += H/2 - 110; draw_text_outlined(hdc, r, u->result_title, DT_CENTER|DT_TOP, med, RGB(255,255,255));
            char sc[64]; snprintf(sc,sizeof(sc),"%ld",u->result_score); r.top += H/12; draw_text_outlined(hdc, r, sc, DT_CENTER|DT_TOP, big, RGB(55,217,154)); r.top += H/8; draw_text_outlined(hdc, r, u->result_line, DT_CENTER|DT_TOP|DT_WORDBREAK, small, RGB(210,210,210));
        }
        if (u->difficulty_overlay) {
            HBRUSH br = CreateSolidBrush(RGB(0,0,0)); FillRect(hdc, &area, br); DeleteObject(br);
            RECT title = area; title.top += H*3/10; draw_text_outlined(hdc, title, "Choose a difficulty to start", DT_CENTER|DT_TOP, med, RGB(124,156,255));
            const char *dn[4] = {"Easy","Hard","Hard+","Minigame"}; COLORREF dc[4] = {COL_ANS2,COL_ANS1,COL_ANS4,COL_BADGE_FG};
            int vw=area.right-area.left, bh=TB_MAX(44,H/10), bw=TB_MIN(vw/5,220), gap=bw/8, total=bw*4+gap*3, x=area.left+(vw-total)/2, y=area.top+H/2;
            for (int i=0;i<4;i++) { a->diff_rect[i].left=x+i*(bw+gap); a->diff_rect[i].top=y; a->diff_rect[i].right=a->diff_rect[i].left+bw; a->diff_rect[i].bottom=y+bh; HBRUSH bb=CreateSolidBrush(dc[i]); FillRect(hdc,&a->diff_rect[i],bb); DeleteObject(bb); draw_text_outlined(hdc,a->diff_rect[i],dn[i],DT_CENTER|DT_VCENTER|DT_SINGLELINE,small,RGB(255,255,255)); }
        } else for (int i=0;i<4;i++) SetRectEmpty(&a->diff_rect[i]);
    }
    DeleteObject(big); DeleteObject(med); DeleteObject(small);
}

static void app_render(App *a, HDC paint_dc) {
    if (a->show_gallery) return;
    const tb_ui *u = a->game ? tb_game_ui(a->game) : NULL;
    bool blank = u && u->difficulty_overlay;   /* anti-cheat blanking during difficulty pick */
    if (u && u->transition_seq != a->last_trans_seq) {
        if (u->mode == TB_GM_MANIA && !blank) tb_win32_video_begin_push(a->video, u->transition_dir);
        a->last_trans_seq = u->transition_seq;
    }
    RECT area = a->video_rect;
    tb_frame f; memset(&f,0,sizeof(f)); if (a->player && !blank) f = tb_player_acquire_frame(a->player);
    RECT dst = f.valid ? fit_rect(area, f.width, f.height) : area;
    tb_win32_video_present(a->video, &dst, &f, true);
    HDC hdc = tb_win32_video_overlay_dc(a->video);
    if (hdc) draw_overlays(a, dst, hdc);
    if (paint_dc) tb_win32_video_finish(a->video, paint_dc, &area);
    else { HDC wdc = GetDC(a->hwnd); tb_win32_video_finish(a->video, wdc, &area); ReleaseDC(a->hwnd, wdc); }
}

static void app_update_controls(App *a) {
    const tb_ui *u = a->game ? tb_game_ui(a->game) : NULL;
    char buf[256];
    if (u) {
        snprintf(buf,sizeof(buf),"%ld",u->score); set_text_if(a->hud_score, a->c_score, sizeof(a->c_score), buf);
        snprintf(buf,sizeof(buf),"%ldx",u->streak); set_text_if(a->hud_streak, a->c_streak, sizeof(a->c_streak), buf);
        if (u->mode == TB_GM_MANIA) snprintf(buf,sizeof(buf),"HI %ld",u->mania_high);
        else if (u->lives >= 1000000) snprintf(buf,sizeof(buf),"\xe2\x88\x9e");   /* infinity */
        else { int n = u->lives; if (n < 0) n = 0; if (n > 12) n = 12; buf[0]=0; for (int i=0;i<n;i++) strcat(buf,"\xe2\x99\xa5"); if(!buf[0]) strcpy(buf,"-"); }
        set_text_if(a->hud_lives, a->c_lives, sizeof(a->c_lives), buf);
        if (u->mode == TB_GM_MANIA) snprintf(buf,sizeof(buf),"R %d",u->mania_round); else snprintf(buf,sizeof(buf),"%d/%d",u->correct,u->n_questions); set_text_if(a->hud_correct, a->c_correct, sizeof(a->c_correct), buf);
        const char *mn[] = {"EASY","HARD","HARD+","MINIGAME"}; set_text_if(a->mode_badge, a->c_badge, sizeof(a->c_badge), mn[u->mode]);
        for(int i=0;i<4;i++) enable_if(a->btn_ans[i], &a->e_ans[i], u->buttons_enabled);
        enable_if(a->btn_pickup, &a->e_pickup, u->pickup_enabled);
        enable_if(a->btn_play, &a->e_play, a->ready && !tb_game_pause_locked(a->game));
    } else {
        enable_if(a->btn_play, &a->e_play, 0); for(int i=0;i<4;i++) enable_if(a->btn_ans[i], &a->e_ans[i], 0); enable_if(a->btn_pickup, &a->e_pickup, 0);
    }
    set_text_if(a->btn_play, a->c_play, sizeof(a->c_play), (a->ready && a->player && !tb_player_is_paused(a->player)) ? "Pause" : "Play");
    enable_if(a->btn_save, &a->e_save, save_allowed(a)); enable_if(a->btn_load, &a->e_load, save_allowed(a) && has_saved_state(a));
    char t0[32], t1[32]; fmt_time(a->player ? tb_player_time(a->player) : 0, t0, sizeof(t0)); fmt_time(a->duration, t1, sizeof(t1)); snprintf(buf,sizeof(buf),"%s / %s",t0,t1); set_text_if(a->time_label, a->c_time, sizeof(a->c_time), buf);
}

static void app_tick(App *a) {
#if defined(TB_WIN32_USE_SDL_INPUT)
    if (a->input) tb_input_poll(a->input, input_action, a);
#elif defined(TB_WIN32_USE_WINMM_INPUT)
    winmm_input_poll(a);
#endif
    if (a->sfx && a->mania_music) tb_sfx_tick(a->sfx, app_now_ms(a));
    if (a->game && a->player) {
        if (tb_player_ended(a->player) && tb_game_running(a->game)) tb_game_on_media_ended(a->game);
        const tb_ui *u = tb_game_ui(a->game); if (u) tb_sfx_set_round(a->sfx, u->mania_round);
        tb_game_tick(a->game);
    }
    app_update_controls(a);
    if (!a->show_gallery) InvalidateRect(a->hwnd, &a->video_rect, FALSE);
}

/* owner-drawn flat button, BS_OWNERDRAW, font applied per-control */
static HWND make_button(App *a, const wchar_t *text, int id, HFONT font) {
    HWND b = CreateWindowW(L"BUTTON", text, WS_CHILD|BS_OWNERDRAW, 0,0,80,30, a->hwnd, (HMENU)(INT_PTR)id, a->inst, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)font, TRUE);
    return b;
}
static HWND make_static(App *a, const wchar_t *text, int id, HFONT font) {
    HWND s = CreateWindowW(L"STATIC", text, WS_CHILD, 0,0,100,24, a->hwnd, (HMENU)(INT_PTR)id, a->inst, NULL);
    SendMessageW(s, WM_SETFONT, (WPARAM)font, TRUE);
    return s;
}

static void create_controls(App *a) {
    a->gallery_header = CreateWindowW(L"STATIC",L"Open a gallery folder from File > Open Gallery Folder.",WS_CHILD|WS_VISIBLE,0,0,100,24,a->hwnd,(HMENU)ID_GALLERY_HEADER,a->inst,NULL);
    SendMessageW(a->gallery_header, WM_SETFONT, (WPARAM)a->font_ui, TRUE);
    a->gallery_list = CreateWindowW(WC_LISTVIEWW,L"",WS_CHILD|WS_VISIBLE|WS_BORDER|LVS_ICON|LVS_SINGLESEL|LVS_AUTOARRANGE|LVS_SHAREIMAGELISTS,0,24,100,100,a->hwnd,(HMENU)ID_GALLERY_LIST,a->inst,NULL);
    ListView_SetExtendedListViewStyle(a->gallery_list, LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(a->gallery_list, COL_BG);
    ListView_SetTextBkColor(a->gallery_list, CLR_NONE);
    ListView_SetTextColor(a->gallery_list, COL_TEXT);
    ListView_SetIconSpacing(a->gallery_list, THUMB_W + 28, THUMB_H + 48);
    SendMessageW(a->gallery_list, WM_SETFONT, (WPARAM)a->font_ui, TRUE);
    a->gallery_images = ImageList_Create(THUMB_W, THUMB_H, ILC_COLOR32, 0, 64);
    ListView_SetImageList(a->gallery_list, a->gallery_images, LVSIL_NORMAL);

    a->btn_gallery = make_button(a, L"\x2190" L" Gallery", ID_BTN_GALLERY, a->font_btn);
    a->btn_play = make_button(a, L"Play", ID_BTN_PLAY, a->font_btn);
    a->btn_restart = make_button(a, L"Restart", ID_BTN_RESTART, a->font_btn);
    a->btn_save = make_button(a, L"Save", ID_BTN_SAVE, a->font_btn);
    a->btn_load = make_button(a, L"Load", ID_BTN_LOAD, a->font_btn);
    a->btn_fullscreen = make_button(a, L"Fullscreen", ID_BTN_FULLSCREEN, a->font_btn);
    a->time_label = make_static(a, L"00:00 / --:--", ID_STATIC_FIRST+1, a->font_ui);
    a->cap_score = make_static(a, L"SCORE", ID_STATIC_FIRST+10, a->font_small);
    a->cap_streak = make_static(a, L"STREAK", ID_STATIC_FIRST+11, a->font_small);
    a->cap_lives = make_static(a, L"LIVES", ID_STATIC_FIRST+12, a->font_small);
    a->cap_correct = make_static(a, L"CORRECT", ID_STATIC_FIRST+13, a->font_small);
    a->hud_score = make_static(a, L"0", ID_STATIC_FIRST+2, a->font_value);
    a->hud_streak = make_static(a, L"0x", ID_STATIC_FIRST+3, a->font_value);
    a->hud_lives = make_static(a, L"\x221e", ID_STATIC_FIRST+4, a->font_value);
    a->hud_correct = make_static(a, L"0/0", ID_STATIC_FIRST+5, a->font_value);
    a->mode_badge = make_static(a, L"EASY", ID_STATIC_FIRST+6, a->font_btn);
    a->status_label = CreateWindowW(L"STATIC",L"No video loaded",WS_CHILD|WS_VISIBLE,0,0,200,24,a->hwnd,(HMENU)(INT_PTR)(ID_STATIC_FIRST+7),a->inst,NULL);
    SendMessageW(a->status_label, WM_SETFONT, (WPARAM)a->font_ui, TRUE);
    for (int i=0;i<4;i++) { wchar_t n[2]={(wchar_t)('1'+i),0}; a->btn_ans[i]=make_button(a,n,ID_BTN_ANS1+i,a->font_bigbtn); }
    a->btn_pickup = make_button(a, L"\x260e" L" Pick up", ID_BTN_PICKUP, a->font_bigbtn);
}

static void show_player_controls(App *a, BOOL on) {
    HWND hs[] = {a->btn_gallery,a->btn_play,a->btn_restart,a->btn_save,a->btn_load,a->btn_fullscreen,a->time_label,a->cap_score,a->cap_streak,a->cap_lives,a->cap_correct,a->hud_score,a->hud_streak,a->hud_lives,a->hud_correct,a->mode_badge,a->status_label,a->btn_pickup,a->btn_ans[0],a->btn_ans[1],a->btn_ans[2],a->btn_ans[3]};
    for (unsigned i=0;i<sizeof(hs)/sizeof(hs[0]);i++) ShowWindow(hs[i], on?SW_SHOW:SW_HIDE);
    ShowWindow(a->gallery_list, on?SW_HIDE:SW_SHOW); ShowWindow(a->gallery_header,on?SW_HIDE:SW_SHOW);
}
static void app_layout(App *a) {
    RECT rc; GetClientRect(a->hwnd, &rc); int W=rc.right, H=rc.bottom; int statusH=24;
    if (a->show_gallery) {
        show_player_controls(a, FALSE); ShowWindow(a->status_label, SW_SHOW);
        MoveWindow(a->status_label, 8, H-statusH, W-16, statusH, TRUE);
        MoveWindow(a->gallery_header, 8, 8, W-16, 24, TRUE); MoveWindow(a->gallery_list, 8, 40, W-16, H-72, TRUE); return;
    }
    show_player_controls(a, TRUE);

    if (a->fullscreen) {
        /* Fullscreen: hide menu/transport/HUD/status.  The optional Terebikko
         * phone panel occupies its own black right-side lane; when disabled,
         * the video gets the whole monitor rectangle and fit_rect() keeps the
         * frame centered while preserving aspect ratio. */
        HWND hide[] = {a->btn_gallery,a->btn_play,a->btn_restart,a->btn_save,a->btn_load,a->btn_fullscreen,a->time_label,
                       a->cap_score,a->cap_streak,a->cap_lives,a->cap_correct,a->hud_score,a->hud_streak,a->hud_lives,a->hud_correct,a->mode_badge,a->status_label};
        for (unsigned i=0;i<sizeof(hide)/sizeof(hide[0]);i++) ShowWindow(hide[i], SW_HIDE);
        if (!a->fullscreen_controls_panel) {
            a->side_width = 0;
            a->video_rect.left = 0; a->video_rect.top = 0; a->video_rect.right = TB_MAX(1, W); a->video_rect.bottom = TB_MAX(1, H);
            ShowWindow(a->btn_pickup, SW_HIDE);
            for (int i = 0; i < 4; i++) ShowWindow(a->btn_ans[i], SW_HIDE);
            return;
        }
        /* Scale the fullscreen phone lane from the monitor size instead of
         * keeping it at a fixed 380 px.  4K displays need physically larger
         * controls, while small/narrow fullscreen windows must still reserve a
         * non-overlapping video area.  Clamp the lane to at most half the
         * client width so the controls can never be placed over the video. */
        int maxSide = TB_MAX(1, W / 2);
        int side = TB_CLAMP(H / 3, 330, 760);
        if (side > maxSide) side = maxSide;
        a->side_width = side;
        a->video_rect.left = 0; a->video_rect.top = 0; a->video_rect.right = TB_MAX(1, W - side); a->video_rect.bottom = TB_MAX(1, H);
        ShowWindow(a->btn_pickup, SW_SHOW);
        for (int i = 0; i < 4; i++) ShowWindow(a->btn_ans[i], SW_SHOW);

        int laneX = W - side;
        int marginX = TB_CLAMP(side / 12, 18, 54);
        int marginY = TB_CLAMP(H / 24, 12, 64);
        int gap = TB_CLAMP(TB_MIN(side, H) / 48, 10, 30);
        int pickupGap = gap + gap / 2;
        int blockW = TB_MAX(1, side - marginX * 2);
        int btnW = TB_MAX(1, (blockW - gap) / 2);

        int availH = TB_MAX(1, H - marginY * 2);
        int btnH = TB_CLAMP(TB_MIN(H / 7, btnW), 72, 360);
        int pickupH = TB_CLAMP((btnH * 6) / 5, 88, 430);
        int blockH = 2 * btnH + gap + pickupGap + pickupH;
        if (blockH > availH) {
            int noGapH = TB_MAX(1, availH - gap - pickupGap);
            btnH = TB_MAX(44, (noGapH * 5) / 16);       /* 2*btnH + 1.2*btnH */
            pickupH = TB_MAX(54, noGapH - 2 * btnH);
            blockH = 2 * btnH + gap + pickupGap + pickupH;
            if (blockH > availH) {
                gap = TB_MAX(4, gap / 2);
                pickupGap = TB_MAX(4, pickupGap / 2);
                blockH = 2 * btnH + gap + pickupGap + pickupH;
            }
        }

        int sx = laneX + (side - blockW) / 2;          /* centered in right lane */
        int sy = (H - blockH) / 2;                     /* centered vertically */
        if (sy < marginY) sy = marginY;
        for (int i = 0; i < 4; i++) MoveWindow(a->btn_ans[i], sx + (i % 2) * (btnW + gap), sy + (i / 2) * (btnH + gap), btnW, btnH, TRUE);
        sy += 2 * btnH + gap + pickupGap;
        MoveWindow(a->btn_pickup, sx, sy, blockW, pickupH, TRUE);
        return;
    }

    MoveWindow(a->status_label, 8, H-statusH, W-16, statusH, TRUE);
    int toolbarH = 42;

    /* Windowed player mode uses the same proportional Terebikko phone sizing
     * policy as fullscreen.  The old fixed 330 px lane and 78/90 px buttons
     * were too small on large/4K windows and could become badly aligned as the
     * client height changed.  Keep the lane non-overlapping by reserving it
     * from the right edge and clamping it to at most half the window width. */
    int playH = TB_MAX(1, H - toolbarH - statusH);
    int maxSide = TB_MAX(1, W / 2);
    int side = TB_CLAMP(playH / 3, 330, 700);
    if (side > maxSide) side = maxSide;
    a->side_width = side;
    a->video_rect.left = 0;
    a->video_rect.top = 0;
    a->video_rect.right = TB_MAX(1, W - side);
    a->video_rect.bottom = playH;

    int y = H - toolbarH - statusH + 6, x = 8;
    HWND tb[] = {a->btn_gallery,a->btn_play,a->btn_restart,a->btn_save,a->btn_load,a->btn_fullscreen}; int bw[] = {96,72,82,66,66,94};
    for (int i=0;i<6;i++) { MoveWindow(tb[i],x,y,bw[i],30,TRUE); x+=bw[i]+6; }
    MoveWindow(a->time_label, TB_MAX(8, W-side-176), y+6, 170,24, TRUE);

    int laneX = W - side;
    int laneH = playH;
    int marginX = TB_CLAMP(side / 24, 14, 38);
    int marginTop = TB_CLAMP(laneH / 48, 10, 32);
    int marginBot = TB_CLAMP(laneH / 48, 10, 32);
    int gap = TB_CLAMP(TB_MIN(side, laneH) / 60, 8, 24);
    int cardGap = TB_CLAMP(side / 32, 8, 20);
    int sx = laneX + marginX;
    int sy = marginTop;
    int blockW = TB_MAX(1, side - marginX * 2);
    int cardW = TB_MAX(1, (blockW - cardGap) / 2);

    MoveWindow(a->cap_score,sx,sy,cardW,16,TRUE); MoveWindow(a->cap_streak,sx+cardW+cardGap,sy,cardW,16,TRUE); sy+=18;
    MoveWindow(a->hud_score,sx,sy,cardW,28,TRUE); MoveWindow(a->hud_streak,sx+cardW+cardGap,sy,cardW,28,TRUE); sy+=34;
    MoveWindow(a->cap_lives,sx,sy,cardW,16,TRUE); MoveWindow(a->cap_correct,sx+cardW+cardGap,sy,cardW,16,TRUE); sy+=18;
    MoveWindow(a->hud_lives,sx,sy,cardW,28,TRUE); MoveWindow(a->hud_correct,sx+cardW+cardGap,sy,cardW,28,TRUE); sy+=40;
    MoveWindow(a->mode_badge,sx,sy,TB_MIN(blockW, 150 + side / 10),26,TRUE);

    int btnAreaTop = sy + TB_CLAMP(laneH / 36, 28, 72);
    int btnAreaBottom = TB_MAX(btnAreaTop + 1, laneH - marginBot);
    int availH = TB_MAX(1, btnAreaBottom - btnAreaTop);
    int btnW = TB_MAX(1, (blockW - gap) / 2);
    int btnH = TB_CLAMP(TB_MIN(laneH / 7, btnW), 60, 320);
    int pickupGap = gap + gap / 2;
    int pickupH = TB_CLAMP((btnH * 6) / 5, 72, 380);
    int phoneH = 2 * btnH + gap + pickupGap + pickupH;

    if (phoneH > availH) {
        gap = TB_MIN(gap, TB_MAX(2, availH / 32));
        pickupGap = TB_MIN(pickupGap, TB_MAX(2, availH / 24));
        int noGapH = TB_MAX(1, availH - gap - pickupGap);
        btnH = TB_MAX(12, (noGapH * 5) / 16);
        pickupH = TB_MAX(16, noGapH - 2 * btnH);
        phoneH = 2 * btnH + gap + pickupGap + pickupH;
        if (phoneH > availH) {
            btnH = TB_MAX(1, (availH - gap - pickupGap) / 4);
            pickupH = TB_MAX(1, availH - gap - pickupGap - 2 * btnH);
            phoneH = 2 * btnH + gap + pickupGap + pickupH;
        }
    }

    sy = btnAreaTop + (availH - phoneH) / 2;       /* vertically centered in remaining lane */
    if (sy < btnAreaTop) sy = btnAreaTop;
    for(int i=0;i<4;i++) MoveWindow(a->btn_ans[i], sx+(i%2)*(btnW+gap), sy+(i/2)*(btnH+gap), btnW, btnH, TRUE);
    sy += 2 * btnH + gap + pickupGap;
    MoveWindow(a->btn_pickup, sx, sy, blockW, pickupH, TRUE);
}

static void build_menu(App *a) {
    HMENU m=CreateMenu(), file=CreatePopupMenu(), state=CreatePopupMenu(), view=CreatePopupMenu(), help=CreatePopupMenu();
#ifndef TB_WIN32_WAVEOUT_ONLY
    HMENU audio=CreatePopupMenu();
#endif
#ifndef TB_WIN32_GDI_ONLY
    HMENU video=CreatePopupMenu(), hw=CreatePopupMenu();
#endif
    AppendMenuW(file,MF_STRING,ID_FILE_LOAD,L"Load Video..."); AppendMenuW(file,MF_STRING,ID_FILE_GALLERY,L"Open Gallery Folder..."); AppendMenuW(file,MF_SEPARATOR,0,NULL); AppendMenuW(file,MF_STRING,ID_FILE_EXIT,L"Exit");
    AppendMenuW(state,MF_STRING,ID_STATE_SAVE,L"Save State\tCtrl+S"); AppendMenuW(state,MF_STRING,ID_STATE_LOAD,L"Load State\tCtrl+L");
    AppendMenuW(view,MF_STRING,ID_VIEW_FULLSCREEN,L"Fullscreen\tF11"); AppendMenuW(view,MF_STRING,ID_VIEW_SETTINGS,L"Settings / Controls..."); AppendMenuW(view,MF_STRING,ID_VIEW_GALLERY,L"Show Gallery");
#ifndef TB_WIN32_GDI_ONLY
    AppendMenuW(video,MF_STRING,ID_VIDEO_D3D11,L"Smooth scaling (linear)"); AppendMenuW(video,MF_STRING,ID_VIDEO_GDI,L"Fast scaling (nearest)");
#endif
#ifndef TB_WIN32_WAVEOUT_ONLY
    AppendMenuW(audio,MF_STRING,ID_AUDIO_WASAPI_SHARED,L"WASAPI shared"); AppendMenuW(audio,MF_STRING,ID_AUDIO_WASAPI_EXCLUSIVE,L"WASAPI exclusive"); AppendMenuW(audio,MF_STRING,ID_AUDIO_WAVEOUT,L"WaveOut");
#endif
#ifndef TB_WIN32_GDI_ONLY
    AppendMenuW(hw,MF_STRING,ID_HW_SW,L"Software decode"); AppendMenuW(hw,MF_STRING,ID_HW_AUTO,L"Auto"); AppendMenuW(hw,MF_STRING,ID_HW_D3D11VA,L"D3D11VA"); AppendMenuW(hw,MF_STRING,ID_HW_NVDEC,L"NVDEC");
#endif
    AppendMenuW(help,MF_STRING,ID_HELP_ABOUT,L"About");
    AppendMenuW(m,MF_POPUP,(UINT_PTR)file,L"File"); AppendMenuW(m,MF_POPUP,(UINT_PTR)state,L"Save States"); AppendMenuW(m,MF_POPUP,(UINT_PTR)view,L"View");
#ifndef TB_WIN32_GDI_ONLY
    AppendMenuW(m,MF_POPUP,(UINT_PTR)video,L"Video Scaling");
#endif
#ifndef TB_WIN32_WAVEOUT_ONLY
    AppendMenuW(m,MF_POPUP,(UINT_PTR)audio,L"Audio Output");
#endif
#ifndef TB_WIN32_GDI_ONLY
    AppendMenuW(m,MF_POPUP,(UINT_PTR)hw,L"FFmpeg HW Decode");
#endif
    AppendMenuW(m,MF_POPUP,(UINT_PTR)help,L"Help");
    a->menu=m; SetMenu(a->hwnd,m);
}
static void update_menu_checks(App *a) {
#ifndef TB_WIN32_GDI_ONLY
    CheckMenuItem(a->menu,ID_VIDEO_D3D11,MF_BYCOMMAND|((tb_win32_video_get_backend(a->video)==TB_WIN32_VIDEO_D3D11)?MF_CHECKED:MF_UNCHECKED));
    CheckMenuItem(a->menu,ID_VIDEO_GDI,MF_BYCOMMAND|((tb_win32_video_get_backend(a->video)==TB_WIN32_VIDEO_GDI)?MF_CHECKED:MF_UNCHECKED));
#endif
#ifndef TB_WIN32_WAVEOUT_ONLY
    int ab=tb_win32_audio_get_backend(); CheckMenuItem(a->menu,ID_AUDIO_WASAPI_SHARED,MF_BYCOMMAND|((ab==TB_WIN32_AUDIO_WASAPI_SHARED)?MF_CHECKED:MF_UNCHECKED)); CheckMenuItem(a->menu,ID_AUDIO_WASAPI_EXCLUSIVE,MF_BYCOMMAND|((ab==TB_WIN32_AUDIO_WASAPI_EXCLUSIVE)?MF_CHECKED:MF_UNCHECKED)); CheckMenuItem(a->menu,ID_AUDIO_WAVEOUT,MF_BYCOMMAND|((ab==TB_WIN32_AUDIO_WAVEOUT)?MF_CHECKED:MF_UNCHECKED));
#endif
#ifndef TB_WIN32_GDI_ONLY
    CheckMenuItem(a->menu,ID_HW_SW,MF_BYCOMMAND|((a->hwaccel==TB_HW_NONE)?MF_CHECKED:MF_UNCHECKED)); CheckMenuItem(a->menu,ID_HW_AUTO,MF_BYCOMMAND|((a->hwaccel==TB_HW_AUTO)?MF_CHECKED:MF_UNCHECKED)); CheckMenuItem(a->menu,ID_HW_D3D11VA,MF_BYCOMMAND|((a->hwaccel==TB_HW_D3D11VA)?MF_CHECKED:MF_UNCHECKED)); CheckMenuItem(a->menu,ID_HW_NVDEC,MF_BYCOMMAND|((a->hwaccel==TB_HW_NVDEC)?MF_CHECKED:MF_UNCHECKED));
#endif
}

static void choose_file(App *a) {
    WCHAR path[MAX_PATH*4]=L""; OPENFILENAMEW ofn; memset(&ofn,0,sizeof(ofn)); ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=a->hwnd; ofn.lpstrFile=path; ofn.nMaxFile=MAX_PATH*4; ofn.lpstrFilter=L"Media\0*.mp4;*.m4v;*.mkv;*.webm;*.mov;*.avi;*.mpg;*.mpeg;*.ogv;*.mp3;*.m4a;*.wav;*.flac\0All files\0*.*\0"; ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) { char *u8 = wtou8(path); if (u8) { app_load_file(a,u8); free(u8); } }
}
static void choose_gallery(App *a) {
    BROWSEINFOW bi; memset(&bi,0,sizeof(bi)); bi.hwndOwner=a->hwnd; bi.lpszTitle=L"Choose a folder of Terebikko videos"; bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_USENEWUI;
    PIDLIST_ABSOLUTE pid = SHBrowseForFolderW(&bi); if (!pid) return; WCHAR path[MAX_PATH*4]; if (SHGetPathFromIDListW(pid,path)) { char *u8=wtou8(path); if (u8) { gallery_scan(a,u8); reg_save_gallery(u8); free(u8); } a->show_gallery=true; app_layout(a); } CoTaskMemFree(pid);
}
static void toggle_fullscreen(App *a) {
    a->fullscreen = !a->fullscreen;
    if (a->fullscreen) {
        a->windowed_style = GetWindowLongW(a->hwnd,GWL_STYLE); GetWindowRect(a->hwnd,&a->windowed_rect);
        SetMenu(a->hwnd,NULL); SetWindowLongW(a->hwnd,GWL_STYLE,a->windowed_style & ~(WS_OVERLAPPEDWINDOW));
        MONITORINFO mi={sizeof(mi)}; GetMonitorInfoW(MonitorFromWindow(a->hwnd,MONITOR_DEFAULTTONEAREST),&mi); SetWindowPos(a->hwnd,HWND_TOP,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(a->hwnd,GWL_STYLE,a->windowed_style); SetMenu(a->hwnd,a->menu); SetWindowPos(a->hwnd,NULL,a->windowed_rect.left,a->windowed_rect.top,a->windowed_rect.right-a->windowed_rect.left,a->windowed_rect.bottom-a->windowed_rect.top,SWP_NOZORDER|SWP_FRAMECHANGED);
    }
    app_layout(a);
    InvalidateRect(a->hwnd, NULL, TRUE);   /* repaint vacated control areas (no ghosting) */
}

static bool app_handle_keydown(App *a, WPARAM wp, LPARAM lp) {
    (void)lp;
    if (!a || !a->hwnd) return false;

    /* Keep global/window shortcuts live even when focus is on an owner-drawn
     * child button in the fullscreen phone lane.  Gameplay bindings are only
     * consumed in player mode so the gallery/listview keeps its normal keys. */
    if (wp == VK_ESCAPE && a->fullscreen) { toggle_fullscreen(a); return true; }
    if (wp == VK_F11) { toggle_fullscreen(a); return true; }
    if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == 'S') { SendMessageW(a->hwnd, WM_COMMAND, ID_STATE_SAVE, 0); return true; }
    if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == 'L') { SendMessageW(a->hwnd, WM_COMMAND, ID_STATE_LOAD, 0); return true; }
    if (a->show_gallery) return false;

#if defined(TB_WIN32_USE_SDL_INPUT)
    { tb_action ac = tb_input_action_for_key(a->input, (int)wp); if (ac != TB_ACT_NONE) { input_action(ac, a); return true; } }
#else
    { tb_action ac = keyboard_action_for_vk(a, wp); if (ac != TB_ACT_NONE) { input_action(ac, a); return true; } }
#endif
    return false;
}

/* ---- owner-draw: flat rounded buttons matching the Qt phone/transport look ---- */
static void draw_button(App *a, LPDRAWITEMSTRUCT d) {
    int id = (int)d->CtlID;
    bool disabled = (d->itemState & ODS_DISABLED) != 0;
    bool pressed = (d->itemState & ODS_SELECTED) != 0;
    COLORREF bg, fg; bool border = false;
    if (id == ID_BTN_PLAY)              { bg = COL_BLUE;   fg = RGB(255,255,255); }
    else if (id == ID_BTN_ANS1)         { bg = COL_ANS1;   fg = RGB(255,255,255); }
    else if (id == ID_BTN_ANS2)         { bg = COL_ANS2;   fg = RGB(255,255,255); }
    else if (id == ID_BTN_ANS3)         { bg = COL_ANS3;   fg = RGB(255,255,255); }
    else if (id == ID_BTN_ANS4)         { bg = COL_ANS4;   fg = COL_ANS4_FG; }
    else if (id == ID_BTN_PICKUP)       { bg = COL_PICKUP; fg = COL_PICKUP_FG; }
    else                                { bg = COL_PANEL;  fg = COL_TEXT; border = true; }
    if (disabled) { bg = COL_DIS_BG; fg = COL_DIS_FG; border = false; }
    else if (pressed) { bg = RGB(GetRValue(bg)*4/5, GetGValue(bg)*4/5, GetBValue(bg)*4/5); }

    RECT r = d->rcItem;
    FillRect(d->hDC, &r, (a->fullscreen && a->fullscreen_controls_panel) ? a->br_black : a->br_bg);   /* clear rounded corners to lane/window bg */
    HBRUSH brush = CreateSolidBrush(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, border ? COL_BORDER : bg);
    HGDIOBJ ob = SelectObject(d->hDC, brush), op = SelectObject(d->hDC, pen);
    int bh = r.bottom - r.top;
    int rad = (id>=ID_BTN_ANS1 && id<=ID_BTN_PICKUP) ? TB_CLAMP(bh / 8, 18, 44) : 12;
    RoundRect(d->hDC, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(d->hDC, ob); SelectObject(d->hDC, op); DeleteObject(brush); DeleteObject(pen);

    WCHAR txt[256]; GetWindowTextW(d->hwndItem, txt, 256);
    HFONT dyn_font = NULL;
    HFONT base_font = (HFONT)SendMessageW(d->hwndItem, WM_GETFONT, 0, 0);
    if (id >= ID_BTN_ANS1 && id <= ID_BTN_PICKUP) {
        int px = (id == ID_BTN_PICKUP) ? TB_CLAMP(bh / 4, 28, 86) : TB_CLAMP(bh / 3, 30, 96);
        dyn_font = CreateFontW(-px,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        if (dyn_font) base_font = dyn_font;
    }
    HGDIOBJ of = SelectObject(d->hDC, base_font);
    SetBkMode(d->hDC, TRANSPARENT); SetTextColor(d->hDC, fg);
    DrawTextW(d->hDC, txt, -1, &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(d->hDC, of);
    if (dyn_font) DeleteObject(dyn_font);
}

/* ---------------- controls / input settings dialog ---------------- */
static const char *act_names[TB_ACT_COUNT] = { "Answer 1","Answer 2","Answer 3","Answer 4","Pick Up","Play / Pause","Fullscreen" };
static const int   act_default_keys[TB_ACT_COUNT] = { '1','2','3','4', VK_RETURN, 'P', VK_F11 };

static int binding_key(App *a, int act) {
#if defined(TB_WIN32_USE_SDL_INPUT)
    return tb_input_get(a->input, (tb_action)act).key;
#else
    return a->keymap[act];
#endif
}
static void binding_set_key(App *a, int act, int vk) {
#if defined(TB_WIN32_USE_SDL_INPUT)
    tb_input_set_key(a->input, (tb_action)act, vk);
#else
    a->keymap[act] = vk;
#endif
}
static void vk_name(int vk, char *out, size_t n) {
    if (vk <= 0) { safe_copy(out, n, "(unset)"); return; }
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    switch (vk) { case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
                  case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE: lp |= (1L << 24); break; default: break; }
    WCHAR wb[64] = {0};
    if (GetKeyNameTextW(lp, wb, 64) > 0) { char *u = wtou8(wb); if (u) { safe_copy(out, n, u); free(u); return; } }
    snprintf(out, n, "VK %d", vk);
}
static void input_save(App *a) {
#if defined(TB_WIN32_USE_SDL_INPUT)
    char p[MAX_PATH*4]; snprintf(p, sizeof p, "%s\\input.cfg", a->exe_dir); tb_input_save(a->input, p);
#else
    HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\TerebikkoEmu", 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        for (int i = 0; i < TB_ACT_COUNT; i++) { WCHAR nm[32]; wsprintfW(nm, L"Key%d", i); DWORD v = (DWORD)a->keymap[i]; RegSetValueExW(k, nm, 0, REG_DWORD, (const BYTE*)&v, sizeof v); }
        RegCloseKey(k);
    }
#endif
}
static void input_load(App *a) {
#if defined(TB_WIN32_USE_SDL_INPUT)
    char p[MAX_PATH*4]; snprintf(p, sizeof p, "%s\\input.cfg", a->exe_dir); tb_input_load(a->input, p);
#else
    HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\TerebikkoEmu", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        for (int i = 0; i < TB_ACT_COUNT; i++) { WCHAR nm[32]; wsprintfW(nm, L"Key%d", i); DWORD v = 0, sz = sizeof v, t = 0; if (RegQueryValueExW(k, nm, NULL, &t, (BYTE*)&v, &sz) == ERROR_SUCCESS && t == REG_DWORD && v) a->keymap[i] = (int)v; }
        RegCloseKey(k);
    }
#endif
}

#define SID_KEY 3000
#define SID_PAD 3100
#define SID_CLOSE 3200
#define SID_RESET 3201
#define SID_FULLSCREEN_PANEL 3202
typedef struct { App *a; HWND wnd, bind_static[TB_ACT_COUNT], status, fullscreen_panel_check; int capture, capture_pad; bool done; } SettingsDlg;
static SettingsDlg g_set;
#if defined(TB_WIN32_USE_SDL_INPUT)
static void settings_noop_cb(tb_action x, void *u) { (void)x; (void)u; }
#endif

static void settings_refresh_row(int i) {
    char kb[80], b[200]; vk_name(binding_key(g_set.a, i), kb, sizeof kb);
#if defined(TB_WIN32_USE_SDL_INPUT)
    int pad = tb_input_get(g_set.a->input, (tb_action)i).pad; const char *pn = tb_input_pad_button_name(pad);
    snprintf(b, sizeof b, "%s    |    Pad: %s", kb, (pad >= 0 && pn) ? pn : "-");
#else
    snprintf(b, sizeof b, "%s", kb);
#endif
    set_text_u8(g_set.bind_static[i], b);
}

static LRESULT CALLBACK settings_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    App *a = g_set.a;
    switch (m) {
        case WM_CTLCOLORSTATIC: { HDC dc=(HDC)w; SetBkMode(dc,TRANSPARENT); SetTextColor(dc,COL_TEXT); SetBkColor(dc,COL_BG); return (LRESULT)a->br_bg; }
        case WM_KEYDOWN:
            if (g_set.capture >= 0) {
                if (w == VK_ESCAPE) { g_set.capture = -1; set_text_u8(g_set.status, "Cancelled."); return 0; }
                binding_set_key(a, g_set.capture, (int)w); settings_refresh_row(g_set.capture);
                set_text_u8(g_set.status, "Key bound."); g_set.capture = -1; return 0;
            }
            break;
        case WM_TIMER:
#if defined(TB_WIN32_USE_SDL_INPUT)
            if (g_set.capture_pad >= 0 && a->input) {
                tb_input_poll(a->input, settings_noop_cb, a);
                int b = tb_input_take_captured(a->input);
                if (b >= 0) { tb_input_set_pad(a->input, (tb_action)g_set.capture_pad, b); settings_refresh_row(g_set.capture_pad); g_set.capture_pad = -1; set_text_u8(g_set.status, "Gamepad button bound."); }
            }
#endif
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id >= SID_KEY && id < SID_KEY + TB_ACT_COUNT) { g_set.capture = id - SID_KEY; g_set.capture_pad = -1; set_text_u8(g_set.status, "Press a key...  (Esc cancels)"); SetFocus(h); return 0; }
#if defined(TB_WIN32_USE_SDL_INPUT)
            if (id >= SID_PAD && id < SID_PAD + TB_ACT_COUNT) { if (a->input) { g_set.capture_pad = id - SID_PAD; g_set.capture = -1; tb_input_begin_capture(a->input); set_text_u8(g_set.status, "Press a gamepad button..."); } return 0; }
#endif
            if (id == SID_RESET) { for (int i = 0; i < TB_ACT_COUNT; i++) { binding_set_key(a, i, act_default_keys[i]); settings_refresh_row(i); } set_text_u8(g_set.status, "Reset to defaults."); return 0; }
            if (id == SID_FULLSCREEN_PANEL && HIWORD(w) == BN_CLICKED) {
                a->fullscreen_controls_panel = (SendMessageW(g_set.fullscreen_panel_check, BM_GETCHECK, 0, 0) == BST_CHECKED);
                app_save_prefs(a);
                app_layout(a);
                InvalidateRect(a->hwnd, NULL, TRUE);
                set_text_u8(g_set.status, a->fullscreen_controls_panel ? "Fullscreen controls panel enabled." : "Fullscreen controls panel disabled.");
                return 0;
            }
            if (id == SID_CLOSE || id == IDCANCEL) { g_set.done = true; return 0; }
            return 0;
        }
        case WM_CLOSE: g_set.done = true; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void open_settings(App *a) {
    static bool reg = false;
    if (!reg) { WNDCLASSW wc; memset(&wc, 0, sizeof wc); wc.lpfnWndProc = settings_proc; wc.hInstance = a->inst; wc.lpszClassName = L"TBSettings"; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = a->br_bg; RegisterClassW(&wc); reg = true; }
    memset(&g_set, 0, sizeof g_set); g_set.a = a; g_set.capture = -1; g_set.capture_pad = -1;
    int rowH = 34, top = 14, W = 470, H = top + TB_ACT_COUNT * rowH + 126;
    RECT pr; GetWindowRect(a->hwnd, &pr);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"TBSettings", L"Controls / Input", WS_POPUP|WS_CAPTION|WS_SYSMENU, pr.left + 70, pr.top + 70, W, H, a->hwnd, NULL, a->inst, NULL);
    g_set.wnd = dlg;
    for (int i = 0; i < TB_ACT_COUNT; i++) {
        int ry = top + i * rowH;
        HWND lab = CreateWindowW(L"STATIC", NULL, WS_CHILD|WS_VISIBLE, 12, ry + 4, 104, 22, dlg, NULL, a->inst, NULL); SendMessageW(lab, WM_SETFONT, (WPARAM)a->font_ui, TRUE); set_text_u8(lab, act_names[i]);
        g_set.bind_static[i] = CreateWindowW(L"STATIC", NULL, WS_CHILD|WS_VISIBLE, 122, ry + 4, 212, 22, dlg, NULL, a->inst, NULL); SendMessageW(g_set.bind_static[i], WM_SETFONT, (WPARAM)a->font_ui, TRUE);
        HWND bk = CreateWindowW(L"BUTTON", L"Set Key", WS_CHILD|WS_VISIBLE|WS_TABSTOP, 338, ry, 60, 26, dlg, (HMENU)(INT_PTR)(SID_KEY + i), a->inst, NULL); SendMessageW(bk, WM_SETFONT, (WPARAM)a->font_ui, TRUE);
#if defined(TB_WIN32_USE_SDL_INPUT)
        HWND bp = CreateWindowW(L"BUTTON", L"Pad", WS_CHILD|WS_VISIBLE|WS_TABSTOP, 402, ry, 52, 26, dlg, (HMENU)(INT_PTR)(SID_PAD + i), a->inst, NULL); SendMessageW(bp, WM_SETFONT, (WPARAM)a->font_ui, TRUE);
#endif
        settings_refresh_row(i);
    }
    int by = top + TB_ACT_COUNT * rowH + 8;
    g_set.fullscreen_panel_check = CreateWindowW(L"BUTTON", L"Show right-side controls panel in fullscreen", WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX, 12, by, W - 24, 24, dlg, (HMENU)(INT_PTR)SID_FULLSCREEN_PANEL, a->inst, NULL);
    SendMessageW(g_set.fullscreen_panel_check, WM_SETFONT, (WPARAM)a->font_ui, TRUE);
    SendMessageW(g_set.fullscreen_panel_check, BM_SETCHECK, a->fullscreen_controls_panel ? BST_CHECKED : BST_UNCHECKED, 0);
    by += 30;
    g_set.status = CreateWindowW(L"STATIC", L"Click \"Set Key\", then press a key. Gamepad: \"Pad\".", WS_CHILD|WS_VISIBLE, 12, by, W - 24, 20, dlg, NULL, a->inst, NULL); SendMessageW(g_set.status, WM_SETFONT, (WPARAM)a->font_ui, TRUE);
    CreateWindowW(L"BUTTON", L"Reset", WS_CHILD|WS_VISIBLE|WS_TABSTOP, W - 184, by + 26, 72, 28, dlg, (HMENU)(INT_PTR)SID_RESET, a->inst, NULL);
    CreateWindowW(L"BUTTON", L"Close", WS_CHILD|WS_VISIBLE|WS_TABSTOP, W - 100, by + 26, 72, 28, dlg, (HMENU)(INT_PTR)SID_CLOSE, a->inst, NULL);
    SetTimer(dlg, 9, 60, NULL);
    EnableWindow(a->hwnd, FALSE); ShowWindow(dlg, SW_SHOW); SetFocus(dlg);
    MSG msg;
    while (!g_set.done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* During key capture, bypass IsDialogMessage so WM_KEYDOWN reaches us. */
        if (g_set.capture >= 0 || !IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    KillTimer(dlg, 9); EnableWindow(a->hwnd, TRUE); DestroyWindow(dlg); g_set.wnd = NULL;
    input_save(a); SetForegroundWindow(a->hwnd);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App *a = &g_app;
    switch (msg) {
        case WM_CREATE:
            a->hwnd=hwnd; a->side_width=330; a->show_gallery=true; a->mode=TB_GM_EASY; a->hwaccel=TB_HW_NONE; app_load_prefs(a); QueryPerformanceFrequency(&a->qpc_freq); QueryPerformanceCounter(&a->qpc_start); srand((unsigned)GetTickCount());
            for(int i=0;i<4;i++) a->e_ans[i]=-1; a->e_pickup=a->e_play=a->e_save=a->e_load=-1;
            { WCHAR exew[MAX_PATH*4]; GetModuleFileNameW(NULL,exew,MAX_PATH*4); char *u8=wtou8(exew); safe_copy(a->exe_dir,sizeof(a->exe_dir),u8?u8:"."); free(u8); char *s=strrchr(a->exe_dir,'\\'); if(s)*s=0; }
            a->br_bg = CreateSolidBrush(COL_BG); a->br_panel = CreateSolidBrush(COL_PANEL); a->br_badge = CreateSolidBrush(COL_BADGE_BG); a->br_black = CreateSolidBrush(COL_BLACK);
            a->font_ui     = CreateFontW(-15,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            a->font_small  = CreateFontW(-11,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            a->font_value  = CreateFontW(-22,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            a->font_btn    = CreateFontW(-15,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            a->font_bigbtn = CreateFontW(-30,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
#if defined(TB_WIN32_USE_SDL_INPUT)
            SDL_Init(SDL_INIT_GAMEPAD); a->input=tb_input_create(); { int keys[TB_ACT_COUNT]={'1','2','3','4',VK_RETURN,'P',VK_F11}; tb_input_reset_defaults(a->input,keys); } input_load(a);
#elif defined(TB_WIN32_USE_WINMM_INPUT)
            winmm_input_init(a); for(int i=0;i<TB_ACT_COUNT;i++) a->keymap[i]=act_default_keys[i]; input_load(a);
#endif
            a->sfx=tb_sfx_create(); a->video=tb_win32_video_create(hwnd); build_menu(a); create_controls(a); SetTimer(hwnd,TIMER_FRAME,16,NULL); app_layout(a); update_menu_checks(a);
            { char saved[MAX_PATH*4]; if (reg_load_gallery(saved, sizeof(saved)) && dir_exists(saved)) { gallery_scan(a, saved); app_set_status("Gallery"); } }
            return 0;
        case WM_SIZE: app_layout(a); if (a->video) tb_win32_video_resize(a->video, LOWORD(lp), HIWORD(lp)); InvalidateRect(hwnd, NULL, TRUE); return 0;
        case WM_TIMER: if (wp==TIMER_FRAME) { app_tick(a); return 0; } break;
        case WM_ERASEBKGND: return 1;   /* WM_PAINT paints everything; skip the flicker-inducing erase */
        case WM_CTLCOLORSTATIC: {
            HDC dc=(HDC)wp; HWND ch=(HWND)lp; SetBkMode(dc,TRANSPARENT);
            if (ch==a->cap_score||ch==a->cap_streak||ch==a->cap_lives||ch==a->cap_correct||ch==a->status_label||ch==a->gallery_header) { SetTextColor(dc,COL_MUTE); SetBkColor(dc,COL_BG); return (LRESULT)a->br_bg; }
            if (ch==a->mode_badge) { SetTextColor(dc,COL_BADGE_FG); SetBkColor(dc,COL_BADGE_BG); return (LRESULT)a->br_badge; }
            SetTextColor(dc,COL_TEXT); SetBkColor(dc,COL_BG); return (LRESULT)a->br_bg;
        }
        case WM_DRAWITEM: { LPDRAWITEMSTRUCT d=(LPDRAWITEMSTRUCT)lp; if (d->CtlType==ODT_BUTTON) { draw_button(a,d); return TRUE; } break; }
        case WM_PAINT: {
            PAINTSTRUCT ps; BeginPaint(hwnd,&ps);
            RECT c; GetClientRect(hwnd,&c);
            if (a->show_gallery) { FillRect(ps.hdc, &c, a->br_bg); }
            else {
                /* dark-fill only the regions outside the video (side panel + toolbar
                 * gaps); the video region is covered by a single back-buffer blit, so
                 * nothing flashes. Child controls are excluded via WS_CLIPCHILDREN. */
                RECT side={a->video_rect.right,0,c.right,c.bottom}; FillRect(ps.hdc,&side,(a->fullscreen && a->fullscreen_controls_panel) ? a->br_black : a->br_bg);
                RECT bot={0,a->video_rect.bottom,a->video_rect.right,c.bottom}; FillRect(ps.hdc,&bot,a->br_bg);
                app_render(a, ps.hdc);
            }
            EndPaint(hwnd,&ps); return 0;
        }
        case WM_NOTIFY: {
            LPNMHDR nh=(LPNMHDR)lp;
            if (nh->idFrom==ID_GALLERY_LIST && nh->code==LVN_ITEMACTIVATE) {
                LPNMITEMACTIVATE ia=(LPNMITEMACTIVATE)lp; int item=ia->iItem;
                if (item<0) item=ListView_GetNextItem(a->gallery_list,-1,LVNI_SELECTED|LVNI_FOCUSED);
                if (item>=0) { LVITEMW lv; memset(&lv,0,sizeof(lv)); lv.mask=LVIF_PARAM; lv.iItem=item; if (SendMessageW(a->gallery_list,LVM_GETITEMW,0,(LPARAM)&lv)) { int idx=(int)lv.lParam; if (idx>=0 && idx<a->gallery_count) app_load_file(a,a->gallery_items[idx].path); } }
                return 0;
            }
            break;
        }
        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            {
                POINT p={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; const tb_ui *u=a->game?tb_game_ui(a->game):NULL;
                if (u && u->difficulty_overlay) { static const tb_gamemode dm[4]={TB_GM_EASY,TB_GM_HARD,TB_GM_VERYHARD,TB_GM_MANIA}; for(int i=0;i<4;i++) if(PtInRect(&a->diff_rect[i],p)){ a->mode=dm[i]; tb_game_set_mode(a->game,dm[i]); return 0; } }
            }
            break;
        case WM_KEYDOWN:
            if (app_handle_keydown(a, wp, lp)) return 0;
            break;
        case WM_COMMAND: {
            int id=LOWORD(wp);
            if (id==ID_FILE_LOAD) choose_file(a);
            else if (id==ID_FILE_GALLERY) choose_gallery(a);
            else if (id==ID_FILE_EXIT) DestroyWindow(hwnd);
            else if (id==ID_VIEW_FULLSCREEN || id==ID_BTN_FULLSCREEN) toggle_fullscreen(a);
            else if (id==ID_VIEW_SETTINGS) open_settings(a);
            else if (id==ID_VIEW_GALLERY || id==ID_BTN_GALLERY) { app_free_media(a); a->show_gallery=true; app_layout(a); app_set_status("Gallery"); SetWindowTextW(hwnd,L"TerebikkoEmu"); InvalidateRect(hwnd,NULL,TRUE); }
            else if (id==ID_BTN_PLAY) app_toggle_pause(a);
            else if (id==ID_BTN_RESTART) app_choose_difficulty(a,"restart");
            else if (id>=ID_BTN_ANS1 && id<=ID_BTN_ANS4) { if(a->game) tb_game_on_answer(a->game,id-ID_BTN_ANS1+1); }
            else if (id==ID_BTN_PICKUP) { if(a->game) tb_game_on_pickup(a->game); }
            else if (id==ID_STATE_SAVE || id==ID_BTN_SAVE) { app_set_status(app_save_state(a)?"Save state stored.":"Save states are available in Easy/Hard after decoding."); }
            else if (id==ID_STATE_LOAD || id==ID_BTN_LOAD) { app_set_status(app_load_state(a)?"Save state loaded.":"No matching save state for this file in Easy/Hard."); }
#ifndef TB_WIN32_GDI_ONLY
            else if (id==ID_VIDEO_D3D11) { tb_win32_video_set_backend(a->video,TB_WIN32_VIDEO_D3D11); update_menu_checks(a); }
            else if (id==ID_VIDEO_GDI) { tb_win32_video_set_backend(a->video,TB_WIN32_VIDEO_GDI); update_menu_checks(a); }
#endif
#ifndef TB_WIN32_WAVEOUT_ONLY
            else if (id==ID_AUDIO_WAVEOUT || id==ID_AUDIO_WASAPI_SHARED || id==ID_AUDIO_WASAPI_EXCLUSIVE) { int b=id==ID_AUDIO_WAVEOUT?TB_WIN32_AUDIO_WAVEOUT:(id==ID_AUDIO_WASAPI_EXCLUSIVE?TB_WIN32_AUDIO_WASAPI_EXCLUSIVE:TB_WIN32_AUDIO_WASAPI_SHARED); tb_win32_audio_set_backend(b); tb_win32_audio_shutdown(); tb_win32_audio_init(); update_menu_checks(a); }
#endif
#ifndef TB_WIN32_GDI_ONLY
            else if (id==ID_HW_SW || id==ID_HW_AUTO || id==ID_HW_D3D11VA || id==ID_HW_NVDEC) { a->hwaccel = id==ID_HW_SW?TB_HW_NONE:(id==ID_HW_AUTO?TB_HW_AUTO:(id==ID_HW_D3D11VA?TB_HW_D3D11VA:TB_HW_NVDEC)); update_menu_checks(a); if(a->ready && a->file_path[0]) { char reload_path[MAX_PATH * 4]; safe_copy(reload_path, sizeof(reload_path), a->file_path); app_load_file(a,reload_path); } }
#endif
            else if (id==ID_HELP_ABOUT) MessageBoxW(hwnd,
#if defined(TB_WIN32_WAVEOUT_ONLY)
                L"TerebikkoEmu Win32 backend\nGDI video, software FFmpeg decode, keyboard + WinMM joystick/gamepad input, WaveOut audio.",
#elif defined(TB_WIN32_GDI_ONLY)
                L"TerebikkoEmu Win32 backend\nGDI video, software FFmpeg decode, keyboard + WinMM joystick/gamepad input, WASAPI/WaveOut audio.",
#elif defined(TB_WIN32_USE_SDL_INPUT)
                L"TerebikkoEmu Win64 backend\nD3D11/GDI video, FFmpeg decode, SDL3 gamepad input, WASAPI/WaveOut audio.",
#else
                L"TerebikkoEmu Win32 backend\nD3D11/GDI video, FFmpeg decode, keyboard + WinMM joystick/gamepad input, WASAPI/WaveOut audio.",
#endif
                L"About",MB_OK);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd,TIMER_FRAME); app_free_media(a); if(a->video) tb_win32_video_destroy(a->video); if(a->sfx) tb_sfx_destroy(a->sfx);
            if (a->gallery_images) { ImageList_Destroy(a->gallery_images); a->gallery_images=NULL; }
            if (a->font_ui) DeleteObject(a->font_ui); if (a->font_small) DeleteObject(a->font_small); if (a->font_value) DeleteObject(a->font_value); if (a->font_btn) DeleteObject(a->font_btn); if (a->font_bigbtn) DeleteObject(a->font_bigbtn);
            if (a->br_bg) DeleteObject(a->br_bg); if (a->br_panel) DeleteObject(a->br_panel); if (a->br_badge) DeleteObject(a->br_badge); if (a->br_black) DeleteObject(a->br_black);
#if defined(TB_WIN32_USE_SDL_INPUT)
            if(a->input) tb_input_destroy(a->input); SDL_Quit();
#endif
            free(a->gallery_items); a->gallery_items=NULL; a->gallery_count=0;
            tb_win32_audio_shutdown(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev; (void)cmd; memset(&g_app,0,sizeof(g_app)); g_app.inst=hInst;
    OleInitialize(NULL);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);
    WNDCLASSW wc; memset(&wc,0,sizeof(wc)); wc.lpfnWndProc=wndproc; wc.hInstance=hInst; wc.lpszClassName=APP_CLASS; wc.hCursor=LoadCursor(NULL,IDC_ARROW); wc.hbrBackground=CreateSolidBrush(COL_BG);
    RegisterClassW(&wc);
    HWND hwnd=CreateWindowW(APP_CLASS,L"TerebikkoEmu",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1180,760,NULL,NULL,hInst,NULL);
    ShowWindow(hwnd,show); UpdateWindow(hwnd);

    /* A path passed on the command line (UTF-16 -> UTF-8): a folder opens as a
     * gallery (so dropping a video folder on the exe browses it), a file loads
     * directly. */
    int argc=0; LPWSTR *argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (argv) {
        if (argc>=2) {
            DWORD at=GetFileAttributesW(argv[1]);
            char *u8=wtou8(argv[1]);
            if (u8 && u8[0]) {
                if (at!=INVALID_FILE_ATTRIBUTES && (at&FILE_ATTRIBUTE_DIRECTORY)) { gallery_scan(&g_app,u8); reg_save_gallery(u8); g_app.show_gallery=true; app_layout(&g_app); }
                else app_load_file(&g_app,u8);
            }
            free(u8);
        }
        LocalFree(argv);
    }

    MSG msg; while(GetMessageW(&msg,NULL,0,0)>0){
        if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) && g_app.hwnd &&
            (msg.hwnd == g_app.hwnd || IsChild(g_app.hwnd, msg.hwnd)) &&
            app_handle_keydown(&g_app, msg.wParam, msg.lParam)) {
            continue;
        }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    OleUninitialize();
    return (int)msg.wParam;
}

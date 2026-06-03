/*
 * tb_game.h - Terebikko game state machine (pure C11, opaque handle).
 *
 * Faithful port of the JS game logic: Easy / Hard / Hard+ / Minigame modes,
 * scoring, lives, Dragon's-Lair checkpoint retries, the endless Mania remix and
 * save states. The machine never touches Qt/SDL/ffmpeg; it drives playback through
 * a host callback vtable (tb_host) and exposes UI state through tb_ui.
 */
#ifndef TB_GAME_H
#define TB_GAME_H

#include "tb_events.h"
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { TB_GM_EASY = 0, TB_GM_HARD, TB_GM_VERYHARD, TB_GM_MANIA } tb_gamemode;

typedef enum {
    TB_SFX_NONE = 0, TB_SFX_CORRECT, TB_SFX_WRONG, TB_SFX_GAMEOVER, TB_SFX_WIN,
    TB_SFX_MANIA_ANSWER, TB_SFX_MANIA_PICKUP, TB_SFX_MANIA_STEP
} tb_sfx;

typedef enum { TB_STATUS_GOOD = 0, TB_STATUS_BAD, TB_STATUS_WARN } tb_status_kind;

/* Host callbacks: the frontend implements playback + clock + RNG + sound. */
typedef struct {
    void   (*seek)(void *u, double seconds);
    void   (*play)(void *u);
    void   (*pause)(void *u);
    void   (*set_rate)(void *u, double rate);
    void   (*set_muted)(void *u, bool muted);
    double (*get_time)(void *u);       /* current playback position, seconds */
    double (*get_duration)(void *u);
    bool   (*is_paused)(void *u);
    void   (*play_sfx)(void *u, tb_sfx s);
    double (*now_ms)(void *u);         /* monotonic wall clock, ms */
    double (*rand01)(void *u);         /* uniform [0,1) */
    void   (*mania_music)(void *u, bool on);
} tb_host;

/* UI state the frontend renders every frame (read-only snapshot). */
typedef struct {
    bool buttons_enabled;
    bool pickup_enabled, pickup_live;

    bool call_overlay; char call_title[40]; char call_hint[64];

    bool status_overlay; char status_text[40]; tb_status_kind status_kind;

    bool game_over; char game_over_text[40]; char game_over_sub[80];

    bool results; char result_title[48]; long result_score; char result_line[200];
    bool result_retry, result_difficulty, result_close;

    bool difficulty_overlay;

    /* Mania screen transitions: seq increments on each new prompt; dir 0=top 1=left
     * 2=right 3=bottom (the new screen slides in from that edge). */
    int  transition_seq;
    int  transition_dir;

    /* HUD */
    long score, streak, best;
    int  lives;          /* INT_MAX => infinite */
    int  correct, n_questions;
    int  mania_round; long mania_high;
    tb_gamemode mode;
} tb_ui;

typedef struct tb_game tb_game;

/* events_owned is borrowed; must outlive the game. carrier kept for save metadata. */
tb_game *tb_game_create(const tb_event_list *events, const tb_host *host, void *host_user);
void     tb_game_destroy(tb_game *g);

void tb_game_set_mode(tb_game *g, tb_gamemode mode);
tb_gamemode tb_game_mode(const tb_game *g);

void tb_game_reset(tb_game *g);
void tb_game_start(tb_game *g, bool restart);   /* picks mania path automatically */
void tb_game_restart(tb_game *g);

void tb_game_on_answer(tb_game *g, int n);      /* n = 1..4 */
void tb_game_on_pickup(tb_game *g);

/* Called every animation frame by the frontend. Advances the machine. */
void tb_game_tick(tb_game *g);

/* Called when playback reaches end-of-media. */
void tb_game_on_media_ended(tb_game *g);

/* Difficulty selection helpers (mirror askDifficulty / setMode pending action). */
void tb_game_open_difficulty(tb_game *g, const char *pending_action /* "start"|"restart"|NULL */);

const tb_ui *tb_game_ui(const tb_game *g);
bool tb_game_pause_locked(const tb_game *g);
bool tb_game_running(const tb_game *g);
bool tb_game_ended(const tb_game *g);

/* ---- save states (Easy/Hard only) ---- */
typedef struct {
    int         version;
    tb_gamemode mode;
    double      time;
    int         cur_idx;
    long        score, streak, best;
    int         lives;            /* INT_MAX => infinite */
    int         correct, wrong;
    double      pause_lock_until;
    /* per-play-event runtime flags */
    int         n_play;
    unsigned char *done;          /* n_play bytes */
    int           *got;           /* n_play ints, -1 = null */
    long long   saved_at_ms;
} tb_savestate;

bool tb_game_mode_allows_save(tb_gamemode mode);
bool tb_game_snapshot(const tb_game *g, tb_savestate *out);  /* allocates out->done/got */
void tb_savestate_free(tb_savestate *s);
bool tb_game_restore(tb_game *g, const tb_savestate *s);

#ifdef __cplusplus
}
#endif
#endif /* TB_GAME_H */

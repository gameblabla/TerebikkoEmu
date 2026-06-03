/* tb_game.c - faithful C11 port of the JS game state machine. */
#include "tb_game.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Deferred actions replace the JS setTimeout transitions. */
typedef enum {
    DEF_NONE = 0,
    DEF_CLEAR_SEEK,        /* clear internal-seek lock                       */
    DEF_HARD_RETRY,        /* phase 1: hide game-over, seek checkpoint, re-arm */
    DEF_HARD_PHASE2,       /* phase 2: clear failing + play                   */
    DEF_HARD_RESULTS,      /* hide game-over -> results (dead / forced)       */
    DEF_GAMEOVER_RESULTS,  /* veryHard / showChoiceAfterGameOver -> results   */
    DEF_MANIA_PLAY,        /* clear seek then play (pickup prompt / break)    */
    DEF_MANIA_PAUSE        /* clear seek then pause (answer prompt freeze)    */
} def_kind;

typedef struct { bool active; def_kind kind; double at_ms; int evt; char sub[80]; bool dead; } deferred;

struct tb_game {
    const tb_event_list *events;
    tb_host host; void *u;

    int *play;            /* event indices that are interactive */
    int  n_play;
    unsigned char *done;  /* per play-event */
    int           *got;   /* per play-event, -1 = null */
    int  n_questions;

    tb_gamemode mode;
    long score, streak, best;
    int  lives;           /* INT_MAX => infinite */
    int  correct, wrong;
    bool ended, running, failing;

    int    cur_idx;       /* index into play[] */
    int    active;        /* index into play[], -1 none */
    double pause_lock_until;
    double seek_lock_until;   /* now_ms < this => internal seek */

    char pending_action[16];    /* "start"/"restart"/"" — copied, never aliased */

    /* mania */
    bool   m_active;
    int    m_round;
    bool   m_has_prompt;
    tb_event_type p_type; int p_answer; int p_need, p_got;
    double p_deadline_ms, p_promptstart; int p_source;
    double m_next_at, m_break_until, m_clip_end;
    bool   m_was_muted;
    long   m_high;

    double status_until_ms;
    deferred defs[4];

    tb_ui ui;
};

/* ---------- small helpers ---------- */
static double clampd(double n, double a, double b) { return n < a ? a : (n > b ? b : n); }
static double round2(double x) { return round(x * 100.0) / 100.0; }
static double now_ms(tb_game *g) { return g->host.now_ms(g->u); }
static double vtime(tb_game *g) { return g->host.get_time(g->u); }

static bool mania_mode(const tb_game *g) { return g->mode == TB_GM_MANIA; }
static bool hard_mode(const tb_game *g) { return g->mode == TB_GM_HARD || g->mode == TB_GM_VERYHARD; }
static bool very_hard(const tb_game *g) { return g->mode == TB_GM_VERYHARD; }

static const tb_event *play_event(const tb_game *g, int play_idx) {
    return &g->events->data[g->play[play_idx]];
}

static void schedule(tb_game *g, def_kind kind, double delay_ms, int evt, const char *sub, bool dead) {
    for (int i = 0; i < 4; i++) if (!g->defs[i].active) {
        g->defs[i].active = true; g->defs[i].kind = kind; g->defs[i].at_ms = now_ms(g) + delay_ms;
        g->defs[i].evt = evt; g->defs[i].dead = dead;
        g->defs[i].sub[0] = '\0'; if (sub) { strncpy(g->defs[i].sub, sub, sizeof g->defs[i].sub - 1); g->defs[i].sub[sizeof g->defs[i].sub - 1] = '\0'; }
        return;
    }
}

static void do_seek(tb_game *g, double t, double lock_ms) {
    if (t < 0) t = 0;
    g->host.seek(g->u, t);
    g->seek_lock_until = now_ms(g) + lock_ms;
}
static bool internal_seek(tb_game *g) { return now_ms(g) < g->seek_lock_until; }

/* ---------- UI helpers ---------- */
static void ui_status(tb_game *g, const char *text, tb_status_kind kind, double ms) {
    g->ui.status_overlay = true;
    strncpy(g->ui.status_text, text, sizeof g->ui.status_text - 1);
    g->ui.status_text[sizeof g->ui.status_text - 1] = '\0';
    g->ui.status_kind = kind;
    g->status_until_ms = ms > 0 ? now_ms(g) + ms : 0;
}
static void ui_clear_active(tb_game *g) {
    g->active = -1;
    g->ui.buttons_enabled = false;
    g->ui.pickup_enabled = false; g->ui.pickup_live = false;
    g->ui.call_overlay = false;
}
static void buttons_enabled(tb_game *g, bool on) { g->ui.buttons_enabled = on; }
static void pickup_enabled(tb_game *g, bool on) { g->ui.pickup_enabled = on; g->ui.pickup_live = on; }

static void update_hud(tb_game *g) {
    g->ui.score = g->score; g->ui.streak = g->streak; g->ui.best = g->best;
    g->ui.lives = g->lives; g->ui.correct = g->correct; g->ui.n_questions = g->n_questions;
    g->ui.mania_round = g->m_round; g->ui.mania_high = g->m_high; g->ui.mode = g->mode;
}

static void configure_results(tb_game *g, const char *title, const char *line,
                              bool retry, bool difficulty, bool force_choice) {
    strncpy(g->ui.result_title, title, sizeof g->ui.result_title - 1); g->ui.result_title[sizeof g->ui.result_title - 1] = '\0';
    g->ui.result_score = g->score;
    strncpy(g->ui.result_line, line, sizeof g->ui.result_line - 1); g->ui.result_line[sizeof g->ui.result_line - 1] = '\0';
    g->ui.result_retry = retry;
    g->ui.result_difficulty = difficulty;
    g->ui.result_close = !force_choice;
}

/* ---------- mode timing ---------- */
static double mode_end(const tb_game *g, const tb_event *e) {
    if (!e) return 0;
    if (!very_hard(g)) return e->end;
    double d = e->end - e->start; if (d < 0.1) d = 0.1;
    double mx = e->type == TB_EV_ANSWER ? 3.0 : 2.4;
    double mn = e->type == TB_EV_ANSWER ? 1.15 : 0.9;
    return round2(e->start + clampd(d * 0.28, mn, mx));
}

bool tb_game_pause_locked(const tb_game *g) {
    /* Mania owns playback internally: it seeks, changes rate, pauses on answer
     * prompts, resumes for pickup/break clips, and freezes permanently at game
     * over until Pick Up/Restart starts a new round.  User Play/Pause must never
     * override that state, including after Mania has ended. */
    if (mania_mode(g)) return true;
    if (g->ended) return false;
    if (now_ms((tb_game *)g) < g->seek_lock_until) return false;
    if (hard_mode(g)) {
        if (g->failing) return true;
        if (g->active >= 0 && !g->done[g->active]) return true;
        if (g->host.get_time(g->u) < g->pause_lock_until) return true;
    }
    return false;
}

/* ---------- mania pools ---------- */
typedef bool (*evt_pred)(const tb_game *g, int i);
static bool pred_pickup(const tb_game *g, int i) { (void)g; return g->events->data[i].type == TB_EV_PICKUP; }
static bool pred_answer(const tb_game *g, int i) { return g->events->data[i].type == TB_EV_ANSWER; }
static bool pred_clip(const tb_game *g, int i) { tb_event_type t = g->events->data[i].type; return t == TB_EV_INTRO || t == TB_EV_SOUND || t == TB_EV_PICKUP; }
static bool pred_break(const tb_game *g, int i) {
    const tb_event *e = &g->events->data[i];
    const tb_event *prev = i > 0 ? &g->events->data[i-1] : NULL;
    const tb_event *next = (size_t)(i+1) < g->events->count ? &g->events->data[i+1] : NULL;
    bool c1 = prev && prev->type == TB_EV_ANSWER && e->type != TB_EV_ANSWER;
    bool c2 = false;
    if (e->type == TB_EV_PICKUP) {
        bool followed = next && next->type == TB_EV_ANSWER && (next->start - e->start) < 14;
        c2 = !followed;
    }
    return c1 || c2;
}
static int mania_pick(tb_game *g, evt_pred p) {
    int count = 0;
    for (size_t i = 0; i < g->events->count; i++) if (p(g, (int)i)) count++;
    if (!count) return -1;
    int k = (int)floor(g->host.rand01(g->u) * count); if (k >= count) k = count - 1;
    int seen = 0;
    for (size_t i = 0; i < g->events->count; i++) if (p(g, (int)i)) { if (seen == k) return (int)i; seen++; }
    return -1;
}
static int count_pred(tb_game *g, evt_pred p) { int c = 0; for (size_t i = 0; i < g->events->count; i++) if (p(g, (int)i)) c++; return c; }

static double mania_speed(const tb_game *g) { return clampd(1 + g->m_round * 0.085, 1, 3.8); }
static double mania_window(const tb_game *g) { return clampd(3.0 - g->m_round * 0.085, 0.65, 3.0); }
static double mania_prompt_start(const tb_event *e) {
    if (!e) return 0;
    if (e->type == TB_EV_ANSWER) { double v = e->start + (60.0 / 30.0); return v > 0 ? v : 0; }
    double v = e->start - 0.05; return v > 0 ? v : 0;
}

static void stop_mania(tb_game *g) {
    g->host.mania_music(g->u, false);
    if (g->m_active) g->host.set_muted(g->u, g->m_was_muted);
    g->m_active = false; g->m_has_prompt = false; g->m_break_until = 0; g->m_clip_end = 0;
    g->host.set_rate(g->u, 1);
    g->ui.status_overlay = false;
}

/* forward */
static void mania_end(tb_game *g, const char *reason);

/* ---------- reset / lifecycle ---------- */
void tb_game_reset(tb_game *g) {
    stop_mania(g);
    g->cur_idx = 0; g->active = -1; g->score = 0; g->streak = 0; g->correct = 0; g->wrong = 0;
    g->ended = false; g->failing = false; g->pause_lock_until = 0;
    g->lives = very_hard(g) ? 1 : hard_mode(g) ? 3 : INT_MAX;
    for (int i = 0; i < g->n_play; i++) { g->done[i] = 0; g->got[i] = -1; }
    for (int i = 0; i < 4; i++) g->defs[i].active = false;
    buttons_enabled(g, false); pickup_enabled(g, false);
    g->ui.game_over = false; g->ui.results = false; g->ui.call_overlay = false; g->ui.status_overlay = false;
    g->ui.difficulty_overlay = false;
    update_hud(g);
}

tb_game *tb_game_create(const tb_event_list *events, const tb_host *host, void *user) {
    tb_game *g = (tb_game *)calloc(1, sizeof *g);
    if (!g) return NULL;
    g->events = events; g->host = *host; g->u = user; g->mode = TB_GM_EASY;
    g->active = -1; g->m_high = 0;
    /* build play index list */
    g->play = (int *)malloc(events->count * sizeof(int) + 4);
    for (size_t i = 0; i < events->count; i++)
        if (tb_event_is_play(&events->data[i])) g->play[g->n_play++] = (int)i;
    g->done = (unsigned char *)calloc((size_t)g->n_play + 1, 1);
    g->got = (int *)malloc(((size_t)g->n_play + 1) * sizeof(int));
    for (int i = 0; i < g->n_play; i++) g->got[i] = -1;
    for (size_t i = 0; i < events->count; i++) if (events->data[i].type == TB_EV_ANSWER) g->n_questions++;
    tb_game_reset(g);
    return g;
}

void tb_game_destroy(tb_game *g) { if (!g) return; free(g->play); free(g->done); free(g->got); free(g); }

void tb_game_set_mode(tb_game *g, tb_gamemode mode) {
    g->mode = mode;
    char action[16]; strcpy(action, g->pending_action); g->pending_action[0] = '\0';
    tb_game_reset(g);
    g->ui.difficulty_overlay = false;
    if (strcmp(action, "restart") == 0 || strcmp(action, "start") == 0)
        tb_game_start(g, true);
}
tb_gamemode tb_game_mode(const tb_game *g) { return g->mode; }

void tb_game_open_difficulty(tb_game *g, const char *pending_action) {
    /* Fully stop the current run before showing the difficulty prompt. Without this,
     * pressing Restart in Mania froze the footage but left the Mania music + state
     * machine running in the background. */
    stop_mania(g);
    g->running = false; g->m_has_prompt = false;
    ui_clear_active(g);
    g->ui.status_overlay = false; g->ui.call_overlay = false;
    g->host.pause(g->u);
    /* Copy: callers may pass a pointer into a temporary (e.g. QByteArray) that is
     * freed before the player picks a difficulty. Storing the pointer would be a
     * use-after-free that intermittently drops the pending start -> apparent lockup. */
    if (pending_action) { strncpy(g->pending_action, pending_action, sizeof g->pending_action - 1); g->pending_action[sizeof g->pending_action - 1] = '\0'; }
    else g->pending_action[0] = '\0';
    g->ui.difficulty_overlay = true;
}

/* ---------- mania core ---------- */
static void start_mania(tb_game *g) {
    g->ui.difficulty_overlay = false; g->ui.results = false;
    tb_game_reset(g);
    g->running = true; g->ended = false;
    g->m_active = true; g->m_round = 0; g->m_has_prompt = false;
    g->m_next_at = now_ms(g) + 350; g->m_break_until = 0; g->m_clip_end = 0;
    g->m_was_muted = false; /* JS stores video.muted (the visible track); restored on stop */
    g->host.set_muted(g->u, true);
    g->host.set_rate(g->u, 1);
    g->host.mania_music(g->u, true);
    ui_status(g, "MANIA", TB_STATUS_WARN, 700);
    g->host.play(g->u);
}

static void mania_end(tb_game *g, const char *reason) {
    if (!g->m_active) return;
    stop_mania(g);
    g->ended = true; g->running = false;
    g->host.pause(g->u);
    g->host.play_sfx(g->u, TB_SFX_GAMEOVER);
    if (g->score > g->m_high) g->m_high = g->score;
    char line[200];
    snprintf(line, sizeof line, "Mania round %d \xC2\xB7 high score %ld%s%s",
             g->m_round, g->m_high, reason ? " \xC2\xB7 " : "", reason ? reason : "");
    configure_results(g, "Mania Over", line, true, true, false);
    g->ui.results = true;
    g->ui.pickup_enabled = true; g->ui.pickup_live = true;   /* press Pick Up to play again */
    update_hud(g);
}

static void mania_break(tb_game *g, double now) {
    int e = mania_pick(g, pred_break);
    if (e < 0) e = mania_pick(g, pred_clip);
    if (e < 0 && g->events->count) e = (int)floor(g->host.rand01(g->u) * g->events->count);
    g->m_has_prompt = false;
    g->m_break_until = now + 2600; g->m_next_at = g->m_break_until + 220;
    ui_clear_active(g);
    ui_status(g, "BREAK", TB_STATUS_WARN, 0);
    if (e >= 0) {
        const tb_event *ev = &g->events->data[e];
        do_seek(g, ev->start, 80);
        g->m_clip_end = ev->start + 2.4;
        g->host.set_rate(g->u, clampd(mania_speed(g) + 0.6, 1.5, 4));
        schedule(g, DEF_MANIA_PLAY, 80, 0, NULL, false);
    }
}

static void mania_prompt(tb_game *g, double now) {
    g->ui.status_overlay = false;
    int npick = count_pred(g, pred_pickup), nans = count_pred(g, pred_answer);
    if (!npick && !nans) { mania_end(g, "no decoded prompts"); return; }
    g->m_round++;
    bool forcePickup = g->m_round == 1 && npick;
    bool forceAnswer = g->m_round == 2 && nans;
    bool useAnswer = forceAnswer || (!forcePickup && nans &&
        g->host.rand01(g->u) < clampd(0.28 + g->m_round * 0.015, 0.28, 0.62));
    int src = useAnswer ? mania_pick(g, pred_answer)
                        : mania_pick(g, npick ? pred_pickup : pred_answer);
    if (src < 0) { mania_end(g, "no decoded prompts"); return; }
    const tb_event *source = &g->events->data[src];
    double speed = mania_speed(g);
    double win = mania_window(g);
    int multi = source->type == TB_EV_PICKUP
        ? (int)fmin(1 + floor(fmax(0, g->m_round - 6) / 5.0), 4) : 1;
    double promptStart = mania_prompt_start(source);

    g->m_has_prompt = true; g->p_type = source->type; g->p_answer = source->answer;
    g->p_need = multi; g->p_got = 0; g->p_deadline_ms = now + win * 1000; g->p_promptstart = promptStart;
    g->p_source = src; g->active = -1;
    g->ui.transition_seq++; g->ui.transition_dir = g->m_round & 3;   /* slide-in transition */

    do_seek(g, promptStart, 0);
    double clipCap = promptStart + clampd(2.2 / speed, 0.55, 2.2);
    double srcEnd = source->end ? source->end : promptStart + 2;
    g->m_clip_end = srcEnd < clipCap ? srcEnd : clipCap;
    g->host.set_rate(g->u, speed);

    if (source->type == TB_EV_PICKUP) {
        schedule(g, DEF_MANIA_PLAY, 70, 0, NULL, false);
        pickup_enabled(g, true);
        g->ui.call_overlay = true;
        if (multi > 1) snprintf(g->ui.call_title, sizeof g->ui.call_title, "PICK UP x%d", multi);
        else strcpy(g->ui.call_title, "PICK UP");
        strcpy(g->ui.call_hint, "Fast!");
    } else {
        g->m_clip_end = 0;
        schedule(g, DEF_MANIA_PAUSE, 90, 0, NULL, false);
        buttons_enabled(g, true);
        g->ui.call_overlay = false;
    }
    update_hud(g);
}

static void mania_success(tb_game *g) {
    if (!g->m_has_prompt) return;
    g->ui.status_overlay = false;
    ui_clear_active(g);
    g->correct++; g->streak++; if (g->streak > g->best) g->best = g->streak;
    g->score += (long)round(75 + g->m_round * 12 + g->streak * 8);
    g->host.play_sfx(g->u, g->p_type == TB_EV_ANSWER ? TB_SFX_MANIA_ANSWER : TB_SFX_MANIA_PICKUP);
    g->m_has_prompt = false;
    g->m_next_at = now_ms(g) + clampd(520 - g->m_round * 12, 80, 520);
    if (g->score > g->m_high) g->m_high = g->score;
    update_hud(g);
}

static void mania_fail(tb_game *g, const char *reason) {
    g->wrong++; g->streak = 0; ui_clear_active(g); g->m_has_prompt = false;
    mania_end(g, reason ? reason : "miss");
}

static void mania_loop(tb_game *g) {
    double now = now_ms(g);
    if (!g->m_active) return;
    if (g->m_has_prompt && now > g->p_deadline_ms) { mania_fail(g, "too slow"); return; }
    if (g->m_clip_end && vtime(g) > g->m_clip_end) {
        if (g->m_has_prompt && g->p_source >= 0) {
            double ps = g->p_promptstart != 0 ? g->p_promptstart : mania_prompt_start(&g->events->data[g->p_source]);
            do_seek(g, ps, 50);
        } else if (g->m_break_until && now < g->m_break_until) {
            int e = mania_pick(g, pred_break); if (e < 0) e = mania_pick(g, pred_clip);
            if (e >= 0) { do_seek(g, g->events->data[e].start, 50); g->m_clip_end = g->events->data[e].start + 2.0; }
        }
    }
    if (!g->m_has_prompt && now >= g->m_next_at) {
        if (g->m_round > 0 && (g->m_round % 9) == 0 && (!g->m_break_until || now > g->m_break_until + 500))
            mania_break(g, now);
        else { g->m_break_until = 0; mania_prompt(g, now); }
    }
}

/* ---------- non-mania success/fail ---------- */
static void activate(tb_game *g, int play_idx) {
    g->active = play_idx;
    const tb_event *e = play_event(g, play_idx);
    if (e->type == TB_EV_PICKUP) {
        pickup_enabled(g, true);
        g->ui.call_overlay = true;
        strcpy(g->ui.call_title, "INCOMING CALL");
        strcpy(g->ui.call_hint, hard_mode(g) ? "Pick up before the cue ends" : "Pick up the phone");
    } else if (e->type == TB_EV_ANSWER) {
        buttons_enabled(g, true);
        if (!hard_mode(g)) ui_status(g, "ANSWER", TB_STATUS_WARN, 700);
    }
}

static void succeed(tb_game *g, int play_idx, const char *label) {
    tb_event *e = (tb_event *)play_event(g, play_idx);
    g->done[play_idx] = 1; ui_clear_active(g);
    if (e->type == TB_EV_ANSWER) g->correct++;
    g->streak++; if (g->streak > g->best) g->best = g->streak;
    long bonus = e->type == TB_EV_ANSWER ? (100 + (g->streak - 1) * 25) : 50;
    g->score += bonus;
    g->host.play_sfx(g->u, TB_SFX_CORRECT);
    if (!hard_mode(g)) ui_status(g, label ? label : "CORRECT", TB_STATUS_GOOD, 800);
    if (hard_mode(g)) { double me = mode_end(g, e); if (me > g->pause_lock_until) g->pause_lock_until = me; }
    if (g->cur_idx < g->n_play && g->play[g->cur_idx] == g->play[play_idx]) g->cur_idx++;
    if (very_hard(g) && e->type == TB_EV_ANSWER) {
        double t = mode_end(g, e) + 0.05;
        if (vtime(g) < t) do_seek(g, t, 100);
    }
    update_hud(g);
}

static void show_choice_after_gameover(tb_game *g, const char *sub, double delay) {
    g->ended = true; g->running = false; g->failing = true;
    ui_clear_active(g);
    g->host.play_sfx(g->u, TB_SFX_GAMEOVER);
    update_hud(g);
    g->host.pause(g->u);
    g->ui.game_over = true; strcpy(g->ui.game_over_text, "GAME OVER");
    strncpy(g->ui.game_over_sub, sub ? sub : "Try again", sizeof g->ui.game_over_sub - 1);
    g->ui.game_over_sub[sizeof g->ui.game_over_sub - 1] = '\0';
    schedule(g, DEF_GAMEOVER_RESULTS, delay, 0, g->ui.game_over_sub, false);
}

static void very_hard_start_over(tb_game *g, const char *reason) {
    if (reason && strcmp(reason, "early") == 0) g->wrong++;
    g->streak = 0; g->lives = 0;
    show_choice_after_gameover(g, (reason && strcmp(reason, "early") == 0) ? "Early input" : "Start over", 1000);
}

static void fail_event(tb_game *g, int play_idx, const char *reason) {
    if (g->done[play_idx]) return;
    tb_event *e = (tb_event *)play_event(g, play_idx);
    if (e->type == TB_EV_ANSWER) g->wrong++;
    g->streak = 0;
    ui_clear_active(g);
    if (!hard_mode(g)) {
        g->done[play_idx] = 1;
        ui_status(g, (reason && strcmp(reason, "timeout") == 0) ? "TIME OUT" : "WRONG", TB_STATUS_BAD, 1000);
        g->host.play_sfx(g->u, TB_SFX_WRONG);
        if (g->cur_idx < g->n_play && g->play[g->cur_idx] == g->play[play_idx]) g->cur_idx++;
        update_hud(g); return;
    }
    if (very_hard(g)) { very_hard_start_over(g, reason); return; }
    /* hard mode: lose a life, Dragon's-Lair retry */
    g->lives--; g->failing = true;
    g->host.play_sfx(g->u, TB_SFX_GAMEOVER);
    update_hud(g);
    g->host.pause(g->u);
    g->ui.game_over = true;
    bool dead = g->lives <= 0;
    strcpy(g->ui.game_over_text, dead ? "GAME OVER" : "MISS!");
    if (dead) strcpy(g->ui.game_over_sub, "Out of lives");
    else snprintf(g->ui.game_over_sub, sizeof g->ui.game_over_sub, "%d %s left - try again",
                  g->lives, g->lives == 1 ? "life" : "lives");
    schedule(g, dead ? DEF_HARD_RESULTS : DEF_HARD_RETRY, 1300, play_idx, NULL, dead);
}

/* ---------- input ---------- */
static const tb_event *early_cue_at(tb_game *g, double t) {
    for (int i = 0; i < g->n_play; i++) {
        const tb_event *e = play_event(g, i);
        if (e->type == TB_EV_ANSWER && e->delayed_cue && !g->done[i] &&
            isfinite(e->early_guard_start) && t >= e->early_guard_start && t < e->start)
            return e;
    }
    return NULL;
}

void tb_game_on_answer(tb_game *g, int n) {
    if (mania_mode(g) && g->m_active) {
        if (!g->m_has_prompt || g->p_type != TB_EV_ANSWER) { mania_fail(g, "early input"); return; }
        if (n == g->p_answer) mania_success(g); else mania_fail(g, "wrong answer");
        return;
    }
    int a = g->active;
    if (a < 0 || play_event(g, a)->type != TB_EV_ANSWER || g->done[a]) {
        const tb_event *early = early_cue_at(g, vtime(g));
        if (early && very_hard(g) && g->running && !g->ended && !g->failing) very_hard_start_over(g, "early");
        else if (early && !hard_mode(g)) ui_status(g, "TOO EARLY", TB_STATUS_WARN, 700);
        else if (very_hard(g) && g->running && !g->ended && !g->failing) very_hard_start_over(g, "early");
        else if (!hard_mode(g)) ui_status(g, "WAIT", TB_STATUS_WARN, 500);
        return;
    }
    if (n == play_event(g, a)->answer) succeed(g, a, "CORRECT");
    else fail_event(g, a, "wrong");
}

void tb_game_on_pickup(tb_game *g) {
    /* In Mania, once it's over the Pick-up button restarts the run (no need for Restart). */
    if (mania_mode(g) && !g->m_active && g->ended) { tb_game_start(g, true); return; }
    if (mania_mode(g) && g->m_active) {
        if (!g->m_has_prompt || g->p_type != TB_EV_PICKUP) { mania_fail(g, "early input"); return; }
        g->p_got++;
        if (g->p_got >= g->p_need) mania_success(g);
        else { snprintf(g->ui.call_title, sizeof g->ui.call_title, "PICK UP x%d", g->p_need - g->p_got); g->host.play_sfx(g->u, TB_SFX_MANIA_STEP); }
        return;
    }
    int a = g->active;
    if (a < 0 || play_event(g, a)->type != TB_EV_PICKUP || g->done[a]) {
        if (very_hard(g) && g->running && !g->ended && !g->failing) very_hard_start_over(g, "early");
        return;
    }
    succeed(g, a, "CONNECTED");
}

/* ---------- start / end ---------- */
void tb_game_start(tb_game *g, bool restart) {
    if (mania_mode(g)) { start_mania(g); return; }
    g->ui.difficulty_overlay = false; g->ui.results = false;
    tb_game_reset(g);
    g->running = true;
    if (restart) do_seek(g, 0, 120);
    g->host.play(g->u);
}
void tb_game_restart(tb_game *g) { tb_game_start(g, true); }

void tb_game_on_media_ended(tb_game *g) {
    if (mania_mode(g) && g->m_active) { mania_end(g, "video ended"); return; }
    g->ended = true; g->running = false; ui_clear_active(g);
    stop_mania(g);
    g->host.pause(g->u);
    char line[200];
    snprintf(line, sizeof line, "%d / %d correct  \xC2\xB7  best streak %ld%s",
             g->correct, g->n_questions, g->best,
             hard_mode(g) ? (g->lives > 0 ? "  \xC2\xB7  cleared!" : "  \xC2\xB7  out of lives") : "");
    configure_results(g, "Results", line, true, true, false);
    g->ui.results = true;
    if (hard_mode(g) && g->lives > 0) g->host.play_sfx(g->u, TB_SFX_WIN);
}

/* ---------- deferred + main tick ---------- */
static void run_deferred(tb_game *g, deferred *d) {
    switch (d->kind) {
        case DEF_CLEAR_SEEK: g->seek_lock_until = 0; break;
        case DEF_HARD_RETRY: {
            /* phase 1: hide game-over, seek to checkpoint, re-arm; stay frozen */
            g->ui.game_over = false;
            int play_idx = d->evt;
            const tb_event *e = play_event(g, play_idx);
            double t = isfinite(e->checkpoint) ? e->checkpoint : e->start - 0.6; if (t < 0) t = 0;
            g->host.seek(g->u, t);
            g->active = -1; g->cur_idx = play_idx;
            schedule(g, DEF_HARD_PHASE2, 220, 0, NULL, false);
            break;
        }
        case DEF_HARD_PHASE2:
            g->seek_lock_until = 0; g->failing = false; g->host.play(g->u); break;
        case DEF_HARD_RESULTS:
            g->ui.game_over = false; g->ended = true; g->running = false; g->failing = false;
            configure_results(g, "Game Over", "Out of lives", true, true, true);
            g->ui.results = true; update_hud(g); break;
        case DEF_GAMEOVER_RESULTS:
            g->ui.game_over = false; g->failing = false;
            configure_results(g, "Game Over", d->sub[0] ? d->sub : "Try again", true, true, true);
            g->ui.results = true; update_hud(g); break;
        case DEF_MANIA_PLAY: g->seek_lock_until = 0; g->host.play(g->u); break;
        case DEF_MANIA_PAUSE: g->seek_lock_until = 0; g->host.pause(g->u); break;
        default: break;
    }
}

void tb_game_tick(tb_game *g) {
    double now = now_ms(g);

    /* auto-hide timed status overlay */
    if (g->ui.status_overlay && g->status_until_ms > 0 && now >= g->status_until_ms)
        g->ui.status_overlay = false;

    /* process deferred transitions */
    for (int i = 0; i < 4; i++) {
        if (g->defs[i].active && now >= g->defs[i].at_ms) {
            deferred d = g->defs[i]; g->defs[i].active = false;
            run_deferred(g, &d);
        }
    }

    double t = vtime(g);
    if (mania_mode(g) && g->m_active) { mania_loop(g); update_hud(g); return; }
    if (g->failing) { update_hud(g); return; }
    if (!g->ended && !internal_seek(g)) {
        if (g->active >= 0 && t > mode_end(g, play_event(g, g->active)) + 0.02 && !g->done[g->active]) {
            fail_event(g, g->active, "timeout"); update_hud(g); return;
        }
        if (g->active < 0 && g->cur_idx < g->n_play) {
            int pi = g->cur_idx;
            const tb_event *e = play_event(g, pi);
            if (g->done[pi]) g->cur_idx++;
            else if (t >= e->start && t <= e->end) activate(g, pi);
            else if (t > e->end) { g->done[pi] = 1; g->cur_idx++; }
        }
    }
    update_hud(g);
}

const tb_ui *tb_game_ui(const tb_game *g) { return &g->ui; }
bool tb_game_running(const tb_game *g) { return g->running; }
bool tb_game_ended(const tb_game *g) { return g->ended; }

/* ---------- save states ---------- */
bool tb_game_mode_allows_save(tb_gamemode mode) { return mode == TB_GM_EASY || mode == TB_GM_HARD; }

bool tb_game_snapshot(const tb_game *g, tb_savestate *out) {
    if (!tb_game_mode_allows_save(g->mode)) return false;
    memset(out, 0, sizeof *out);
    out->version = 2; out->mode = g->mode; out->time = g->host.get_time(g->u);
    if (out->time < 0) out->time = 0;
    out->cur_idx = g->cur_idx; out->score = g->score; out->streak = g->streak; out->best = g->best;
    out->lives = g->lives; out->correct = g->correct; out->wrong = g->wrong;
    out->pause_lock_until = g->pause_lock_until;
    out->n_play = g->n_play;
    out->done = (unsigned char *)malloc((size_t)g->n_play + 1);
    out->got = (int *)malloc(((size_t)g->n_play + 1) * sizeof(int));
    if (!out->done || !out->got) { tb_savestate_free(out); return false; }
    memcpy(out->done, g->done, (size_t)g->n_play);
    memcpy(out->got, g->got, (size_t)g->n_play * sizeof(int));
    out->saved_at_ms = (long long)now_ms((tb_game *)g);
    return true;
}
void tb_savestate_free(tb_savestate *s) { if (!s) return; free(s->done); free(s->got); s->done = NULL; s->got = NULL; }

bool tb_game_restore(tb_game *g, const tb_savestate *s) {
    if (!tb_game_mode_allows_save(s->mode)) return false;
    stop_mania(g);
    g->host.pause(g->u);
    g->mode = s->mode;
    g->ui.results = false; g->ui.game_over = false; g->ui.status_overlay = false; g->ui.difficulty_overlay = false;
    ui_clear_active(g);
    int n = s->n_play < g->n_play ? s->n_play : g->n_play;
    for (int i = 0; i < n; i++) { g->done[i] = s->done[i]; g->got[i] = s->got[i]; }
    g->cur_idx = s->cur_idx < 0 ? 0 : (s->cur_idx > g->n_play ? g->n_play : s->cur_idx);
    g->score = s->score; g->streak = s->streak; g->best = s->best;
    g->lives = s->mode == TB_GM_HARD ? (s->lives < 1 ? 3 : (s->lives > 9 ? 9 : s->lives)) : INT_MAX;
    g->correct = s->correct; g->wrong = s->wrong;
    g->ended = false; g->running = true; g->failing = false;
    g->pause_lock_until = s->pause_lock_until; g->active = -1;
    do_seek(g, s->time, 180);
    /* Loading a save state is an active resume operation.  The restore path
     * pauses before seeking so queued audio from the old position cannot leak
     * through, but leaving the player paused makes the user press Play again and
     * also leaves the UI in a misleading running-but-silent state.  Restart
     * playback after the seek has been requested; the frontend player will clear
     * and rebase its audio stream from the restored timestamp. */
    g->host.play(g->u);
    update_hud(g);
    return true;
}

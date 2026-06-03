/* tb_test_game.c - drive the game machine with a mock host (no media). */
#include "../src/core/tb_game.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct { double t; double dur; bool paused; double rate; bool muted; double clock; } mock;
static mock M;

static void h_seek(void *u, double s) { (void)u; M.t = s; }
static void h_play(void *u) { (void)u; M.paused = false; }
static void h_pause(void *u) { (void)u; M.paused = true; }
static void h_rate(void *u, double r) { (void)u; M.rate = r; }
static void h_mute(void *u, bool m) { (void)u; M.muted = m; }
static double h_time(void *u) { (void)u; return M.t; }
static double h_dur(void *u) { (void)u; return M.dur; }
static bool h_isp(void *u) { (void)u; return M.paused; }
static void h_sfx(void *u, tb_sfx s) { (void)u; (void)s; }
static double h_now(void *u) { (void)u; return M.clock; }
static double h_rand(void *u) { (void)u; return 0.5; }
static void h_music(void *u, bool on) { (void)u; (void)on; }

int main(void) {
    /* synthetic timeline: pickup @5, answer(ans=2) @10, answer(ans=4) @20 */
    tb_event_list ev; tb_event_list_init(&ev);
    tb_event a; memset(&a, 0, sizeof a);
    a.type = TB_EV_PICKUP; a.start = 5; a.end = 9; ev.data = malloc(8 * sizeof(tb_event)); ev.cap = 8;
    ev.data[0] = a;
    memset(&a, 0, sizeof a); a.type = TB_EV_ANSWER; a.start = 10; a.end = 18; a.answer = 2; a.checkpoint = 4; ev.data[1] = a;
    memset(&a, 0, sizeof a); a.type = TB_EV_ANSWER; a.start = 20; a.end = 28; a.answer = 4; a.checkpoint = 14; ev.data[2] = a;
    ev.count = 3;

    tb_host host = { h_seek, h_play, h_pause, h_rate, h_mute, h_time, h_dur, h_isp, h_sfx, h_now, h_rand, h_music };
    M.dur = 30; M.rate = 1; M.paused = true; M.t = 0; M.clock = 0;

    tb_game *g = tb_game_create(&ev, &host, NULL);
    tb_game_set_mode(g, TB_GM_EASY);
    tb_game_start(g, true);

    /* simulate playback advancing 0.1s per tick, 100ms clock per tick */
    int answered_pickup = 0, answered_q1 = 0, answered_q2 = 0;
    for (int i = 0; i < 350; i++) {
        M.t += 0.1; M.clock += 100;
        tb_game_tick(g);
        const tb_ui *ui = tb_game_ui(g);
        if (ui->pickup_enabled && !answered_pickup) { tb_game_on_pickup(g); answered_pickup = 1; }
        if (ui->buttons_enabled && M.t < 15 && !answered_q1) { tb_game_on_answer(g, 2); answered_q1 = 1; }
        if (ui->buttons_enabled && M.t >= 15 && !answered_q2) { tb_game_on_answer(g, 4); answered_q2 = 1; }
    }
    tb_game_on_media_ended(g);
    const tb_ui *ui = tb_game_ui(g);
    printf("EASY: score=%ld streak=%ld correct=%d/%d  results='%s' line='%s'\n",
           ui->score, ui->best, ui->correct, ui->n_questions, ui->result_title, ui->result_line);

    int ok = (ui->correct == 2 && answered_pickup && ui->score > 0);
    printf("EASY %s\n", ok ? "PASS" : "FAIL");

    /* hard mode wrong-answer life loss */
    M.t = 0; M.clock = 1000000; M.paused = true;
    tb_game_set_mode(g, TB_GM_HARD);
    tb_game_start(g, true);
    int wrong_done = 0; int saw_gameover = 0;
    for (int i = 0; i < 600; i++) {
        M.t += 0.1; M.clock += 100;
        tb_game_tick(g);
        const tb_ui *u2 = tb_game_ui(g);
        if (u2->pickup_enabled) tb_game_on_pickup(g);
        if (u2->buttons_enabled) {
            /* q1 expects 2, q2 expects 4: miss q1 exactly once, then play perfectly */
            int want = (M.t < 15) ? 2 : 4;
            if (!wrong_done && M.t < 15) { tb_game_on_answer(g, 1 /*wrong*/); wrong_done = 1; }
            else tb_game_on_answer(g, want);
        }
        if (u2->game_over) saw_gameover = 1;
    }
    const tb_ui *u2 = tb_game_ui(g);
    printf("HARD: lives=%d saw_gameover=%d  (started 3, one miss => 2)\n", u2->lives, saw_gameover);
    printf("HARD %s\n", (u2->lives == 2 && saw_gameover) ? "PASS" : "FAIL");

    tb_game_destroy(g);
    free(ev.data);
    return ok ? 0 : 1;
}

/* tb_gamedb.c - runtime use of the generated game database. */
#include "tb_gamedb.h"

#include <string.h>
#include <math.h>

const char *tb_gamedb_name(int idx) {
    return (idx >= 0 && idx < tb_gamedb_count) ? tb_gamedb_games[idx].name : NULL;
}
const char *tb_gamedb_mania_anchor(int idx) {
    if (idx < 0 || idx >= tb_gamedb_count) return NULL;
    const char *a = tb_gamedb_games[idx].mania_anchor;
    return (a && a[0]) ? a : NULL;
}

/* Fraction of a game's answer-value sequence matched, index-aligned, against the
 * loaded answers (the values 1-4 are stable across rips even when the raw bytes
 * differ). */
static double answer_seq_score(const tb_gamedb_game *g, const tb_event_list *ev) {
    int live[256], nlive = 0;
    for (size_t i = 0; i < ev->count && nlive < 256; i++)
        if (ev->data[i].type == TB_EV_ANSWER && ev->data[i].answer >= 1 && ev->data[i].answer <= 4)
            live[nlive++] = ev->data[i].answer;
    if (nlive == 0 || g->n_answers == 0) return 0;
    int n = nlive < g->n_answers ? nlive : g->n_answers;
    int match = 0;
    for (int i = 0; i < n; i++) if (live[i] == g->answer_vals[i]) match++;
    int span = nlive > g->n_answers ? nlive : g->n_answers;   /* penalise count mismatch */
    return (double)match / span;
}

int tb_gamedb_detect(const tb_event_list *events) {
    int best = -1; double bestScore = 0.70;   /* require >=70% of the sequence to align */
    for (int g = 0; g < tb_gamedb_count; g++) {
        double s = answer_seq_score(&tb_gamedb_games[g], events);
        if (s > bestScore) { bestScore = s; best = g; }
    }
    return best;
}

static double round2(double x) { return round(x * 100.0) / 100.0; }

void tb_gamedb_apply_cues(int idx, tb_event_list *events, double carrier) {
    (void)carrier;
    if (idx < 0 || idx >= tb_gamedb_count) return;
    const tb_gamedb_game *g = &tb_gamedb_games[idx];
    /* The game is already identified, so cues match by TIMING (minute/second window,
     * preceding sound packet, gap) rather than the raw answer bytes, which vary per
     * rip. This is the "press at a specific moment" rule for that game. */
    for (int c = 0; c < g->n_cues; c++) {
        const tb_gamedb_cue *cue = &g->cues[c];
        for (size_t i = 0; i < events->count; i++) {
            tb_event *e = &events->data[i];
            if (e->type != TB_EV_ANSWER) continue;
            int minute = (int)floor(e->start / 60.0);
            double second = e->start - minute * 60.0;
            if (minute != cue->minute || second < cue->sec_lo || second > cue->sec_hi) continue;
            if (i == 0) continue;
            tb_event *prev = &events->data[i - 1];
            if (cue->prev_is_sound && prev->type != TB_EV_SOUND) continue;
            double gap = e->start - prev->start;
            if (gap < cue->gap_lo || gap > cue->gap_hi) continue;
            e->delayed_cue = true;
            e->early_guard_start = round2(prev->start);
            e->cue_gap = round2(gap);
        }
    }
}

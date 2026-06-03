/* tb_events.c - faithful C11 port of buildEvents(). */
#include "tb_events.h"
#include "tb_gamedb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void tb_event_list_init(tb_event_list *l) { l->data = NULL; l->count = 0; l->cap = 0; }
void tb_event_list_free(tb_event_list *l) { free(l->data); l->data = NULL; l->count = l->cap = 0; }

static void event_push(tb_event_list *l, const tb_event *e) {
    if (l->count == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 64;
        tb_event *nd = realloc(l->data, nc * sizeof(*nd));
        if (!nd) return;
        l->data = nd; l->cap = nc;
    }
    l->data[l->count++] = *e;
}

static double round2(double x) { return round(x * 100.0) / 100.0; }
static double clampd(double n, double a, double b) { return n < a ? a : (n > b ? b : n); }

static void byte_hex(const unsigned char *b, int n, char *out, size_t outsz) {
    out[0] = '\0';
    size_t pos = 0;
    for (int i = 0; i < n && pos + 3 < outsz; i++) {
        pos += (size_t)snprintf(out + pos, outsz - pos, i ? " %02X" : "%02X", b[i]);
    }
}

static int exact_byte_answer(const unsigned char *bytes, int nbytes) {
    if (nbytes < 3 || bytes[0] != 0x0D) return 0;
    int marker = bytes[2] & 0x0F;
    if (!(marker == 0x0E || marker == 0x0F)) return 0;
    return ((bytes[2] >> 6) & 3) + 1;
}

/* weighted majority vote over a set of packet pointers (voteAnswer). */
static int vote_answer(const tb_packet **ps, size_t n) {
    double scores[5] = {0,0,0,0,0};
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        const tb_packet *p = ps[i];
        if (p->answer < 1 || p->answer > 4) continue;
        any = true;
        double w = (p->answer_confidence > 0 ? p->answer_confidence : 0.5)
                 + (p->quality > 0 ? p->quality : 0)
                 + fmin(1.0, p->nbits / 40.0);
        scores[p->answer] += w;
    }
    if (!any) return 0;
    int best = 0; double bestScore = -1e18;
    for (int a = 1; a <= 4; a++) if (scores[a] > bestScore) { bestScore = scores[a]; best = a; }
    return best;
}

/* distinct count of answers (1..4) in a pointer set; returns the single value via *only. */
static int distinct_answers(const tb_packet **ps, size_t n, int *only) {
    bool seen[5] = {0,0,0,0,0};
    int cnt = 0, last = 0;
    for (size_t i = 0; i < n; i++) {
        int a = ps[i]->answer;
        if (a >= 1 && a <= 4 && !seen[a]) { seen[a] = true; cnt++; last = a; }
    }
    if (only) *only = (cnt == 1) ? last : 0;
    return cnt;
}

static int distinct_byte_answers(const tb_packet **ps, size_t n, int *only) {
    bool seen[5] = {0,0,0,0,0};
    int cnt = 0, last = 0;
    for (size_t i = 0; i < n; i++) {
        int a = exact_byte_answer(ps[i]->bytes, ps[i]->nbytes);
        if (a >= 1 && a <= 4 && !seen[a]) { seen[a] = true; cnt++; last = a; }
    }
    if (only) *only = (cnt == 1) ? last : 0;
    return cnt;
}

/* Delayed cues now live in the game database (gamedb/*.txt -> tb_gamedb). */
static void apply_delayed_cues(tb_event_list *events, double carrier) {
    int g = tb_gamedb_detect(events);
    if (g >= 0) tb_gamedb_apply_cues(g, events, carrier);
}

bool tb_event_is_play(const tb_event *e) {
    return e->type == TB_EV_PICKUP || e->type == TB_EV_ANSWER;
}

static tb_event_type kind_to_event(tb_kind k) {
    switch (k) {
        case TB_KIND_PICKUP: return TB_EV_PICKUP;
        case TB_KIND_SOUND:  return TB_EV_SOUND;
        case TB_KIND_INTRO:  return TB_EV_INTRO;
        default:             return TB_EV_SOUND;
    }
}

void tb_build_events(const tb_packet_list *packets, double carrier, tb_event_list *out) {
    /* ps = packets with kind || answerCandidate, already time-sorted by decoder. */
    size_t pcap = 256, pn = 0;
    const tb_packet **ps = malloc(pcap * sizeof(*ps));
    if (!ps) return;
    for (size_t i = 0; i < packets->count; i++) {
        const tb_packet *p = &packets->data[i];
        if (p->kind != TB_KIND_NONE || p->answer_candidate) {
            if (pn == pcap) { pcap *= 2; const tb_packet **np = realloc(ps, pcap * sizeof(*ps)); if (!np) break; ps = np; }
            ps[pn++] = p;
        }
    }

    /* group with gap < 1.0s */
    for (size_t gi = 0; gi < pn; ) {
        size_t gj = gi + 1;
        double t1 = ps[gi]->time;
        while (gj < pn && ps[gj]->time - t1 < 1.0) { t1 = ps[gj]->time; gj++; }
        double g_t0 = ps[gi]->time, g_t1 = t1;
        size_t gcount = gj - gi;
        const tb_packet **items = &ps[gi];

        /* exact = kind==answer && 1..4 ; ans = (answer||candidate) && 1..4 */
        const tb_packet *exact[256]; size_t nexact = 0;
        const tb_packet *ans[256];   size_t nans = 0;
        for (size_t k = 0; k < gcount && k < 256; k++) {
            const tb_packet *p = items[k];
            bool valid = p->answer >= 1 && p->answer <= 4;
            if (p->kind == TB_KIND_ANSWER && valid) exact[nexact++] = p;
            if ((p->kind == TB_KIND_ANSWER || p->answer_candidate) && valid) ans[nans++] = p;
        }

        if (nexact > 0) {
            double t0 = exact[0]->time;
            for (size_t k = 1; k < nexact; k++) if (exact[k]->time < t0) t0 = exact[k]->time;

            const tb_packet *primary[256]; size_t nprimary = 0;
            for (size_t k = 0; k < nexact; k++) if (exact[k]->time - t0 < 0.12) primary[nprimary++] = exact[k];

            const tb_packet **voteSet = nprimary ? primary : exact;
            size_t voteN = nprimary ? nprimary : nexact;
            int answer = vote_answer(voteSet, voteN);

            bool anyCandidate = false;
            for (size_t k = 0; k < nans; k++) if (ans[k]->answer_candidate) { anyCandidate = true; break; }

            int primaryOnly = 0; distinct_answers(voteSet, voteN, &primaryOnly);

            if (anyCandidate) {
                bool correctedByCandidate = false;
                const tb_packet *cand[256]; size_t ncand = 0;
                for (size_t k = 0; k < nans; k++)
                    if (ans[k]->answer_candidate && ans[k]->time - t0 >= 0.15 && ans[k]->time - t0 <= 0.45)
                        cand[ncand++] = ans[k];
                int candOnly = 0; int candDistinct = distinct_answers(cand, ncand, &candOnly);
                int primDistinct = distinct_answers(voteSet, voteN, &primaryOnly);
                if (primDistinct == 1 && candDistinct == 1 && primaryOnly != candOnly) {
                    double candQ = 0; bool primaryHasGapRun = false;
                    for (size_t k = 0; k < ncand; k++) if (cand[k]->quality > candQ) candQ = cand[k]->quality;
                    for (size_t k = 0; k < voteN; k++) if (voteSet[k]->mode == TB_MODE_GAP_RUN) { primaryHasGapRun = true; break; }
                    if (candQ >= 0.82 && primaryHasGapRun) { answer = candOnly; correctedByCandidate = true; }
                }
                if (!correctedByCandidate && answer == primaryOnly) {
                    int cv = vote_answer(ans, nans);
                    if (cv != 0) answer = cv;
                }
            }

            /* companion byte-answer agreement */
            const tb_packet *companions[256]; size_t ncomp = 0;
            for (size_t k = 0; k < nans; k++)
                if (ans[k]->time - t0 >= 0.15 && ans[k]->time - t0 <= 0.45) companions[ncomp++] = ans[k];
            if (ncomp > 0) {
                const tb_packet *primarySet[256]; size_t nprimarySet;
                if (nprimary) { memcpy(primarySet, primary, nprimary * sizeof(*primary)); nprimarySet = nprimary; }
                else { nprimarySet = 0; for (size_t k = 0; k < nexact; k++) if (exact[k]->time - t0 < 0.15) primarySet[nprimarySet++] = exact[k]; }
                int pbOnly = 0, cbOnly = 0;
                int pbD = distinct_byte_answers(primarySet, nprimarySet, &pbOnly);
                int cbD = distinct_byte_answers(companions, ncomp, &cbOnly);
                if (pbD == 1 && cbD == 1 && pbOnly == cbOnly && answer != pbOnly) answer = pbOnly;
            }

            /* window duration from unique sorted ans times */
            double times[256]; size_t ntimes = 0;
            for (size_t k = 0; k < nans; k++) {
                double r = round2(ans[k]->time); bool dup = false;
                for (size_t t = 0; t < ntimes; t++) if (times[t] == r) { dup = true; break; }
                if (!dup) times[ntimes++] = r;
            }
            for (size_t a = 1; a < ntimes; a++) { double key = times[a]; size_t b = a; while (b && times[b-1] > key) { times[b] = times[b-1]; b--; } times[b] = key; }
            double dur = 10;
            if (ntimes >= 2) { double gap = times[1] - times[0]; if (gap >= 0.15 && gap <= 0.45) dur = clampd(gap * 40, 7, 14); }

            tb_event e; memset(&e, 0, sizeof e);
            e.type = TB_EV_ANSWER; e.start = round2(t0); e.end = round2(t0 + dur); e.answer = answer;
            byte_hex(exact[0]->bytes, exact[0]->nbytes, e.bytes, sizeof e.bytes);
            event_push(out, &e);
        } else {
            /* most common typed kind */
            int counts[5] = {0,0,0,0,0}; tb_kind first_typed = TB_KIND_NONE; const tb_packet *typed0 = NULL;
            for (size_t k = 0; k < gcount; k++) {
                if (items[k]->kind != TB_KIND_NONE) {
                    counts[items[k]->kind]++;
                    if (!typed0) { typed0 = items[k]; first_typed = items[k]->kind; }
                }
            }
            if (typed0) {
                int bestc = -1; tb_kind bk = first_typed;
                /* iterate kinds in insertion-ish order; Object.entries order = first seen */
                tb_kind order[4] = {TB_KIND_ANSWER, TB_KIND_PICKUP, TB_KIND_SOUND, TB_KIND_INTRO};
                for (int o = 0; o < 4; o++) if (counts[order[o]] > bestc) { bestc = counts[order[o]]; bk = order[o]; }
                tb_event e; memset(&e, 0, sizeof e);
                e.type = kind_to_event(bk); e.start = round2(g_t0); e.end = round2(g_t1);
                byte_hex(typed0->bytes, typed0->nbytes, e.bytes, sizeof e.bytes);
                event_push(out, &e);
            }
        }
        gi = gj;
    }
    free(ps);

    /* pick-up reaction windows */
    for (size_t i = 0; i < out->count; i++) {
        if (out->data[i].type == TB_EV_PICKUP) {
            double winEnd;
            if (i + 1 < out->count) { double ns = out->data[i+1].start; double cap = out->data[i].start + 7; winEnd = ns < cap ? ns : cap; }
            else winEnd = out->data[i].start + 5;
            double lo = out->data[i].start + 2.0;
            out->data[i].end = round2(winEnd > lo ? winEnd : lo);
        }
    }

    /* checkpoints */
    for (size_t i = 0; i < out->count; i++) {
        tb_event *e = &out->data[i];
        if (e->type == TB_EV_ANSWER) {
            if (i > 0 && (e->start - out->data[i-1].start) < 16) {
                double v = out->data[i-1].start - 0.4; e->checkpoint = round2(v > 0 ? v : 0);
            } else { double v = e->start - 6; e->checkpoint = round2(v > 0 ? v : 0); }
        } else if (e->type == TB_EV_PICKUP) {
            double v = e->start - 0.6; e->checkpoint = round2(v > 0 ? v : 0);
        }
    }

    apply_delayed_cues(out, carrier);
}

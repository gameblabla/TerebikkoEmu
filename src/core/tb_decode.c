/* tb_decode.c - faithful C11 port of the JS DECODER_WORKER. */
#include "tb_decode.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------- packet list ---------------- */
void tb_packet_list_init(tb_packet_list *l) { l->data = NULL; l->count = 0; l->cap = 0; }
void tb_packet_list_free(tb_packet_list *l) { free(l->data); l->data = NULL; l->count = l->cap = 0; }

static void packet_list_push(tb_packet_list *l, const tb_packet *p) {
    if (l->count == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 256;
        tb_packet *nd = (tb_packet *)realloc(l->data, nc * sizeof(*nd));
        if (!nd) return; /* drop on OOM */
        l->data = nd; l->cap = nc;
    }
    l->data[l->count++] = *p;
}

static void report(tb_progress_fn fn, void *user, double v, const char *t) {
    if (fn) fn(v, t, user);
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* ---------------- bit helpers ---------------- */
/* parseInt(bits[off..off+len], 2) over '0'/'1' chars. */
static int bits_to_int(const char *bits, int off, int len) {
    int v = 0;
    for (int i = 0; i < len; i++) v = (v << 1) | (bits[off + i] == '1' ? 1 : 0);
    return v;
}

static int hamming_byte(int a, int b) {
    int x = (a ^ b) & 0xFF, n = 0;
    while (x) { n += x & 1; x >>= 1; }
    return n;
}

static void bits_to_bytes(const char *bits, int nbits, unsigned char *out, int *nout) {
    int usable = nbits - (nbits % 8), k = 0;
    for (int i = 0; i < usable && k < TB_MAX_BYTES; i += 8) out[k++] = (unsigned char)bits_to_int(bits, i, 8);
    *nout = k;
}

/* ---------------- carrier detection ---------------- */
double tb_detect_carrier(const float *samples, size_t n, double sr) {
    static const double cands[] = {6000, 6500, 7000, 7500, 8000, 8500, 9000};
    int win = (int)round(sr * 0.0015); if (win < 24) win = 24;
    int stride = win * 16;
    double best = 8000, best_ratio = 0;
    for (int ci = 0; ci < 7; ci++) {
        double f = cands[ci];
        double c2 = 2 * cos(2 * M_PI * f / sr);
        /* collect strided Goertzel magnitudes */
        size_t cap = 1024, cnt = 0;
        double *vals = (double *)malloc(cap * sizeof(double));
        if (!vals) return best;
        for (size_t pos = 0; pos + (size_t)win < n; pos += (size_t)stride) {
            double s1 = 0, s2 = 0;
            for (int k = 0; k < win; k++) { double s0 = samples[pos + k] + c2 * s1 - s2; s2 = s1; s1 = s0; }
            double mag = sqrt(fmax(0.0, s1 * s1 + s2 * s2 - c2 * s1 * s2)) / win;
            if (cnt == cap) { cap *= 2; double *nv = realloc(vals, cap * sizeof(double)); if (!nv) break; vals = nv; }
            vals[cnt++] = mag;
        }
        if (cnt) {
            qsort(vals, cnt, sizeof(double), cmp_double);   /* ascending for median + max */
            double med = vals[cnt / 2]; if (med <= 0) med = 1e-9;
            double mx = vals[cnt - 1];
            double ratio = mx / med;
            if (ratio > best_ratio) { best_ratio = ratio; best = f; }
        }
        free(vals);
    }
    return best;
}

/* ---------------- envelope ---------------- */
/* Returns a malloc'd Float array; caller frees. *out_bins set. */
static float *make_envelope(const float *samples, size_t n, double sr, double freq,
                            size_t *out_bins, tb_progress_fn pr, void *user) {
    if (freq <= 0) freq = 8000;
    int step = (int)round(sr * 0.001); if (step < 1) step = 1;
    int win = (int)round(sr * 0.0015); if (win < 24) win = 24;
    long bins = ((long)n - win) / step;
    if (bins < 0) bins = 0;
    float *env = (float *)calloc((size_t)bins + 1, sizeof(float));
    double *cosv = (double *)malloc((size_t)win * sizeof(double));
    double *sinv = (double *)malloc((size_t)win * sizeof(double));
    if (!env || !cosv || !sinv) { free(env); free(cosv); free(sinv); *out_bins = 0; return NULL; }
    double w = 2 * M_PI * freq / sr;
    for (int nn = 0; nn < win; nn++) { cosv[nn] = cos(w * nn); sinv[nn] = sin(w * nn); }
    size_t pos = 0;
    for (long i = 0; i < bins; i++, pos += (size_t)step) {
        double re = 0, im = 0;
        for (int nn = 0; nn < win; nn++) { double x = samples[pos + nn]; re += x * cosv[nn]; im += x * sinv[nn]; }
        env[i] = (float)(sqrt(re * re + im * im) / win);
        if ((i % 50000) == 0) {
            double pct = (double)i / (bins > 0 ? bins : 1);
            char buf[96];
            snprintf(buf, sizeof buf, "Scanning %d kHz envelope... %d%%", (int)round(freq/1000), (int)round(pct * 100));
            report(pr, user, fmin(70.0, pct * 70.0), buf);
        }
    }
    free(cosv); free(sinv);
    *out_bins = (size_t)bins;
    return env;
}

/* percentile over (subsampled) env, like the JS percentile(). */
static double percentile(const float *arr, size_t n, double pct) {
    const size_t maxN = 220000;
    size_t step = n / maxN; if (step < 1) step = 1;
    size_t cap = (n / step) + 1, cnt = 0;
    double *vals = (double *)malloc(cap * sizeof(double));
    if (!vals) return 0;
    for (size_t i = 0; i < n; i += step) { double v = arr[i]; if (isfinite(v)) vals[cnt++] = v; }
    if (!cnt) { free(vals); return 0; }
    qsort(vals, cnt, sizeof(double), cmp_double);
    long idx = (long)floor((pct / 100.0) * (double)(cnt - 1));
    if (idx < 0) idx = 0;
    if ((size_t)idx > cnt - 1) idx = (long)(cnt - 1);
    double r = vals[idx];
    free(vals);
    return r;
}

/* ---------------- answer / control classification ---------------- */
typedef struct { int answer; double confidence; bool candidate; } answer_meta;

static answer_meta decode_answer_from_bits(const char *bits, int nbits) {
    answer_meta m = {0, 0, false};
    if (nbits < 24) return m;
    int byte0 = bits_to_int(bits, 0, 8);
    int byte2 = bits_to_int(bits, 16, 8);
    int low = byte2 & 0x0F;
    if (!(low == 0x0F || low == 0x0E)) return m;
    int e0d = hamming_byte(byte0, 0x0D), e1d = hamming_byte(byte0, 0x1D);
    int opcodeErr = e0d < e1d ? e0d : e1d;
    if (opcodeErr > 1 || (opcodeErr == 1 && nbits < 32)) return m;
    double markerConfidence = (low == 0x0F) ? 1.0 : 0.85;
    m.answer = ((byte2 >> 6) & 3) + 1;
    m.confidence = opcodeErr == 0 ? markerConfidence : markerConfidence * 0.55;
    m.candidate = (opcodeErr == 1);
    return m;
}

static tb_kind classify_control(const char *bits, int nbits) {
    if (nbits < 6) return TB_KIND_NONE;
    if (strncmp(bits, "101010", 6) == 0) {
        if (nbits >= 32) {
            int byte3 = bits_to_int(bits, 24, 8);
            if ((byte3 & 0xF0) == 0xD0) return TB_KIND_INTRO;
        }
        return TB_KIND_PICKUP;
    }
    if (strncmp(bits, "101011", 6) == 0) return TB_KIND_SOUND;
    if (strncmp(bits, "101000", 6) == 0) return TB_KIND_INTRO;
    return TB_KIND_NONE;
}

/* run descriptor */
typedef struct { int v; long s; long e; } run_t;

static void build_packet(double t_s, const char *bits, int nbits, tb_parse_mode mode,
                         int leaderMark, int leaderGap, int markErr, int gapErr, int nb,
                         tb_packet *out) {
    memset(out, 0, sizeof *out);
    out->time = t_s;
    memcpy(out->bits, bits, (size_t)nbits);
    out->bits[nbits] = '\0';
    out->nbits = nbits;
    bits_to_bytes(bits, nbits, out->bytes, &out->nbytes);
    answer_meta meta = decode_answer_from_bits(bits, nbits);
    out->answer = meta.answer;
    out->answer_confidence = meta.confidence;
    out->answer_candidate = meta.candidate;
    tb_kind control = (meta.answer != 0) ? TB_KIND_NONE : classify_control(bits, nbits);
    double q = 1.0 - ((fabs((double)leaderMark - 20.0) / 12.0 + fabs((double)leaderGap - 10.0) / 8.0
                       + (double)(markErr + gapErr) / (nb > 0 ? nb : 1) / 4.0) / 4.0);
    out->quality = q < 0 ? 0 : q;
    out->kind = (meta.answer != 0 && !meta.candidate) ? TB_KIND_ANSWER : control;
    out->mode = mode;
}

/* Port of parseAtThreshold: appends packets to list. */
static void parse_at_threshold(const float *env, size_t bins, double sr, double threshold,
                               tb_packet_list *out) {
    /* build runs */
    size_t rcap = 1024, rcount = 0;
    run_t *runs = (run_t *)malloc(rcap * sizeof(run_t));
    if (!runs) return;
    int value = bins ? (env[0] > threshold) : 0;
    long start = 0;
    for (size_t i = 1; i < bins; i++) {
        int v = env[i] > threshold;
        if (v != value) {
            if (rcount == rcap) { rcap *= 2; run_t *nr = realloc(runs, rcap * sizeof(run_t)); if (!nr) { free(runs); return; } runs = nr; }
            runs[rcount++] = (run_t){value, start, (long)i};
            value = v; start = (long)i;
        }
    }
    if (rcount == rcap) { rcap += 1; run_t *nr = realloc(runs, rcap * sizeof(run_t)); if (!nr) { free(runs); return; } runs = nr; }
    runs[rcount++] = (run_t){value, start, (long)bins};

    /* msec-per-bin: each env bin is step samples = ~1ms (step=round(sr*0.001)). The JS
     * works in bin units directly and converts time as run.s/1000 because 1 bin ≈ 1 ms. */
    for (size_t i = 0; i + 3 < rcount; i++) {
        run_t r = runs[i], gap = runs[i + 1];
        if (!r.v || gap.v) continue;
        int leaderMark = (int)(r.e - r.s), leaderGap = (int)(gap.e - gap.s);
        if (!(leaderMark >= 14 && leaderMark <= 31 && leaderGap >= 5 && leaderGap <= 17)) continue;

        /* Parser A: classic mark+gap run lengths */
        {
            char bits[TB_MAX_BITS + 1]; int nbits = 0, markErr = 0, gapErr = 0, nb = 0;
            size_t j = i + 2;
            while (j + 1 < rcount && nbits < TB_MAX_BITS) {
                run_t m = runs[j], g = runs[j + 1];
                if (!m.v || g.v) break;
                int ml = (int)(m.e - m.s), gl = (int)(g.e - g.s);
                if (!(ml >= 1 && ml <= 6)) break;
                if (!(gl >= 1 && gl <= 10)) break;
                bits[nbits++] = (gl >= 3 ? '1' : '0');
                markErr += abs(ml - 2);
                int d2 = abs(gl - 2), d4 = abs(gl - 4); gapErr += (d2 < d4 ? d2 : d4);
                nb++; j += 2;
            }
            if (nbits >= 16) {
                bits[nbits] = '\0';
                tb_packet p; build_packet((double)r.s / 1000.0, bits, nbits, TB_MODE_GAP_RUN,
                                          leaderMark, leaderGap, markErr, gapErr, nb, &p);
                packet_list_push(out, &p);
            }
        }

        /* Parser B: data-mark start-to-start periods */
        {
            run_t marks[64]; int mcount = 0;
            for (size_t k = i + 2; k < rcount; k++) {
                run_t rr = runs[k];
                if (!rr.v) continue;
                int len = (int)(rr.e - rr.s);
                if (len >= 12) break;
                if (rr.s - r.s > 260) break;
                if (len < 1 || len > 7) continue;
                if (mcount > 0) {
                    run_t *last = &marks[mcount - 1];
                    if (rr.s - last->e <= 1 && rr.e - last->s <= 7) { last->e = rr.e; continue; }
                    if (rr.s - last->s > 10) break;
                }
                if (mcount < 64) marks[mcount++] = rr; else break;
                if (mcount >= 50) break;
            }
            if (mcount >= 17) {
                char pbits[TB_MAX_BITS + 1]; int pn = 0, pMarkErr = 0, pPeriodErr = 0, pcnt = 0;
                for (int k = 0; k < mcount - 1 && pn < TB_MAX_BITS; k++) {
                    int ml = (int)(marks[k].e - marks[k].s);
                    int dt = (int)(marks[k + 1].s - marks[k].s);
                    if (!(dt >= 3 && dt <= 9)) break;
                    pbits[pn++] = (dt >= 5 ? '1' : '0');
                    pMarkErr += abs(ml - 2);
                    int d4 = abs(dt - 4), d6 = abs(dt - 6); pPeriodErr += (d4 < d6 ? d4 : d6);
                    pcnt++;
                }
                if (pn >= 16) {
                    pbits[pn] = '\0';
                    tb_packet p; build_packet((double)r.s / 1000.0, pbits, pn, TB_MODE_START_PERIOD,
                                              leaderMark, leaderGap, pMarkErr, pPeriodErr, pcnt, &p);
                    packet_list_push(out, &p);
                }
            }
        }
    }
    (void)sr;
    free(runs);
}

/* ---------------- voting / dedupe ---------------- */
typedef struct { double t0, t1; size_t *idx; size_t n, cap; int answer; double confidence; } pulse_grp;

static double clampd(double n, double a, double b) { return n < a ? a : (n > b ? b : n); }

static int cmp_packet_time(const void *a, const void *b) {
    double ta = ((const tb_packet *)a)->time, tb = ((const tb_packet *)b)->time;
    return (ta < tb) ? -1 : (ta > tb) ? 1 : 0;
}

static void vote_pulse(const tb_packet_list *all, const size_t *idx, size_t n, int *out_ans, double *out_conf) {
    double scores[5] = {0,0,0,0,0};
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        const tb_packet *p = &all->data[idx[i]];
        if (p->answer < 1 || p->answer > 4) continue;
        any = true;
        double w = (p->answer_confidence > 0 ? p->answer_confidence : 0.5)
                 + (p->quality > 0 ? p->quality : 0)
                 + fmin(1.0, p->nbits / 40.0);
        scores[p->answer] += w;
    }
    if (!any) { *out_ans = 0; *out_conf = 0; return; }
    int best = 0; double bestScore = -1e18, total = 0;
    for (int a = 1; a <= 4; a++) { total += scores[a]; if (scores[a] > bestScore) { bestScore = scores[a]; best = a; } }
    *out_ans = best; *out_conf = total > 0 ? bestScore / total : 0;
}

static double packet_score(const tb_packet *p) {
    return (p->kind == TB_KIND_ANSWER ? 8 : (p->answer_candidate ? 4 : 0))
         + (p->answer_confidence) * 8
         + (p->kind != TB_KIND_NONE ? 2 : 0)
         + p->nbits / 36.0 + p->quality
         + (p->mode == TB_MODE_START_PERIOD ? 0.15 : 0);
}

/* ---------------- full channel decode ---------------- */
double tb_decode_channel(const float *samples, size_t n, double sr, double carrier,
                         tb_packet_list *out, tb_progress_fn pr, void *user) {
    if (carrier <= 0) carrier = tb_detect_carrier(samples, n, sr);
    {
        char buf[64]; snprintf(buf, sizeof buf, "Preparing %d kHz envelope...", (int)round(carrier/1000));
        report(pr, user, 0, buf);
    }
    size_t bins = 0;
    float *env = make_envelope(samples, n, sr, carrier, &bins, pr, user);
    if (!env || !bins) { free(env); return carrier; }

    report(pr, user, 72, "Choosing thresholds...");
    double base = percentile(env, bins, 50), high = percentile(env, bins, 99.9);
    if (!(high > base)) { free(env); return carrier; }

    static const double ratios[] = {0.22, 0.28, 0.34, 0.40, 0.46, 0.52, 0.60};
    tb_packet_list all; tb_packet_list_init(&all);
    for (int i = 0; i < 7; i++) {
        parse_at_threshold(env, bins, sr, base + ratios[i] * (high - base), &all);
        char buf[64]; snprintf(buf, sizeof buf, "Parsing pulse packets... threshold %d/7", i + 1);
        report(pr, user, 75 + (i + 1) / 7.0 * 22, buf);
    }
    free(env);

    qsort(all.data, all.count, sizeof(tb_packet), cmp_packet_time);

    /* Vote each answer pulse over ALL raw threshold reads before dedupe. */
    /* group indices of answered packets within 0.12s */
    size_t pg_cap = 64, pg_n = 0;
    pulse_grp *groups = (pulse_grp *)malloc(pg_cap * sizeof(pulse_grp));
    if (groups) {
        for (size_t i = 0; i < all.count; i++) {
            const tb_packet *p = &all.data[i];
            if (p->answer < 1 || p->answer > 4) continue;
            if (pg_n > 0 && p->time - groups[pg_n - 1].t1 < 0.12) {
                pulse_grp *g = &groups[pg_n - 1];
                if (g->n == g->cap) { g->cap *= 2; g->idx = realloc(g->idx, g->cap * sizeof(size_t)); }
                g->idx[g->n++] = i; g->t1 = p->time;
            } else {
                if (pg_n == pg_cap) { pg_cap *= 2; pulse_grp *ng = realloc(groups, pg_cap * sizeof(pulse_grp)); if (!ng) break; groups = ng; }
                pulse_grp *g = &groups[pg_n++];
                g->t0 = g->t1 = p->time; g->cap = 8; g->n = 0; g->idx = malloc(g->cap * sizeof(size_t));
                g->idx[g->n++] = i;
            }
        }
        for (size_t i = 0; i < pg_n; i++) vote_pulse(&all, groups[i].idx, groups[i].n, &groups[i].answer, &groups[i].confidence);
    }

    /* dedupe: collapse packets within 0.035s, keeping the higher score */
    for (size_t i = 0; i < all.count; i++) {
        const tb_packet *p = &all.data[i];
        if (out->count > 0) {
            tb_packet *last = &out->data[out->count - 1];
            if (fabs(p->time - last->time) < 0.035) {
                if (packet_score(p) > packet_score(last)) *last = *p;
                continue;
            }
        }
        packet_list_push(out, p);
    }

    /* filter: keep nbits>=20 || answer || candidate || kind */
    size_t w = 0;
    for (size_t i = 0; i < out->count; i++) {
        tb_packet *p = &out->data[i];
        if (p->nbits >= 20 || p->answer != 0 || p->answer_candidate || p->kind != TB_KIND_NONE)
            out->data[w++] = *p;
    }
    out->count = w;

    /* apply voted answers back onto surviving answer packets */
    if (groups) {
        for (size_t i = 0; i < out->count; i++) {
            tb_packet *p = &out->data[i];
            if (p->answer >= 1 && p->answer <= 4) {
                for (size_t g = 0; g < pg_n; g++) {
                    if (p->time >= groups[g].t0 - 0.06 && p->time <= groups[g].t1 + 0.06 && groups[g].answer) {
                        p->answer = groups[g].answer; p->answer_confidence = groups[g].confidence; break;
                    }
                }
            }
        }
        for (size_t i = 0; i < pg_n; i++) free(groups[i].idx);
        free(groups);
    }

    tb_packet_list_free(&all);
    (void)clampd;
    {
        char buf[64]; snprintf(buf, sizeof buf, "Decoded %zu packets.", out->count);
        report(pr, user, 99, buf);
    }
    return carrier;
}

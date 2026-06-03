/* tb_mania_music.c - Mania music: sibling file + Sailor Moon-style clip detection. */
#include "tb_mania_music.h"
#include "tb_media.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

bool tb_mania_music_from_file(const char *path, float **mono, size_t *frames, double *sr) {
    *mono = NULL; *frames = 0; *sr = 0;
    tb_audio_channels au; char err[128];
    if (!tb_media_decode_audio(path, &au, err, sizeof err, NULL, NULL)) return false;
    float *m = malloc(au.nsamples * sizeof(float));
    if (!m) { tb_audio_channels_free(&au); return false; }
    for (size_t i = 0; i < au.nsamples; i++) {
        double s = 0; for (int c = 0; c < au.nch; c++) s += au.chan[c][i];
        m[i] = (float)(s / (au.nch > 0 ? au.nch : 1));
    }
    *mono = m; *frames = au.nsamples; *sr = au.sample_rate;
    tb_audio_channels_free(&au);
    return true;
}

/* hamming-ish distance between two hex byte strings ("AC B2 55"): number of bytes
 * that differ (mismatched length counts as far). */
static int bytes_distance(const char *a, const char *b) {
    int da = 0;
    while (*a && *b) {
        char *ea, *eb;
        long va = strtol(a, &ea, 16), vb = strtol(b, &eb, 16);
        if (ea == a || eb == b) break;
        if (va != vb) da++;
        a = ea; b = eb;
        while (*a == ' ') a++;
        while (*b == ' ') b++;
    }
    if (*a || *b) da += 2;   /* different number of bytes */
    return da;
}

/* RMS energy track over [t0,t1] at hop seconds; smoothed. Returns malloc'd arrays. */
typedef struct { double *sm; double *t; int n; double hop; double start; } etrack;
static etrack build_energy(const float *s, size_t n, double sr, double t0, double t1) {
    etrack e = {0};
    double hop = 0.25, win = 0.28;
    if (t1 <= t0 + 0.5) return e;
    int cnt = (int)((t1 - win - t0) / hop); if (cnt < 1) return e;
    double *rms = malloc(cnt * sizeof(double)), *tt = malloc(cnt * sizeof(double));
    e.sm = malloc(cnt * sizeof(double));
    if (!rms || !tt || !e.sm) { free(rms); free(tt); free(e.sm); e.sm = NULL; return e; }
    int step = (int)(sr / 12000); if (step < 1) step = 1;
    for (int i = 0; i < cnt; i++) {
        double t = t0 + i * hop;
        size_t a = (size_t)(t * sr), b = (size_t)((t + win) * sr); if (b > n) b = n;
        double sum = 0; int k = 0;
        for (size_t j = a; j < b; j += step) { double x = s[j]; sum += x * x; k++; }
        rms[i] = sqrt(sum / (k > 0 ? k : 1)); tt[i] = t;
    }
    for (int i = 0; i < cnt; i++) {
        double pa = rms[i > 0 ? i - 1 : 0], pb = rms[i], pc = rms[i < cnt - 1 ? i + 1 : i];
        e.sm[i] = (pa + pb + pc) / 3;
    }
    free(rms); e.t = tt; e.n = cnt; e.hop = hop; e.start = t0;
    return e;
}
static double quantile(double *v, int n, double pct) {
    if (n <= 0) return 0;
    int i = (int)((n - 1) * pct); if (i < 0) i = 0; if (i > n - 1) i = n - 1;
    return v[i];
}

/* Port of findSailorMoonManiaMusicAnchor: locate the sound packet just before the long
 * Mania song. The anchor is identified STRUCTURALLY (a sound followed by an intro
 * 150-360s later with no answers in between, preceded by lots of Q&A), NOT by exact
 * bytes - the control-tone bytes vary per rip. The DB anchor_bytes, if present, only
 * adds a soft bonus when it happens to match; it is never required. */
static bool find_anchor(const tb_event_list *ev, const char *anchor_bytes,
                        double *anchorStart, double *nextIntro) {
    double bestScore = -1e18; bool found = false;
    *anchorStart = -1; *nextIntro = -1;
    for (size_t i = 0; i < ev->count; i++) {
        const tb_event *e = &ev->data[i];
        if (e->type != TB_EV_SOUND) continue;
        int dist = (anchor_bytes && anchor_bytes[0]) ? bytes_distance(e->bytes, anchor_bytes) : 99;

        double intro = -1; size_t introIdx = 0; bool haveIntro = false;
        for (size_t j = i + 1; j < ev->count; j++)
            if (ev->data[j].type == TB_EV_INTRO) { intro = ev->data[j].start; introIdx = j; haveIntro = true; break; }
        int answersUntilIntro = 0;
        if (haveIntro) for (size_t j = i + 1; j < introIdx; j++)
            if (ev->data[j].type == TB_EV_ANSWER) answersUntilIntro++;
        int answersBefore = 0, pickupsBefore = 0;
        for (size_t j = 0; j < i; j++) {
            if (ev->data[j].type == TB_EV_ANSWER) answersBefore++;
            else if (ev->data[j].type == TB_EV_PICKUP) pickupsBefore++;
        }
        double introGap = haveIntro ? intro - e->start : 0;

        double score = 0;
        if (dist <= 1) score += dist == 0 ? 40 : 18;          /* soft byte bonus, not required */
        score += fmin(18, answersBefore * 3);
        score += fmin(12, pickupsBefore * 3);
        if (haveIntro && introGap >= 150 && introGap <= 360) score += 24;
        if (haveIntro && answersUntilIntro == 0) score += 12;
        if (e->start > 120) score += 4;

        if (score > bestScore) { bestScore = score; *anchorStart = e->start; *nextIntro = intro; found = true; }
    }
    return found && bestScore >= 55;   /* the 150-360s no-answer gap is what carries it past 55 */
}

bool tb_mania_music_detect_clip(const float *samples, size_t n, double sr,
                                const tb_event_list *events, const char *anchor_bytes,
                                float **mono, size_t *frames, double *out_sr) {
    *mono = NULL; *frames = 0; *out_sr = 0;
    if (!events) return false;

    double anchorStart = -1, nextIntro = -1;
    if (!find_anchor(events, anchor_bytes, &anchorStart, &nextIntro)) return false;
    if (anchorStart < 0) return false;

    double audioDur = (double)n / sr;
    double boundEnd = nextIntro >= 0 ? (nextIntro - 4 < audioDur ? nextIntro - 4 : audioDur)
                                     : (anchorStart + 285 < audioDur ? anchorStart + 285 : audioDur);
    double scanStart = anchorStart + 20; if (scanStart > boundEnd - 12) scanStart = boundEnd - 12;
    if (!(boundEnd > scanStart + 80)) return false;

    etrack tr = build_energy(samples, n, sr, scanStart, boundEnd);
    if (!tr.sm || tr.n < 80) { free(tr.sm); free(tr.t); return false; }

    double *vals = malloc(tr.n * sizeof(double));
    for (int i = 0; i < tr.n; i++) vals[i] = tr.sm[i];
    for (int i = 1; i < tr.n; i++) { double k = vals[i]; int j = i; while (j && vals[j-1] > k) { vals[j] = vals[j-1]; j--; } vals[j] = k; }
    double p05 = quantile(vals, tr.n, 0.05), p10 = quantile(vals, tr.n, 0.10), p25 = quantile(vals, tr.n, 0.25),
           p40 = quantile(vals, tr.n, 0.40), p60 = quantile(vals, tr.n, 0.60);
    free(vals);
    double quiet = fmax(0.00025, fmin(fmin(p25 * 0.70, p10 * 1.50), p05 * 2.00));
    double active = fmax(fmax(p40, quiet * 1.70), p60 * 0.78);
    int minQuiet = (int)(0.70 / tr.hop); if (minQuiet < 3) minQuiet = 3;

    /* scan for the quiet run whose following audible segment is the longest = the song */
    int startLo = (int)((anchorStart + 30 - tr.start) / tr.hop); if (startLo < 0) startLo = 0;
    int startHi = (int)((boundEnd - 35 - tr.start) / tr.hop); if (startHi > tr.n - 1) startHi = tr.n - 1;
    double bestT = -1, bestScore = -1e18;
    int run = 0;
    for (int i = startLo; i <= startHi; i++) {
        if (tr.sm[i] <= quiet) { run++; continue; }
        if (run >= minQuiet) {
            int cand = i;                         /* first audible frame after the silence */
            double t = tr.t[cand];
            /* measure how long it stays audible */
            int audible = 0; for (int k = cand; k < tr.n && tr.sm[k] >= active * 0.8; k++) audible++;
            double audibleDur = audible * tr.hop;
            double score = fmin(160, audibleDur) * 1.35 + fmin(24, run * 1.45);
            if (t >= anchorStart + 34 && t <= boundEnd - 45 && score > bestScore) { bestScore = score; bestT = t; }
        }
        run = 0;
    }
    free(tr.t); free(tr.sm);

    double clipStart = bestT >= 0 ? bestT : anchorStart + 74.6;
    double clipEnd = clipStart + 122;
    if (clipEnd > boundEnd - 2) clipEnd = boundEnd - 2;
    if (clipStart < anchorStart + 15) clipStart = anchorStart + 15;
    if (clipEnd < clipStart + 20) return false;
    if (getenv("TB_MM_DEBUG")) fprintf(stderr, "[mania] anchor=%.1f intro=%.1f clip=%.1f..%.1f\n",
                                       anchorStart, nextIntro, clipStart, clipEnd);

    /* slice mono PCM from the analysis channel */
    size_t a = (size_t)(clipStart * sr), b = (size_t)(clipEnd * sr);
    if (b > n) b = n; if (a >= b) return false;
    size_t fr = b - a;
    float *m = malloc(fr * sizeof(float));
    if (!m) return false;
    memcpy(m, samples + a, fr * sizeof(float));
    *mono = m; *frames = fr; *out_sr = sr;
    return true;
}

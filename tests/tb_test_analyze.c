/* tb_test_analyze.c - full native load pipeline: ffmpeg -> decode -> events -> subs. */
#include "../src/core/tb_media.h"
#include "../src/core/tb_decode.h"
#include "../src/core/tb_events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s media [pref:auto|left|right]\n", argv[0]); return 2; }
    const char *pref = argc > 2 ? argv[2] : "auto";

    double dur = 0; int w = 0, h = 0; char err[256];
    if (!tb_media_probe(argv[1], &dur, &w, &h, err, sizeof err)) { printf("probe: %s\n", err); return 1; }
    printf("probe: %.1fs  %dx%d\n", dur, w, h);

    tb_audio_channels au;
    if (!tb_media_decode_audio(argv[1], &au, err, sizeof err, NULL, NULL)) { printf("audio: %s\n", err); return 1; }
    printf("audio: %d ch, %.0f Hz, %.1f s\n", au.nch, au.sample_rate, au.nsamples / au.sample_rate);

    /* analyzeChannels: decode each channel, pick the one with most answers */
    int order[8], no = 0;
    if (strcmp(pref, "left") == 0) order[no++] = 0;
    else if (strcmp(pref, "right") == 0 && au.nch > 1) order[no++] = 1;
    else for (int c = 0; c < au.nch && c < 8; c++) order[no++] = c;

    int best = -1, best_ans = -1; double best_car = 8000; tb_packet_list best_pkts; tb_packet_list_init(&best_pkts);
    for (int i = 0; i < no; i++) {
        int c = order[i];
        tb_packet_list pk; tb_packet_list_init(&pk);
        double car = tb_decode_channel(au.chan[c], au.nsamples, au.sample_rate, 0, &pk, NULL, NULL);
        int ans = 0; for (size_t k = 0; k < pk.count; k++) if (pk.data[k].answer >= 1 && pk.data[k].answer <= 4) ans++;
        printf("  channel %d: %d answers, %zu packets, %.0f Hz\n", c, ans, pk.count, car);
        if (ans > best_ans) { best_ans = ans; best = c; best_car = car; tb_packet_list_free(&best_pkts); best_pkts = pk; }
        else tb_packet_list_free(&pk);
    }

    tb_event_list ev; tb_event_list_init(&ev);
    tb_build_events(&best_pkts, best_car, &ev);
    int nQ = 0, nR = 0; for (size_t i = 0; i < ev.count; i++) { if (ev.data[i].type == TB_EV_ANSWER) nQ++; else if (ev.data[i].type == TB_EV_PICKUP) nR++; }
    printf("chosen channel %d @ %.0f Hz: %d questions, %d calls\n", best, best_car, nQ, nR);

    tb_cue_list subs; tb_cue_list_init(&subs);
    if (tb_media_extract_subtitles(argv[1], &subs)) printf("embedded subtitles: %zu cues (%s)\n", subs.count, subs.label);
    else printf("embedded subtitles: none\n");

    tb_cue_list_free(&subs); tb_event_list_free(&ev); tb_packet_list_free(&best_pkts); tb_audio_channels_free(&au);
    return 0;
}

/* tb_test_decode.c - decode raw f32le mono PCM and print the event timeline.
 * Usage: tb_test_decode <file.f32> <sampleRate> [carrierHz]
 */
#include "../src/core/tb_decode.h"
#include "../src/core/tb_events.h"
#include <stdio.h>
#include <stdlib.h>

static const char *ev_name(tb_event_type t) {
    switch (t) { case TB_EV_PICKUP: return "pickup"; case TB_EV_SOUND: return "sound";
                 case TB_EV_INTRO: return "intro"; case TB_EV_ANSWER: return "answer"; }
    return "?";
}
static void fmt_time(double t, char *b) { int m = (int)(t/60); double s = t - m*60; sprintf(b, "%02d:%04.1f", m, s); }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s file.f32 sampleRate [carrier]\n", argv[0]); return 2; }
    double sr = atof(argv[2]);
    double carrier = argc > 3 ? atof(argv[3]) : 0;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long bytes = ftell(f); fseek(f, 0, SEEK_SET);
    size_t n = (size_t)(bytes / 4);
    float *s = malloc(n * sizeof(float));
    if (fread(s, 4, n, f) != n) { fprintf(stderr, "short read\n"); }
    fclose(f);

    tb_packet_list pkts; tb_packet_list_init(&pkts);
    double car = tb_decode_channel(s, n, sr, carrier, &pkts, NULL, NULL);
    free(s);

    tb_event_list ev; tb_event_list_init(&ev);
    tb_build_events(&pkts, car, &ev);

    int nQ = 0, nR = 0;
    for (size_t i = 0; i < ev.count; i++) { if (ev.data[i].type == TB_EV_ANSWER) nQ++; else if (ev.data[i].type == TB_EV_PICKUP) nR++; }
    printf("carrier=%.0f Hz  packets=%zu  events=%zu  (%d questions, %d calls)\n", car, pkts.count, ev.count, nQ, nR);
    for (size_t i = 0; i < ev.count; i++) {
        tb_event *e = &ev.data[i];
        char ts[16], te[16]; fmt_time(e->start, ts); fmt_time(e->end, te);
        printf("%02zu  %-7s %s -> %s", i+1, ev_name(e->type), ts, te);
        if (e->type == TB_EV_ANSWER) printf("  ans=%d", e->answer);
        if (e->delayed_cue) printf("  [delayed cue gap=%.2fs]", e->cue_gap);
        if (e->bytes[0]) printf("  bytes=%s", e->bytes);
        printf("\n");
    }
    tb_event_list_free(&ev); tb_packet_list_free(&pkts);
    return 0;
}

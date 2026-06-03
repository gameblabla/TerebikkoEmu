/* tb_test_player.c - headless smoke test: open, play, seek, read frames. */
#include "../src/core/tb_player.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s media\n", argv[0]); return 2; }
    char err[256];
    tb_hwaccel hw = argc > 2 ? (tb_hwaccel)atoi(argv[2]) : TB_HW_NONE;
    tb_player *p = tb_player_open(argv[1], hw, err, sizeof err);
    printf("decoder: %s\n", tb_player_active_hwaccel(p ? p : NULL));
    if (!p) { printf("open failed: %s\n", err); return 1; }
    int w, h; tb_player_dimensions(p, &w, &h);
    printf("opened: %dx%d dur=%.1fs\n", w, h, tb_player_duration(p));

    tb_player_play(p);
    int frames = 0;
    for (int i = 0; i < 60; i++) {       /* ~1s of playback */
        SDL_Delay(16);
        tb_frame f = tb_player_acquire_frame(p);
        if (f.valid) frames++;
    }
    double t1 = tb_player_time(p);
    printf("after ~1s: time=%.2f, frames seen=%d\n", t1, frames);

    tb_player_seek(p, 300.0);
    SDL_Delay(300);
    tb_frame f = tb_player_acquire_frame(p);
    double t2 = tb_player_time(p);
    printf("after seek to 300: time=%.2f, frame valid=%d (%dx%d)\n", t2, f.valid, f.width, f.height);

    int ok = (t1 > 0.3 && frames > 5 && t2 >= 299 && t2 <= 320);
    printf("PLAYER %s\n", ok ? "PASS" : "CHECK");
    tb_player_close(p);
    SDL_Quit();
    return 0;
}

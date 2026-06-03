/* tb_input.c - SDL3 gamepad polling + remappable binding table. */
#include "tb_input.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct tb_input {
    tb_binding bind[TB_ACT_COUNT];
    SDL_Gamepad *pad;
    SDL_JoystickID pad_id;
    bool capturing;
    int  captured;      /* -1 = none */
};

static const char *NAMES[TB_ACT_COUNT] = { "ans1", "ans2", "ans3", "ans4", "pickup", "pause", "fullscreen" };
const char *tb_action_name(tb_action a) { return (a >= 0 && a < TB_ACT_COUNT) ? NAMES[a] : "?"; }

/* sensible default gamepad mapping: face buttons -> 1..4, start -> pickup, etc. */
static void apply_default_pad(tb_input *in) {
    in->bind[TB_ACT_ANS1].pad = SDL_GAMEPAD_BUTTON_SOUTH;
    in->bind[TB_ACT_ANS2].pad = SDL_GAMEPAD_BUTTON_EAST;
    in->bind[TB_ACT_ANS3].pad = SDL_GAMEPAD_BUTTON_WEST;
    in->bind[TB_ACT_ANS4].pad = SDL_GAMEPAD_BUTTON_NORTH;
    in->bind[TB_ACT_PICKUP].pad = SDL_GAMEPAD_BUTTON_START;
    in->bind[TB_ACT_PAUSE].pad = SDL_GAMEPAD_BUTTON_BACK;
    in->bind[TB_ACT_FULLSCREEN].pad = SDL_GAMEPAD_BUTTON_GUIDE;
}

static void open_first_pad(tb_input *in) {
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (ids && count > 0) { in->pad = SDL_OpenGamepad(ids[0]); in->pad_id = ids[0]; }
    if (ids) SDL_free(ids);
}

void tb_input_begin_capture(tb_input *in) { in->capturing = true; in->captured = -1; }
void tb_input_cancel_capture(tb_input *in) { in->capturing = false; in->captured = -1; }
int  tb_input_take_captured(tb_input *in) { int c = in->captured; in->captured = -1; return c; }

const char *tb_input_pad_button_name(int b) {
    if (b < 0) return "—";
    const char *n = SDL_GetGamepadStringForButton((SDL_GamepadButton)b);
    return n ? n : "?";
}

tb_input *tb_input_create(void) {
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    tb_input *in = calloc(1, sizeof *in);
    in->captured = -1;
    for (int i = 0; i < TB_ACT_COUNT; i++) { in->bind[i].key = 0; in->bind[i].pad = -1; }
    apply_default_pad(in);
    open_first_pad(in);
    return in;
}

void tb_input_destroy(tb_input *in) {
    if (!in) return;
    if (in->pad) SDL_CloseGamepad(in->pad);
    free(in);
}

bool tb_input_has_gamepad(const tb_input *in) { return in->pad != NULL; }

void tb_input_poll(tb_input *in, tb_input_action_cb cb, void *user) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!in->pad) open_first_pad(in);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (in->pad && e.gdevice.which == in->pad_id) { SDL_CloseGamepad(in->pad); in->pad = NULL; open_first_pad(in); }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (in->capturing) { in->captured = e.gbutton.button; in->capturing = false; break; }
                for (int a = 0; a < TB_ACT_COUNT; a++)
                    if (in->bind[a].pad == e.gbutton.button) { if (cb) cb((tb_action)a, user); }
                break;
            default: break;
        }
    }
}

void tb_input_set_key(tb_input *in, tb_action a, int keycode) { if (a >= 0 && a < TB_ACT_COUNT) in->bind[a].key = keycode; }
void tb_input_set_pad(tb_input *in, tb_action a, int padbutton) { if (a >= 0 && a < TB_ACT_COUNT) in->bind[a].pad = padbutton; }
tb_binding tb_input_get(const tb_input *in, tb_action a) { return (a >= 0 && a < TB_ACT_COUNT) ? in->bind[a] : (tb_binding){0, -1}; }

tb_action tb_input_action_for_key(const tb_input *in, int keycode) {
    for (int a = 0; a < TB_ACT_COUNT; a++) if (in->bind[a].key == keycode) return (tb_action)a;
    return TB_ACT_NONE;
}

void tb_input_reset_defaults(tb_input *in, const int default_keys[TB_ACT_COUNT]) {
    for (int a = 0; a < TB_ACT_COUNT; a++) in->bind[a].key = default_keys[a];
    apply_default_pad(in);
}

bool tb_input_save(const tb_input *in, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    for (int a = 0; a < TB_ACT_COUNT; a++)
        fprintf(f, "%s=%d,%d\n", NAMES[a], in->bind[a].key, in->bind[a].pad);
    fclose(f);
    return true;
}

bool tb_input_load(tb_input *in, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = '\0';
        int key = 0, pad = -1;
        sscanf(eq + 1, "%d,%d", &key, &pad);
        for (int a = 0; a < TB_ACT_COUNT; a++)
            if (strcmp(line, NAMES[a]) == 0) { in->bind[a].key = key; in->bind[a].pad = pad; }
    }
    fclose(f);
    return true;
}

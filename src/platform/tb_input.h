/*
 * tb_input.h - remappable controls (SDL3 gamepad + a keyboard binding table).
 *
 * Single source of truth for control bindings and their persistence. The Qt
 * frontend feeds keyboard key codes in (its own Qt::Key convention) and asks for
 * the matching action; SDL3 gamepads are polled here directly.
 */
#ifndef TB_INPUT_H
#define TB_INPUT_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TB_ACT_NONE = -1,
    TB_ACT_ANS1 = 0, TB_ACT_ANS2, TB_ACT_ANS3, TB_ACT_ANS4,
    TB_ACT_PICKUP, TB_ACT_PAUSE, TB_ACT_FULLSCREEN,
    TB_ACT_COUNT
} tb_action;

typedef struct tb_input tb_input;

/* keycode is an opaque int chosen by the frontend (e.g. Qt::Key). gamepad button
 * is an SDL_GamepadButton value, or -1 for unbound. */
typedef struct { int key; int pad; } tb_binding;

tb_input *tb_input_create(void);     /* opens SDL gamepad subsystem + first pad */
void      tb_input_destroy(tb_input *in);

/* Poll gamepad events; calls cb(action, user) on each press. Call each frame. */
typedef void (*tb_input_action_cb)(tb_action a, void *user);
void tb_input_poll(tb_input *in, tb_input_action_cb cb, void *user);

/* Bindings. */
const char *tb_action_name(tb_action a);
void  tb_input_set_key(tb_input *in, tb_action a, int keycode);
void  tb_input_set_pad(tb_input *in, tb_action a, int padbutton);
tb_binding tb_input_get(const tb_input *in, tb_action a);
tb_action  tb_input_action_for_key(const tb_input *in, int keycode);
void  tb_input_reset_defaults(tb_input *in, const int default_keys[TB_ACT_COUNT]);

/* Gamepad-button capture for remapping: begin_capture arms it; the next gamepad
 * button press during tb_input_poll is captured (not dispatched). take_captured
 * returns that button once (or -1), clearing it. */
void tb_input_begin_capture(tb_input *in);
void tb_input_cancel_capture(tb_input *in);
int  tb_input_take_captured(tb_input *in);
const char *tb_input_pad_button_name(int padbutton);

bool tb_input_save(const tb_input *in, const char *path);
bool tb_input_load(tb_input *in, const char *path);

bool tb_input_has_gamepad(const tb_input *in);

#ifdef __cplusplus
}
#endif
#endif /* TB_INPUT_H */

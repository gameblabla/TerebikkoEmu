/*
 * tb_events.h - build the play timeline from decoded packets (pure C11).
 * Faithful port of buildEvents / playEvents / applyDelayedCueDatabase.
 */
#ifndef TB_EVENTS_H
#define TB_EVENTS_H

#include "tb_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TB_EV_PICKUP = 0,
    TB_EV_SOUND,
    TB_EV_INTRO,
    TB_EV_ANSWER
} tb_event_type;

typedef struct {
    tb_event_type type;
    double  start;
    double  end;
    int     answer;          /* answer events only, 1..4 */
    char    bytes[32];       /* hex string, for debug/cue matching */
    double  checkpoint;      /* resume point */
    bool    delayed_cue;
    double  early_guard_start;
    double  cue_gap;
} tb_event;

typedef struct {
    tb_event *data;
    size_t    count;
    size_t    cap;
} tb_event_list;

void tb_event_list_init(tb_event_list *l);
void tb_event_list_free(tb_event_list *l);

/* Build the full event timeline from decoded packets. `carrier` used by the
 * delayed-cue database. */
void tb_build_events(const tb_packet_list *packets, double carrier, tb_event_list *out);

/* True if the event is interactive (pickup or answer). */
bool tb_event_is_play(const tb_event *e);

#ifdef __cplusplus
}
#endif
#endif /* TB_EVENTS_H */

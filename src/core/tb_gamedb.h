/*
 * tb_gamedb.h - known-game database (pure C11).
 *
 * The data table (tb_gamedb_data.c) is generated at build time from gamedb/*.txt by
 * tools/gen_gamedb. It lets the runtime identify a loaded video by matching its
 * decoded answer-pulse bytes, apply game-specific "delayed cue" timing (e.g. the
 * See 'N Say Treasure Hunt countdown) without hardcoding it, and find the Mania
 * music anchor (e.g. the Sailor Moon song lead-in sound packet).
 */
#ifndef TB_GAMEDB_H
#define TB_GAMEDB_H

#include "tb_events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *bytes;     /* answer-pulse hex of the cued question      */
    int    minute;         /* expected minute of the event               */
    int    sec_lo, sec_hi; /* expected second window                     */
    double gap_lo, gap_hi; /* gap (s) from the preceding sound packet     */
    int    prev_is_sound;  /* require the previous event to be 'sound'    */
} tb_gamedb_cue;

typedef struct {
    const char *name;
    const char *mania_anchor;      /* answer/sound byte anchoring Mania music, or "" */
    const int  *answer_vals;       /* answer values (1-4) in order; the fingerprint  */
    int n_answers;
    const tb_gamedb_cue *cues;
    int n_cues;
} tb_gamedb_game;

extern const tb_gamedb_game tb_gamedb_games[];
extern const int tb_gamedb_count;

/* Identify the game from a decoded event timeline; returns index or -1. */
int tb_gamedb_detect(const tb_event_list *events);
const char *tb_gamedb_name(int idx);                 /* game name or NULL              */
const char *tb_gamedb_mania_anchor(int idx);         /* anchor bytes, "" if none, or NULL */

/* Apply the detected game's delayed cues onto the timeline (marks early-guard cues). */
void tb_gamedb_apply_cues(int idx, tb_event_list *events, double carrier);

#ifdef __cplusplus
}
#endif
#endif /* TB_GAMEDB_H */

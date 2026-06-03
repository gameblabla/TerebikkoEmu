/*
 * tb_subs.h - subtitle cue model + sidecar SRT/VTT/ASS parsing (pure C11).
 *
 * Direct .srt/.vtt/.ass/.ssa files are parsed here. Embedded MKV text subtitles
 * are decoded by the ffmpeg media layer (tb_media) which appends cues into the
 * same tb_cue_list, so the rest of the app is source-agnostic.
 */
#ifndef TB_SUBS_H
#define TB_SUBS_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double start;
    double end;
    char  *text;     /* owned, UTF-8, '\n' line breaks */
} tb_cue;

typedef struct {
    tb_cue *data;
    size_t  count;
    size_t  cap;
    char    label[160];
} tb_cue_list;

void tb_cue_list_init(tb_cue_list *l);
void tb_cue_list_free(tb_cue_list *l);
void tb_cue_list_push(tb_cue_list *l, double start, double end, const char *text);
void tb_cue_list_sort(tb_cue_list *l);

/* Strip ASS overrides / tags / entities, normalise newlines (cleanSubtitleText). */
char *tb_clean_subtitle_text(const char *raw);   /* malloc'd, caller frees */

/* Parse a time stamp like "00:01:02,500" / "1:02.5". Returns seconds or NAN. */
double tb_parse_subtitle_time(const char *s);

/* Parse a whole subtitle file's text. `filename` selects ASS vs SRT/VTT by ext. */
bool tb_parse_subtitles(const char *text, const char *filename, tb_cue_list *out);

/* Load + parse a sidecar subtitle file from disk. */
bool tb_load_sidecar_subtitles(const char *path, tb_cue_list *out);

/* Given a media path, find a sibling subtitle file (same stem). Writes path into
 * out_path (size outsz). Returns true if found. */
bool tb_find_sidecar_subtitle(const char *media_path, char *out_path, size_t outsz);

/* Given a media path, find sibling cover art (.png/.jpg/.jpeg). */
bool tb_find_cover_art(const char *media_path, char *out_path, size_t outsz);

/* True if the path looks like a subtitle file by extension. */
bool tb_is_subtitle_path(const char *path);

#ifdef __cplusplus
}
#endif
#endif /* TB_SUBS_H */

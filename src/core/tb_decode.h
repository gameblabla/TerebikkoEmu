/*
 * tb_decode.h - Terebikko / See 'N Say control-tone decoder (pure C11).
 *
 * This is a faithful port of the DECODER_WORKER / analyzeChannels logic from the
 * HTML/JS emulator. It takes mono float PCM for one channel and recovers the hidden
 * 8 kHz (or ~7 kHz) pulse packets: answer pulses (button 1-4) and control pulses
 * (pick-up ring / talk / intro).
 *
 * No Qt, no C++, no I/O. Frontends talk to this through tb_core.h opaque handles.
 */
#ifndef TB_DECODE_H
#define TB_DECODE_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Packet kind, mirroring the JS `kind` field. */
typedef enum {
    TB_KIND_NONE = 0,
    TB_KIND_ANSWER,
    TB_KIND_PICKUP,
    TB_KIND_SOUND,
    TB_KIND_INTRO
} tb_kind;

/* Which parser produced the packet (JS `mode`). */
typedef enum {
    TB_MODE_GAP_RUN = 0,
    TB_MODE_START_PERIOD
} tb_parse_mode;

#define TB_MAX_BITS  48
#define TB_MAX_BYTES 8

/* A decoded pulse packet (JS object returned by buildPacket). */
typedef struct {
    double        time;             /* seconds */
    char          bits[TB_MAX_BITS + 1]; /* '0'/'1' chars, NUL terminated */
    int           nbits;
    unsigned char bytes[TB_MAX_BYTES];
    int           nbytes;
    int           answer;           /* 0 = none, else 1..4 */
    double        answer_confidence;
    bool          answer_candidate;
    tb_kind       kind;
    double        quality;
    tb_parse_mode mode;
} tb_packet;

/* Growable packet list. */
typedef struct {
    tb_packet *data;
    size_t     count;
    size_t     cap;
} tb_packet_list;

void tb_packet_list_init(tb_packet_list *l);
void tb_packet_list_free(tb_packet_list *l);

/* Progress callback: value 0..100, text may be NULL. */
typedef void (*tb_progress_fn)(double value, const char *text, void *user);

/*
 * Detect the most likely carrier frequency by sweeping candidates and picking the
 * one whose envelope is the most "bursty" (peak/median). Returns Hz.
 */
double tb_detect_carrier(const float *samples, size_t n, double sample_rate);

/*
 * Full decode of one channel. Appends packets to `out` (already initialized).
 * If carrier <= 0 it is auto-detected. Returns the carrier used.
 */
double tb_decode_channel(const float *samples, size_t n, double sample_rate,
                         double carrier, tb_packet_list *out,
                         tb_progress_fn progress, void *user);

#ifdef __cplusplus
}
#endif
#endif /* TB_DECODE_H */

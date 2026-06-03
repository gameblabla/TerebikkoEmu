/* tb_media.c - ffmpeg media analysis (audio decode, probe, subtitle extraction). */
#include "tb_media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

static void seterr(char *b, size_t n, const char *m) { if (b && n) { strncpy(b, m, n - 1); b[n - 1] = '\0'; } }

void tb_audio_channels_free(tb_audio_channels *a) {
    if (!a) return;
    if (a->chan) { for (int i = 0; i < a->nch; i++) free(a->chan[i]); free(a->chan); }
    a->chan = NULL; a->nch = 0; a->nsamples = 0;
}

/* growable per-channel float buffers */
typedef struct { float **chan; int nch; size_t cap, len; } chanbuf;
static bool chanbuf_ensure(chanbuf *c, int nch, size_t add) {
    if (c->nch == 0) {
        c->nch = nch; c->chan = calloc((size_t)nch, sizeof(float *)); c->cap = 1 << 20; c->len = 0;
        for (int i = 0; i < nch; i++) { c->chan[i] = malloc(c->cap * sizeof(float)); if (!c->chan[i]) return false; }
    }
    if (c->len + add > c->cap) {
        size_t nc = c->cap; while (c->len + add > nc) nc *= 2;
        for (int i = 0; i < c->nch; i++) { float *p = realloc(c->chan[i], nc * sizeof(float)); if (!p) return false; c->chan[i] = p; }
        c->cap = nc;
    }
    return true;
}

bool tb_media_decode_audio(const char *path, tb_audio_channels *out,
                           char *errbuf, size_t errsz,
                           tb_media_progress progress, void *user) {
    memset(out, 0, sizeof *out);
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) { seterr(errbuf, errsz, "Could not open the media file."); return false; }
    if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); seterr(errbuf, errsz, "Could not read stream info."); return false; }

    int astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (astream < 0) { avformat_close_input(&fmt); seterr(errbuf, errsz, "No decodable audio track found."); return false; }

    AVStream *st = fmt->streams[astream];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) { avformat_close_input(&fmt); seterr(errbuf, errsz, "No decoder for the audio codec."); return false; }
    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    if (avcodec_open2(ctx, dec, NULL) < 0) { avcodec_free_context(&ctx); avformat_close_input(&fmt); seterr(errbuf, errsz, "Could not open the audio decoder."); return false; }

    int nch = ctx->ch_layout.nb_channels;
    if (nch < 1) nch = 1;
    int sr = ctx->sample_rate > 0 ? ctx->sample_rate : 44100;

    /* resampler -> planar float, native rate & layout */
    SwrContext *swr = NULL;
    AVChannelLayout outlay; memset(&outlay, 0, sizeof outlay);  /* copy uninits dst first */
    av_channel_layout_copy(&outlay, &ctx->ch_layout);
    if (outlay.nb_channels < 1) av_channel_layout_default(&outlay, nch);
    swr_alloc_set_opts2(&swr, &outlay, AV_SAMPLE_FMT_FLTP, sr,
                        &ctx->ch_layout, ctx->sample_fmt, sr, 0, NULL);
    if (!swr || swr_init(swr) < 0) { if (swr) swr_free(&swr); avcodec_free_context(&ctx); avformat_close_input(&fmt); seterr(errbuf, errsz, "Could not init the audio resampler."); return false; }

    double dur = (fmt->duration > 0) ? fmt->duration / (double)AV_TIME_BASE : 0;

    chanbuf cb = {0};
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();
    bool ok = true;
    uint8_t **obuf = NULL; int oline = 0;
    int max_out = 0;

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == astream) {
            if (avcodec_send_packet(ctx, pkt) >= 0) {
                while (avcodec_receive_frame(ctx, frm) >= 0) {
                    int need = swr_get_out_samples(swr, frm->nb_samples);
                    if (need > max_out) {
                        if (obuf) av_freep(&obuf[0]), av_freep(&obuf);
                        av_samples_alloc_array_and_samples(&obuf, &oline, nch, need, AV_SAMPLE_FMT_FLTP, 0);
                        max_out = need;
                    }
                    int got = swr_convert(swr, obuf, need, (const uint8_t **)frm->extended_data, frm->nb_samples);
                    if (got > 0) {
                        if (!chanbuf_ensure(&cb, nch, (size_t)got)) { ok = false; break; }
                        for (int ch = 0; ch < nch; ch++)
                            memcpy(cb.chan[ch] + cb.len, obuf[ch], (size_t)got * sizeof(float));
                        cb.len += (size_t)got;
                    }
                }
            }
        }
        av_packet_unref(pkt);
        if (progress && dur > 0 && (cb.len % (1 << 19)) < 4096) {
            double t = (double)cb.len / sr;
            progress(22 + 33.0 * (t / dur > 1 ? 1 : t / dur), "Decoding audio...", user);
        }
        if (!ok) break;
    }
    /* flush */
    avcodec_send_packet(ctx, NULL);
    while (ok && avcodec_receive_frame(ctx, frm) >= 0) {
        int need = swr_get_out_samples(swr, frm->nb_samples);
        if (need > 0) {
            if (need > max_out) { if (obuf) { av_freep(&obuf[0]); av_freep(&obuf); } av_samples_alloc_array_and_samples(&obuf, &oline, nch, need, AV_SAMPLE_FMT_FLTP, 0); max_out = need; }
            int got = swr_convert(swr, obuf, need, (const uint8_t **)frm->extended_data, frm->nb_samples);
            if (got > 0 && chanbuf_ensure(&cb, nch, (size_t)got)) {
                for (int ch = 0; ch < nch; ch++) memcpy(cb.chan[ch] + cb.len, obuf[ch], (size_t)got * sizeof(float));
                cb.len += (size_t)got;
            }
        }
    }

    if (obuf) { av_freep(&obuf[0]); av_freep(&obuf); }
    av_frame_free(&frm); av_packet_free(&pkt);
    av_channel_layout_uninit(&outlay);
    swr_free(&swr); avcodec_free_context(&ctx); avformat_close_input(&fmt);

    if (!ok || cb.len == 0) { for (int i = 0; i < cb.nch; i++) free(cb.chan[i]); free(cb.chan); seterr(errbuf, errsz, "No decodable audio samples."); return false; }

    out->nch = cb.nch; out->nsamples = cb.len; out->sample_rate = sr; out->chan = cb.chan;
    return true;
}

bool tb_media_probe(const char *path, double *duration, int *width, int *height,
                    char *errbuf, size_t errsz) {
    if (duration) *duration = 0; if (width) *width = 0; if (height) *height = 0;
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) { seterr(errbuf, errsz, "Could not open the media file."); return false; }
    if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); seterr(errbuf, errsz, "Could not read stream info."); return false; }
    if (duration && fmt->duration > 0) *duration = fmt->duration / (double)AV_TIME_BASE;
    int v = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (v >= 0) { AVCodecParameters *p = fmt->streams[v]->codecpar; if (width) *width = p->width; if (height) *height = p->height; }
    avformat_close_input(&fmt);
    return true;
}

/* ---------- embedded subtitle extraction ---------- */
bool tb_media_extract_subtitles(const char *path, tb_cue_list *out) {
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return false;
    if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); return false; }

    /* choose subtitle stream: prefer default flag, else first text subtitle */
    int chosen = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVStream *s = fmt->streams[i];
        if (s->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
        const AVCodec *d = avcodec_find_decoder(s->codecpar->codec_id);
        if (!d) continue;
        if (chosen < 0) chosen = (int)i;
        if (s->disposition & AV_DISPOSITION_DEFAULT) { chosen = (int)i; break; }
    }
    if (chosen < 0) { avformat_close_input(&fmt); return false; }

    AVStream *st = fmt->streams[chosen];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    if (avcodec_open2(ctx, dec, NULL) < 0) { avcodec_free_context(&ctx); avformat_close_input(&fmt); return false; }

    AVPacket *pkt = av_packet_alloc();
    AVSubtitle sub;
    AVRational tb = st->time_base;
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == chosen) {
            int got = 0;
            if (avcodec_decode_subtitle2(ctx, &sub, &got, pkt) >= 0 && got) {
                double start = (pkt->pts != AV_NOPTS_VALUE ? pkt->pts * av_q2d(tb) : 0) + sub.start_display_time / 1000.0;
                double end = start + (sub.end_display_time - sub.start_display_time) / 1000.0;
                if (end <= start) end = start + (pkt->duration > 0 ? pkt->duration * av_q2d(tb) : 4.0);
                for (unsigned r = 0; r < sub.num_rects; r++) {
                    AVSubtitleRect *rect = sub.rects[r];
                    const char *raw = rect->ass ? rect->ass : rect->text;
                    if (!raw) continue;
                    /* The decoded ASS rect uses the standard libavcodec event format:
                     *   ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text
                     * so the text starts after the 8th comma. (Legacy libavcodec emitted
                     * a "Dialogue:"-prefixed line with Start/End -> 9 commas; handle both.) */
                    const char *body = raw;
                    if (rect->ass) {
                        const char *p = raw; int need = 8;
                        if (strncmp(p, "Dialogue:", 9) == 0) { p += 9; need = 9; }
                        int commas = 0;
                        for (; *p; p++) { if (*p == ',' && ++commas == need) { body = p + 1; break; } }
                    }
                    char *clean = tb_clean_subtitle_text(body);
                    if (clean[0]) tb_cue_list_push(out, start, end, clean);
                    free(clean);
                }
                avsubtitle_free(&sub);
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avcodec_free_context(&ctx); avformat_close_input(&fmt);
    if (out->count) { tb_cue_list_sort(out); snprintf(out->label, sizeof out->label, "Embedded subtitle track"); return true; }
    return false;
}

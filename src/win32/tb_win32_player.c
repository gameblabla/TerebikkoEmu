/* tb_win32_player.c - threaded ffmpeg decode + native Win32 audio output. */
#include "tb_player.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "tb_win32_audio.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define QCAP 24            /* frame ring capacity (with time-bounded look-ahead) */
#define LOOKAHEAD 0.30     /* decode at most this many seconds ahead of the clock */

/* ---------------- hardware-decode backend selection ----------------
 * Each backend is gated by its platform macro so a build only references the
 * device types that make sense for the target. NVDEC (CUDA) and VideoToolbox are
 * cross-platform within their vendor; D3D11VA is Windows, VAAPI is Linux desktop,
 * MediaCodec is Android. Everything degrades to software decode at runtime. */
#if defined(_WIN32) && !defined(TB_WIN32_GDI_ONLY)
#  define TB_HAVE_D3D11VA 1
#  define TB_HAVE_NVDEC   1
#elif defined(__ANDROID__)
#  define TB_HAVE_MEDIACODEC 1
#elif defined(__APPLE__)
#  define TB_HAVE_VIDEOTOOLBOX 1
#elif defined(__linux__)
#  define TB_HAVE_VAAPI 1
#  define TB_HAVE_NVDEC 1
#endif

typedef struct { uint8_t *rgba; int w, h, stride; double pts; bool used; } vframe;

struct tb_player {
    AVFormatContext *fmt;
    int vstream, astream;
    AVCodecContext *vctx, *actx;
    struct SwsContext *sws;
    SwrContext *swr;
    int sws_w, sws_h;

    bool audio;
    int dev_rate, dev_ch;          /* device audio format */
    double bytes_per_sec;
    bool swr_ready;                /* resampler configured from first real frame */

    /* hardware decode */
    AVBufferRef        *hw_device_ctx;
    enum AVPixelFormat  hw_pix_fmt;     /* AV_PIX_FMT_NONE if software */
    AVFrame            *hw_xfer;        /* reusable target for GPU->CPU transfer */
    const char         *hw_name;        /* "software" / "NVDEC" / ...           */

    HANDLE thread;
    CRITICAL_SECTION mtx;
    bool mtx_ready;
    bool quit, playing, muted, ended;
    double rate, duration;

    /* clock: master is the AUDIO device position at 1x unmuted playback, so video
     * stays in sync and plays at the correct speed; a wall-clock fallback keeps the
     * playhead moving if the audio backend stalls or there is no audio. */
    double playhead;               /* master clock, seconds, guarded by mtx */
    double audio_start_pts;        /* pts of first fed sample since last (re)start */
    long long total_put;           /* bytes fed to SDL since last (re)start */
    bool need_audio_start;
    double last_wall;              /* seconds (high-res) for wall-clock advance */
    double last_audio_time;        /* last observed audio position (stall detection) */
    double audio_progress_wall;    /* wall time the audio position last advanced */
    double audio_pos;              /* current audio device position (s); used to pick frames */
    bool   audio_is_master;        /* audio currently driving the clock                       */

    /* seek */
    double seek_to;                /* >=0 pending */

    /* video queue */
    vframe q[QCAP];
    int qhead, qcount;             /* ring */
    uint8_t *disp; int disp_cap, disp_w, disp_h, disp_stride;
    double disp_pts;               /* pts currently in disp, -1 = none */
    int width, height;
};

static void lock(tb_player *p) { if (p && p->mtx_ready) EnterCriticalSection(&p->mtx); }
static void unlock(tb_player *p) { if (p && p->mtx_ready) LeaveCriticalSection(&p->mtx); }

/* High-resolution monotonic clock in seconds (nanosecond source), so the playhead
 * isn't quantized to 1 ms steps (which judders frame selection). */
static double mono_sec(void) { static LARGE_INTEGER freq; LARGE_INTEGER now; if (!freq.QuadPart) QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&now); return (double)now.QuadPart / (double)freq.QuadPart; }

/* ---------------- hardware decode helpers ---------------- */
const char *tb_hwaccel_name(tb_hwaccel h) {
    switch (h) {
        case TB_HW_NONE: return "Software";
        case TB_HW_AUTO: return "Auto";
        case TB_HW_NVDEC: return "NVDEC (NVIDIA)";
        case TB_HW_VAAPI: return "VAAPI";
        case TB_HW_D3D11VA: return "Direct3D 11";
        case TB_HW_MEDIACODEC: return "MediaCodec";
        case TB_HW_VIDEOTOOLBOX: return "VideoToolbox";
    }
    return "?";
}

bool tb_hwaccel_supported(tb_hwaccel h) {
    switch (h) {
        case TB_HW_NONE: case TB_HW_AUTO: return true;
        case TB_HW_NVDEC:
#ifdef TB_HAVE_NVDEC
            return true;
#else
            return false;
#endif
        case TB_HW_VAAPI:
#ifdef TB_HAVE_VAAPI
            return true;
#else
            return false;
#endif
        case TB_HW_D3D11VA:
#ifdef TB_HAVE_D3D11VA
            return true;
#else
            return false;
#endif
        case TB_HW_MEDIACODEC:
#ifdef TB_HAVE_MEDIACODEC
            return true;
#else
            return false;
#endif
        case TB_HW_VIDEOTOOLBOX:
#ifdef TB_HAVE_VIDEOTOOLBOX
            return true;
#else
            return false;
#endif
    }
    return false;
}

/* Map an explicit preference to an ffmpeg device type (or NONE if not built). */
static enum AVHWDeviceType hw_type_for(tb_hwaccel h) {
    switch (h) {
#ifdef TB_HAVE_NVDEC
        case TB_HW_NVDEC: return AV_HWDEVICE_TYPE_CUDA;
#endif
#ifdef TB_HAVE_VAAPI
        case TB_HW_VAAPI: return AV_HWDEVICE_TYPE_VAAPI;
#endif
#ifdef TB_HAVE_D3D11VA
        case TB_HW_D3D11VA: return AV_HWDEVICE_TYPE_D3D11VA;
#endif
#ifdef TB_HAVE_MEDIACODEC
        case TB_HW_MEDIACODEC: return AV_HWDEVICE_TYPE_MEDIACODEC;
#endif
#ifdef TB_HAVE_VIDEOTOOLBOX
        case TB_HW_VIDEOTOOLBOX: return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#endif
        default: return AV_HWDEVICE_TYPE_NONE;
    }
}

/* Ordered list of device types to try for AUTO on this platform. */
static int hw_auto_candidates(tb_hwaccel *out, int max) {
    int n = 0;
    (void)max;
#if defined(_WIN32) && !defined(TB_WIN32_GDI_ONLY)
    if (n < max) out[n++] = TB_HW_D3D11VA;
    if (n < max) out[n++] = TB_HW_NVDEC;
#elif defined(__ANDROID__)
    if (n < max) out[n++] = TB_HW_MEDIACODEC;
#elif defined(__APPLE__)
    if (n < max) out[n++] = TB_HW_VIDEOTOOLBOX;
#elif defined(__linux__)
    if (n < max) out[n++] = TB_HW_NVDEC;   /* covers NVIDIA */
    if (n < max) out[n++] = TB_HW_VAAPI;   /* covers Intel / AMD / Mesa */
#endif
    return n;
}

static enum AVPixelFormat g_hw_pix_fmt_tls;   /* read inside get_hw_format */
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *fmts) {
    tb_player *p = (tb_player *)ctx->opaque;
    enum AVPixelFormat want = p ? p->hw_pix_fmt : g_hw_pix_fmt_tls;
    for (const enum AVPixelFormat *f = fmts; *f != AV_PIX_FMT_NONE; f++)
        if (*f == want) return *f;
    /* hardware format not offered for this stream: let ffmpeg pick software */
    return fmts[0];
}

/* Find the hw pixel format the decoder exposes for a device type. */
static enum AVPixelFormat hw_pix_fmt_for(const AVCodec *dec, enum AVHWDeviceType type) {
    for (int i = 0;; i++) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(dec, i);
        if (!cfg) break;
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type)
            return cfg->pix_fmt;
    }
    return AV_PIX_FMT_NONE;
}

/* Try to attach a hw device of `type` to the video codec context. Returns true on
 * success and fills p->hw_pix_fmt / p->hw_device_ctx / p->hw_name. */
static bool try_setup_hw(tb_player *p, const AVCodec *dec, enum AVHWDeviceType type, const char *label) {
    if (type == AV_HWDEVICE_TYPE_NONE) return false;
    enum AVPixelFormat pf = hw_pix_fmt_for(dec, type);
    if (pf == AV_PIX_FMT_NONE) return false;       /* codec can't use this backend */
    AVBufferRef *dev = NULL;
    if (av_hwdevice_ctx_create(&dev, type, NULL, NULL, 0) < 0) return false;
    p->hw_device_ctx = dev;
    p->hw_pix_fmt = pf;
    p->hw_name = label;
    p->vctx->hw_device_ctx = av_buffer_ref(dev);
    p->vctx->opaque = p;
    p->vctx->get_format = get_hw_format;
    g_hw_pix_fmt_tls = pf;
    return true;
}

/* Advance the master clock; called under lock.
 *
 * At 1x unmuted playback the AUDIO DEVICE is the master: the playhead tracks how
 * much audio the device has actually played (so video stays in sync and at correct
 * speed). The wall clock interpolates smoothly between audio updates but is clamped
 * to the audio position so it can't run ahead. If audio isn't available or stalls
 * (no advance for >0.4 s), we fall back to a pure wall clock so playback never
 * freezes. */
static void advance_clock(tb_player *p) {
    double wall = mono_sec();
    double dt = wall - p->last_wall; if (dt < 0) dt = 0;
    p->last_wall = wall;
    if (!p->playing) return;

    bool audio_expected = p->audio && p->rate == 1.0 && !p->muted;
    p->audio_is_master = false;
    if (audio_expected && p->total_put == 0) {
        /* Audio will drive the clock but hasn't started feeding yet. HOLD at the start
         * point instead of advancing on the wall clock — otherwise the playhead races
         * ahead during start-up, the decoder runs ahead, and the video ends up ahead of
         * the audio for the rest of playback. */
        p->audio_is_master = true; p->audio_pos = p->audio_start_pts; p->playhead = p->audio_start_pts;
    } else if (audio_expected) {
        int queued = (int)((int)(tb_win32_audio_queued_media_frames() * p->dev_ch * sizeof(float)));
        double consumed = (double)(p->total_put - queued); if (consumed < 0) consumed = 0;
        double at = p->audio_start_pts + consumed / p->bytes_per_sec;
        if (at > p->last_audio_time + 1e-4) { p->last_audio_time = at; p->audio_progress_wall = wall; }
        if (wall - p->audio_progress_wall > 0.4) {
            p->playhead += dt * p->rate;             /* audio stalled -> wall fallback */
        } else {
            p->audio_is_master = true; p->audio_pos = at;
            p->playhead += dt * p->rate;             /* smooth interpolation... */
            if (p->playhead > at + 0.05) p->playhead = at + 0.05;   /* ...but never run ahead of audio */
            if (p->playhead < at - 0.20) p->playhead = at;          /* snap if we fell well behind */
        }
    } else {
        p->playhead += dt * p->rate;
    }
    if (p->duration > 0 && p->playhead > p->duration) p->playhead = p->duration;
    { static int dbg = -1; if (dbg < 0) dbg = getenv("TB_PDEBUG") ? 1 : 0;
      if (dbg) { static double last = 0, w0 = -1, h0 = 0;
          if (w0 < 0) { w0 = wall; h0 = p->playhead; }
          if (wall - last > 0.5) {
              double rate = (wall - w0 > 0.01) ? (p->playhead - h0) / (wall - w0) : 0;
              fprintf(stderr, "[clk] wall=%.2f head=%.3f at=%.3f am=%d put=%lld bps=%.0f head_rate=%.2fx\n",
                      wall, p->playhead, p->audio_pos, p->audio_is_master, p->total_put, p->bytes_per_sec, rate);
              last = wall;
          } } }
}

/* ---------- video queue helpers (called on decode thread under lock) ---------- */
static bool queue_has_future_frame(tb_player *p) {
    for (int i = 0; i < p->qcount; i++) {
        vframe *f = &p->q[(p->qhead + i) % QCAP];
        if (f->pts >= p->playhead - 0.05) return true;
    }
    return false;
}

static void queue_clear(tb_player *p) {
    for (int i = 0; i < QCAP; i++) { av_free(p->q[i].rgba); p->q[i].rgba = NULL; p->q[i].used = false; }
    p->qhead = 0; p->qcount = 0;
}

/* Takes ownership of `buf` (an av_image_alloc'd RGBA buffer). No memcpy under the
 * lock: the lock is held only for pointer/index swaps, so the decode thread never
 * stalls the paint thread for the duration of an 11 MB copy. */
static void push_frame(tb_player *p, uint8_t *buf, int w, int h, int stride, double pts) {
    lock(p);
    if (p->qcount == QCAP) {   /* drop oldest, freeing its buffer */
        av_free(p->q[p->qhead].rgba); p->q[p->qhead].rgba = NULL;
        p->qhead = (p->qhead + 1) % QCAP; p->qcount--;
    }
    int idx = (p->qhead + p->qcount) % QCAP;
    av_free(p->q[idx].rgba);   /* defensive: slot should already be empty */
    vframe *f = &p->q[idx];
    f->rgba = buf; f->w = w; f->h = h; f->stride = stride; f->pts = pts; f->used = true;
    p->qcount++;
    unlock(p);
}

/* ---------- decode thread ---------- */
static DWORD WINAPI decode_thread(void *arg) {
    tb_player *p = arg;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();
    int DBG = getenv("TB_PDEBUG") ? 1 : 0;
    int vpushed = 0;

    while (1) {
        lock(p);
        bool quit = p->quit;
        double seek = p->seek_to;
        bool playing = p->playing;
        bool needfill = !queue_has_future_frame(p);
        bool vroom = (p->qcount < QCAP - 1);
        /* newest queued video pts: stop decoding once we're LOOKAHEAD seconds ahead of
         * the clock so the decoder can't race far ahead and overflow the ring (which
         * would drop frames the clock still needs). */
        double newest = p->qcount ? p->q[(p->qhead + p->qcount - 1) % QCAP].pts : -1e9;
        bool want_video = vroom && (newest < p->playhead + LOOKAHEAD);
        int aqueued = p->audio ? (int)((int)(tb_win32_audio_queued_media_frames() * p->dev_ch * sizeof(float))) : 0;
        bool needaudio = playing && p->audio && p->rate == 1.0 && !p->muted
                         && aqueued < (int)(p->bytes_per_sec * 0.25);
        unlock(p);
        if (quit) break;

        if (seek >= 0) {
            if (DBG) fprintf(stderr, "[pl] SEEK -> %.3f\n", seek);
            int64_t ts = (int64_t)(seek * AV_TIME_BASE);
            av_seek_frame(p->fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
            if (p->vctx) avcodec_flush_buffers(p->vctx);
            if (p->actx) avcodec_flush_buffers(p->actx);
            if (p->swr_ready && p->swr) { swr_close(p->swr); swr_init(p->swr); } /* drop stale buffered audio */
            lock(p);
            queue_clear(p);
            if (p->audio) tb_win32_audio_clear_media();
            p->total_put = 0; p->need_audio_start = true; p->audio_start_pts = seek;
            p->playhead = seek; p->ended = false; p->seek_to = -1; p->last_wall = mono_sec();
            p->last_audio_time = seek; p->audio_progress_wall = p->last_wall;
            unlock(p);
            continue;
        }

        /* Read when: we still need a frame for the playhead, or (while playing) there is
         * room in the video queue or the SDL audio buffer is running low. Audio feeding
         * must not be starved just because the small video queue is full. */
        bool should_read = needfill || needaudio || (playing && want_video);
        if (!should_read) {
            if (DBG) { static double last = 0; double n = mono_sec(); if (n - last > 0.5) { fprintf(stderr, "[pl] idle q=%d play=%d vroom=%d na=%d nf=%d aq=%d\n", p->qcount, playing, vroom, needaudio, needfill, aqueued); last = n; } }
            Sleep(5); continue;
        }

        int r = av_read_frame(p->fmt, pkt);
        if (r < 0) { if (DBG) fprintf(stderr, "[pl] read EOF/err=%d q=%d\n", r, p->qcount); lock(p); p->ended = true; unlock(p); Sleep(15); continue; }

        if (pkt->stream_index == p->vstream && p->vctx) {
            if (avcodec_send_packet(p->vctx, pkt) >= 0) {
                while (avcodec_receive_frame(p->vctx, frm) >= 0) {
                    double pts = (frm->best_effort_timestamp != AV_NOPTS_VALUE)
                        ? frm->best_effort_timestamp * av_q2d(p->fmt->streams[p->vstream]->time_base) : 0;
                    /* If this is a GPU surface, download it to a CPU frame (NV12/etc.). */
                    AVFrame *src = frm;
                    if (p->hw_device_ctx && frm->format == p->hw_pix_fmt) {
                        av_frame_unref(p->hw_xfer);
                        if (av_hwframe_transfer_data(p->hw_xfer, frm, 0) < 0) continue;
                        src = p->hw_xfer;
                    }
                    int w = src->width, h = src->height;
                    p->sws = sws_getCachedContext(p->sws, w, h, (enum AVPixelFormat)src->format,
                                                  w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
                    /* av_image_alloc gives an aligned, padded destination so sws_scale's
                     * SIMD stores can't overrun (a plain w*4 malloc corrupts the heap for
                     * widths that aren't 32-byte aligned, e.g. 1946px). */
                    uint8_t *dst[4] = {0}; int dstst[4] = {0};
                    if (av_image_alloc(dst, dstst, w, h, AV_PIX_FMT_RGBA, 32) >= 0) {
                        sws_scale(p->sws, (const uint8_t * const *)src->data, src->linesize, 0, h, dst, dstst);
                        push_frame(p, dst[0], w, h, dstst[0], pts);   /* ownership transferred */
                        if (DBG && (++vpushed % 30 == 0)) fprintf(stderr, "[pl] vpushed=%d pts=%.3f q=%d\n", vpushed, pts, p->qcount);
                    }
                }
            }
        } else if (pkt->stream_index == p->astream && p->actx) {
            if (avcodec_send_packet(p->actx, pkt) >= 0) {
                while (avcodec_receive_frame(p->actx, frm) >= 0) {
                    double pts = (frm->best_effort_timestamp != AV_NOPTS_VALUE)
                        ? frm->best_effort_timestamp * av_q2d(p->fmt->streams[p->astream]->time_base) : 0;
                    lock(p);
                    bool feed = p->audio && p->playing && p->rate == 1.0 && !p->muted;
                    /* Only (re)base the audio clock from a frame we actually feed, so a
                     * frame decoded while paused/seeking can't latch the wrong start pts. */
                    if (feed && p->need_audio_start) { p->audio_start_pts = pts; p->need_audio_start = false; p->total_put = 0; }
                    unlock(p);
                    if (feed) {
                        /* Configure the resampler from the first real frame, whose channel
                         * layout / format / rate are always valid (the codec context's may
                         * not be until decoding has started). */
                        if (!p->swr_ready) {
                            if (p->swr) swr_free(&p->swr);
                            AVChannelLayout outlay; av_channel_layout_default(&outlay, p->dev_ch);
                            /* av_channel_layout_copy uninits dst first, so dst must be zeroed */
                            AVChannelLayout inlay; memset(&inlay, 0, sizeof inlay);
                            av_channel_layout_copy(&inlay, &frm->ch_layout);
                            if (inlay.nb_channels < 1) av_channel_layout_default(&inlay, 1);
                            swr_alloc_set_opts2(&p->swr, &outlay, AV_SAMPLE_FMT_FLT, p->dev_rate,
                                                &inlay, (enum AVSampleFormat)frm->format,
                                                frm->sample_rate > 0 ? frm->sample_rate : p->dev_rate, 0, NULL);
                            if (p->swr && swr_init(p->swr) >= 0) p->swr_ready = true;
                            av_channel_layout_uninit(&outlay);
                            av_channel_layout_uninit(&inlay);
                        }
                        if (!p->swr_ready || !p->swr) continue;
                        uint8_t *out = NULL; int outline = 0;
                        int want = swr_get_out_samples(p->swr, frm->nb_samples);
                        if (want < 1) want = frm->nb_samples;
                        av_samples_alloc(&out, &outline, p->dev_ch, want, AV_SAMPLE_FMT_FLT, 0);
                        int got = swr_convert(p->swr, &out, want, (const uint8_t **)frm->extended_data, frm->nb_samples);
                        if (got > 0) {
                            int bytes = got * p->dev_ch * (int)sizeof(float);
                            tb_win32_audio_submit_media((const float*)out, (size_t)got);
                            lock(p); p->total_put += bytes; unlock(p);
                        }
                        av_freep(&out);
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }
    av_frame_free(&frm); av_packet_free(&pkt);
    return 0;
}

/* ---------- open / close ---------- */
static void seterr(char *b, size_t n, const char *m) { if (b && n) { strncpy(b, m, n - 1); b[n - 1] = '\0'; } }

tb_player *tb_player_open(const char *path, tb_hwaccel hw, char *errbuf, size_t errsz) {
    tb_player *p = calloc(1, sizeof *p);
    p->vstream = p->astream = -1; p->rate = 1.0; p->seek_to = -1; p->need_audio_start = true;
    p->hw_pix_fmt = AV_PIX_FMT_NONE; p->hw_name = "software";

    if (avformat_open_input(&p->fmt, path, NULL, NULL) < 0) { seterr(errbuf, errsz, "Could not open the media file."); free(p); return NULL; }
    if (avformat_find_stream_info(p->fmt, NULL) < 0) { seterr(errbuf, errsz, "Could not read stream info."); avformat_close_input(&p->fmt); free(p); return NULL; }
    p->duration = p->fmt->duration > 0 ? p->fmt->duration / (double)AV_TIME_BASE : 0;

    p->vstream = av_find_best_stream(p->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    p->astream = av_find_best_stream(p->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    if (p->vstream >= 0) {
        AVStream *s = p->fmt->streams[p->vstream];
        const AVCodec *d = avcodec_find_decoder(s->codecpar->codec_id);
        p->vctx = avcodec_alloc_context3(d);
        avcodec_parameters_to_context(p->vctx, s->codecpar);

        /* Optional hardware decode: try the requested backend(s); on any failure the
         * codec context simply decodes in software (get_hw_format returns the sw fmt). */
        if (hw != TB_HW_NONE && d) {
            tb_hwaccel cand[4]; int nc = 0;
            if (hw == TB_HW_AUTO) nc = hw_auto_candidates(cand, 4);
            else cand[nc++] = hw;
            for (int i = 0; i < nc; i++) {
                if (!tb_hwaccel_supported(cand[i])) continue;
                if (try_setup_hw(p, d, hw_type_for(cand[i]), tb_hwaccel_name(cand[i]))) break;
            }
        }

        if (avcodec_open2(p->vctx, d, NULL) < 0) { avcodec_free_context(&p->vctx); p->vstream = -1; }
        else {
            p->width = p->vctx->width; p->height = p->vctx->height;
            if (p->hw_device_ctx) p->hw_xfer = av_frame_alloc();
        }
    }
    if (p->astream >= 0) {
        AVStream *s = p->fmt->streams[p->astream];
        const AVCodec *d = avcodec_find_decoder(s->codecpar->codec_id);
        p->actx = avcodec_alloc_context3(d);
        avcodec_parameters_to_context(p->actx, s->codecpar);
        if (avcodec_open2(p->actx, d, NULL) < 0) { avcodec_free_context(&p->actx); p->astream = -1; }
    }
    if (p->vstream < 0 && p->astream < 0) { seterr(errbuf, errsz, "No decodable video or audio streams."); avformat_close_input(&p->fmt); free(p); return NULL; }

    /* Decoded media audio is resampled to the native mixer format
     * (48 kHz stereo float) and rendered by the selected native backend.
     * The 32-bit build compiles only the WaveOut backend; Win64 keeps WASAPI. */
    if (p->astream >= 0) {
        p->dev_ch = TB_WIN32_AUDIO_CHANNELS;
        p->dev_rate = TB_WIN32_AUDIO_RATE;
        p->audio = tb_win32_audio_init();
        if (p->audio) {
            tb_win32_audio_set_running(false);
            p->bytes_per_sec = (double)p->dev_rate * p->dev_ch * sizeof(float);
        }
    }

    InitializeCriticalSection(&p->mtx);
    p->mtx_ready = true;
    p->last_wall = mono_sec();
    p->thread = CreateThread(NULL, 0, decode_thread, p, 0, NULL);
    if (!p->thread) { seterr(errbuf, errsz, "Could not start the decode thread."); tb_player_close(p); return NULL; }
    return p;
}

void tb_player_close(tb_player *p) {
    if (!p) return;
    lock(p); p->quit = true; unlock(p);
    if (p->thread) { WaitForSingleObject(p->thread, INFINITE); CloseHandle(p->thread); }
    if (p->audio) tb_win32_audio_clear_media();
    if (p->swr) swr_free(&p->swr);
    if (p->sws) sws_freeContext(p->sws);
    if (p->hw_xfer) av_frame_free(&p->hw_xfer);
    if (p->vctx) avcodec_free_context(&p->vctx);
    if (p->actx) avcodec_free_context(&p->actx);
    if (p->hw_device_ctx) av_buffer_unref(&p->hw_device_ctx);
    if (p->fmt) avformat_close_input(&p->fmt);
    for (int i = 0; i < QCAP; i++) av_free(p->q[i].rgba);
    av_free(p->disp);
    if (p->mtx_ready) DeleteCriticalSection(&p->mtx);
    free(p);
}

const char *tb_player_active_hwaccel(const tb_player *p) {
    return p && p->hw_name ? p->hw_name : "software";
}

/* ---------- controls ---------- */
void tb_player_play(tb_player *p) {
    lock(p);
    p->playing = true; p->last_wall = mono_sec();
    /* rebase the audio clock to the current playhead; the next fed frame fixes the pts */
    p->need_audio_start = true; p->total_put = 0; p->audio_start_pts = p->playhead;
    p->last_audio_time = p->playhead; p->audio_progress_wall = p->last_wall;
    if (p->audio) { tb_win32_audio_clear_media(); tb_win32_audio_set_running(true); }
    unlock(p);
}
void tb_player_pause(tb_player *p) { lock(p); advance_clock(p); p->playing = false; if (p->audio) tb_win32_audio_set_running(false); unlock(p); }
bool tb_player_is_paused(const tb_player *p) { return !p->playing; }
void tb_player_seek(tb_player *p, double s) { lock(p); if (s < 0) s = 0; p->seek_to = s; p->playhead = s; unlock(p); }
void tb_player_set_rate(tb_player *p, double r) { lock(p); advance_clock(p); p->rate = r > 0 ? r : 1.0; unlock(p); }
void tb_player_set_muted(tb_player *p, bool m) { lock(p); p->muted = m; if (p->audio) { tb_win32_audio_set_media_muted(m); tb_win32_audio_clear_media(); } p->need_audio_start = true; unlock(p); }
double tb_player_duration(const tb_player *p) { return p->duration; }
bool tb_player_ended(const tb_player *p) { return p->ended && p->playhead >= p->duration - 0.3; }
void tb_player_dimensions(const tb_player *p, int *w, int *h) { if (w) *w = p->width; if (h) *h = p->height; }

double tb_player_time(const tb_player *p) {
    tb_player *m = (tb_player *)p;
    lock(m); advance_clock(m); double t = m->playhead; unlock(m);
    return t;
}

tb_frame tb_player_acquire_frame(tb_player *p) {
    tb_frame out; memset(&out, 0, sizeof out);
    lock(p);
    advance_clock(p);

    /* Select frames at the actual AUDIO device position when audio drives the clock,
     * so video is locked to audio and can never outrun it (the interpolated playhead
     * is only a fallback for no-audio / stalled cases). */
    double sel = p->audio_is_master ? p->audio_pos : p->playhead;
    double now = mono_sec();

    /* choose newest frame with pts <= sel */
    int best = -1; double bestpts = -1;
    for (int i = 0; i < p->qcount; i++) {
        int idx = (p->qhead + i) % QCAP;
        vframe *f = &p->q[idx];
        if (f->pts <= sel + 0.001 && f->pts >= bestpts) { bestpts = f->pts; best = idx; }
    }
    /* Only fall back to the oldest queued frame when nothing is on screen yet (the very
     * first frame). Otherwise, if the clock hasn't reached any queued frame, KEEP the
     * current frame rather than jumping ahead — jumping ahead is what made the video
     * outrun the audio. The look-ahead-bounded decoder keeps the queue spanning the
     * clock, so best is normally found and this never freezes. */
    if (best < 0 && !p->disp && p->qcount > 0) best = p->qhead;

    { static int dbg = -1; if (dbg < 0) dbg = getenv("TB_PDEBUG") ? 1 : 0;
      if (dbg) { static double last = 0; if (now - last > 0.5) {
          double fp = p->qcount ? p->q[p->qhead].pts : -9;
          double lp = p->qcount ? p->q[(p->qhead + p->qcount - 1) % QCAP].pts : -9;
          fprintf(stderr, "[acq] sel=%.3f q=%d head=%.3f tail=%.3f best=%d bestpts=%.3f disp=%.3f\n",
                  sel, p->qcount, fp, lp, best, best >= 0 ? p->q[best].pts : -9, p->disp_pts); last = now; } } }

    if (best >= 0) {
        /* Copy-free: free frames older than the chosen one, then transfer the chosen
         * buffer's ownership into `disp`. No memcpy anywhere in the hot path. */
        while (p->qhead != best) {
            av_free(p->q[p->qhead].rgba); p->q[p->qhead].rgba = NULL; p->q[p->qhead].used = false;
            p->qhead = (p->qhead + 1) % QCAP; p->qcount--;
        }
        vframe *f = &p->q[best];
        av_free(p->disp);
        p->disp = f->rgba; f->rgba = NULL; f->used = false;
        p->disp_w = f->w; p->disp_h = f->h; p->disp_stride = f->stride; p->disp_pts = f->pts;
        p->qhead = (p->qhead + 1) % QCAP; p->qcount--;   /* chosen frame moved out of the ring */
    }
    if (p->disp) {
        out.rgba = p->disp; out.width = p->disp_w; out.height = p->disp_h;
        out.stride = p->disp_stride; out.pts = p->disp_pts; out.valid = true;
    }
    unlock(p);
    return out;
}

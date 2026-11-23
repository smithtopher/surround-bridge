// PORTABLE: the live pipeline (see bridge.h). FFmpeg + the shared output/upmix/encode layers.
// Bitstream: the converted audio (b->fmt = the encoder's format, b->enc_layout) collects in the
// fifo and leaves as whole encoder frames -> IEC 61937 bursts. PCM (b->pcm): b->fmt is packed
// float in the device's layout, and the fifo is written to the output as it fills.
#include "bridge.h"
#include "pc_capture.h"
#include "pc_encode.h"
#include "pc_output.h"
#include "pc_platform.h"
#include "pc_upmix.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define RING_SECONDS 1.0
#define CHUNK 1024                       // frames the worker takes at a time
#define COMP_DISTANCE 24000              // drift correction applied over 0.5 s of output
#define MAX_PPM 1000.0                   // clocks are within ~±100 ppm; leave headroom
#define KP_PPM_PER_MS 40.0               // drift control: proportional, ppm per ms of buffer error
#define KI_PPM_PER_MS_S 0.8              // integral, ppm per ms of error per second
#define MAX_I_PPM 300.0                  // integral term limit (anti-windup)

struct SBBridge {
    SBConfig cfg;
    char *s_input, *s_file, *s_output, *s_dump;

    pc_mutex lock;                       // upmix settings, status strings
    PCUpmix upmix;
    _Atomic int upmix_gen;

    // input ring: interleaved float, single producer (capture / feeder), single consumer (worker)
    float *ring;
    int ring_frames, in_ch;
    double in_rate;
    _Atomic int64_t wpos, rpos;
    _Atomic long long overruns;

    PCCapture *cap;
    PCCaptureFormat capfmt;
    AVFormatContext *ifmt;               // input_file
    AVCodecContext *idec;
    int istream;

    pc_thread worker, feeder;
    int have_worker, have_feeder, running;
    _Atomic int quit, input_eof, out_serial, worker_done;

    // worker only
    PCOut *out;
    PCEncoder *enc;                      // NULL in PCM mode
    int pcm;                             // SB_CODEC_PCM: multichannel PCM, no encoder
    long long pcm_gaps;                  // PCM underruns already refilled (worker only)
    enum AVSampleFormat fmt;             // what the converter / upmix graph / fifo hold
    SwrContext *swr;
    PCUpmixer *um;
    int um_gen, upmixing;
    AVAudioFifo *fifo;
    AVChannelLayout enc_layout;
    int64_t comp_next;                   // output sample count of the next drift update
    double target;                       // output ring fill held (s); auto: what it settles at
    long long gaps_seen;                 // output gaps already answered by raising the target
    int64_t skip_in;                     // input frames still to discard (worker only): latency trims
    int64_t settle_at, cooldown_until;   // produced-sample marks: auto-target lock, after a jump cut
    int target_locked;
    _Atomic long long jumps;             // big buffer jumps corrected by dropping input
    int64_t out_samples;
    double fill_ema, integ;

    // status (worker writes, sb_bridge_status reads; doubles are fine to tear-read for display)
    _Atomic double st_fill, st_ppm;
    float peaks[8];
    int peak_frames;
    char out_desc[512];
    uint8_t silence[PC_MAX_BURST_BYTES];   // one encoded-silence burst, for the start-up prefill
    int silence_len;
};

static void free_cfg_strings(SBBridge *b) {
    av_freep(&b->s_input); av_freep(&b->s_file); av_freep(&b->s_output); av_freep(&b->s_dump);
}

SBBridge *sb_bridge_create(void) {
    av_log_set_level(AV_LOG_ERROR);
    SBBridge *b = av_mallocz(sizeof(*b));
    if (!b) return NULL;
    pc_mutex_init(&b->lock);
    b->upmix = (PCUpmix){ .enabled = 1, .include_video = 1, .surround_db = -3, .center = 0.4f, .width = 0.55f,
                          .lfe_db = -6, .crossover_hz = 100, .lfe_move = 0, .bass_db = 0, .rear_delay_ms = 10,
                          .rear_highcut_hz = 7000 };
    return b;
}

void sb_bridge_destroy(SBBridge *b) {
    if (!b) return;
    sb_bridge_stop(b);
    pc_mutex_destroy(&b->lock);
    av_free(b);
}

void sb_bridge_set_upmix(SBBridge *b, const PCUpmix *u) {
    PCUpmix v = *u;
    pc_upmix_sanitize(&v);
    pc_mutex_lock(&b->lock);
    b->upmix = v;
    pc_mutex_unlock(&b->lock);
    atomic_fetch_add(&b->upmix_gen, 1);
}

static PCUpmix upmix_settings(SBBridge *b) {
    pc_mutex_lock(&b->lock);
    PCUpmix u = b->upmix;
    pc_mutex_unlock(&b->lock);
    return u;
}

int sb_list_inputs(PCDevice *out, int max) {
#if PC_HAVE_CAPTURE
    return pccap_list(out, max);
#else
    (void)out; (void)max;
    return 0;
#endif
}

int sb_list_outputs(PCDevice *out, int max) { return pc_list_output_devices(out, max); }

// ---------------------------------------------------------------------------
// input ring

static int ring_free(SBBridge *b) {
    return b->ring_frames - (int)(atomic_load(&b->wpos) - atomic_load(&b->rpos));
}

// Producer side. Returns frames written (the rest are dropped and counted).
static int ring_push(SBBridge *b, const float *data, int frames) {
    int64_t w = atomic_load_explicit(&b->wpos, memory_order_relaxed);
    int fit = ring_free(b);
    int n = frames < fit ? frames : fit;
    if (n < frames) atomic_fetch_add(&b->overruns, frames - n);
    int off = (int)(w % b->ring_frames);
    int first = n < b->ring_frames - off ? n : b->ring_frames - off;
    memcpy(b->ring + (size_t)off * b->in_ch, data, (size_t)first * b->in_ch * sizeof(float));
    if (n > first) memcpy(b->ring, data + (size_t)first * b->in_ch, (size_t)(n - first) * b->in_ch * sizeof(float));
    atomic_store_explicit(&b->wpos, w + n, memory_order_release);
    return n;
}

static int ring_pop(SBBridge *b, float *dst, int max) {
    int64_t r = atomic_load_explicit(&b->rpos, memory_order_relaxed);
    int avail = (int)(atomic_load_explicit(&b->wpos, memory_order_acquire) - r);
    int n = avail < max ? avail : max;
    int off = (int)(r % b->ring_frames);
    int first = n < b->ring_frames - off ? n : b->ring_frames - off;
    memcpy(dst, b->ring + (size_t)off * b->in_ch, (size_t)first * b->in_ch * sizeof(float));
    if (n > first) memcpy(dst + (size_t)first * b->in_ch, b->ring, (size_t)(n - first) * b->in_ch * sizeof(float));
    atomic_store_explicit(&b->rpos, r + n, memory_order_release);
    return n;
}

static void on_capture(void *ctx, const float *data, int frames, int64_t ns) {
    (void)ns;
    ring_push(ctx, data, frames);
}

// ---------------------------------------------------------------------------
// testing input: a media file decoded into the ring (paced at real time, or as fast as it goes)

static int open_input_file(SBBridge *b, const char *path, char *err, int errlen) {
    if (avformat_open_input(&b->ifmt, path, NULL, NULL) < 0 || avformat_find_stream_info(b->ifmt, NULL) < 0) {
        snprintf(err, errlen, "Could not open %s", path);
        return -1;
    }
    b->istream = av_find_best_stream(b->ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (b->istream < 0) { snprintf(err, errlen, "No audio in %s", path); return -1; }
    AVCodecParameters *par = b->ifmt->streams[b->istream]->codecpar;
    const AVCodec *c = avcodec_find_decoder(par->codec_id);
    b->idec = c ? avcodec_alloc_context3(c) : NULL;
    if (!b->idec || avcodec_parameters_to_context(b->idec, par) < 0 || avcodec_open2(b->idec, c, NULL) < 0) {
        snprintf(err, errlen, "No decoder for %s", path);
        return -1;
    }
    b->in_ch = par->ch_layout.nb_channels > 8 ? 8 : par->ch_layout.nb_channels;
    b->in_rate = 48000;   // the feeder resamples, like a virtual device running at 48 kHz
    snprintf(b->capfmt.description, sizeof(b->capfmt.description), "file %s (%d ch, %s)", path, b->in_ch,
             b->cfg.input_realtime ? "real time" : "as fast as possible");
    return 0;
}

static void *feeder_thread(void *arg) {
    SBBridge *b = arg;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    SwrContext *swr = NULL;
    float *buf = NULL;
    int buf_frames = 0;
    int64_t pushed = 0, t0 = pc_now_ns();
    AVChannelLayout lay = { 0 };
    av_channel_layout_default(&lay, b->in_ch);
    int done = 0;
    while (!done && !atomic_load(&b->quit)) {
        int r = av_read_frame(b->ifmt, pkt);
        if (r < 0) { avcodec_send_packet(b->idec, NULL); done = 1; }
        else if (pkt->stream_index != b->istream) { av_packet_unref(pkt); continue; }
        else { avcodec_send_packet(b->idec, pkt); av_packet_unref(pkt); }
        while (!atomic_load(&b->quit) && avcodec_receive_frame(b->idec, f) >= 0) {
            if (!swr) {
                AVChannelLayout in = { 0 };
                if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&in, f->ch_layout.nb_channels);
                else av_channel_layout_copy(&in, &f->ch_layout);
                if (swr_alloc_set_opts2(&swr, &lay, AV_SAMPLE_FMT_FLT, 48000, &in, f->format, f->sample_rate, 0, NULL) < 0 ||
                    swr_init(swr) < 0) { av_channel_layout_uninit(&in); done = 1; break; }
                av_channel_layout_uninit(&in);
            }
            int max = swr_get_out_samples(swr, f->nb_samples);
            if (max > buf_frames) { av_free(buf); buf_frames = max; buf = av_malloc((size_t)max * b->in_ch * sizeof(float)); }
            uint8_t *o[1] = { (uint8_t *)buf };
            int n = swr_convert(swr, o, max, (const uint8_t **)f->extended_data, f->nb_samples);
            av_frame_unref(f);
            int off = 0;
            while (n > 0 && off < n && !atomic_load(&b->quit)) {
                if (b->cfg.input_realtime) {
                    int64_t due = t0 + (int64_t)((double)pushed / 48000.0 * 1e9);   // like a device's clock
                    int64_t now = pc_now_ns();
                    if (due > now) pc_sleep_us((due - now) / 1000);
                    int chunk = n - off < 256 ? n - off : 256;
                    off += ring_push(b, buf + (size_t)off * b->in_ch, chunk);   // drops like a device would
                    pushed += chunk;
                    if (off < n && chunk == 0) break;
                } else {
                    int fit = ring_free(b);
                    if (fit <= 0) { pc_sleep_us(1000); continue; }
                    int chunk = n - off < fit ? n - off : fit;
                    off += ring_push(b, buf + (size_t)off * b->in_ch, chunk);
                }
            }
        }
    }
    atomic_store(&b->input_eof, 1);
    swr_free(&swr);
    av_free(buf);
    av_channel_layout_uninit(&lay);
    av_frame_free(&f);
    av_packet_free(&pkt);
    return NULL;
}

// ---------------------------------------------------------------------------
// worker: ring -> resample -> [upmix] -> encode -> IEC 61937 -> output

// PCM: everything in the fifo, as packed float frames in the device's layout.
static int send_pcm(SBBridge *b, int serial) {
    int n = av_audio_fifo_size(b->fifo);
    if (n <= 0) return 0;
    int ch = b->enc_layout.nb_channels;
    uint8_t **tmp = NULL;
    if (av_samples_alloc_array_and_samples(&tmp, NULL, ch, n, AV_SAMPLE_FMT_FLT, 0) < 0) return -1;
    av_audio_fifo_read(b->fifo, (void **)tmp, n);
    int r = pc_out_write(b->out, tmp[0], n * ch * (int)sizeof(float), NAN, &b->out_serial, serial) < 0 ? -1 : 0;
    if (r == 0) b->out_samples += n;
    av_freep(&tmp[0]);
    av_freep(&tmp);
    return r;
}

static int send_bursts(SBBridge *b, int serial) {
    if (b->pcm) return send_pcm(b, serial);
    int fs = pc_enc_frame_size(b->enc);
    uint8_t *planes[8] = { 0 };
    int nplanes = av_sample_fmt_is_planar(pc_enc_sample_fmt(b->enc)) ? b->enc_layout.nb_channels : 1;
    uint8_t **tmp = NULL;
    if (av_samples_alloc_array_and_samples(&tmp, NULL, b->enc_layout.nb_channels, fs, pc_enc_sample_fmt(b->enc), 0) < 0)
        return -1;
    int r = 0;
    while (av_audio_fifo_size(b->fifo) >= fs) {
        av_audio_fifo_read(b->fifo, (void **)tmp, fs);
        for (int c = 0; c < nplanes; c++) planes[c] = tmp[c];
        const uint8_t *burst = NULL;
        int n = pc_enc_encode(b->enc, planes, &burst);
        if (n < 0) { r = -1; break; }
        if (n > 0 && pc_out_write(b->out, burst, n, NAN, &b->out_serial, serial) < 0) { r = -1; break; }
        b->out_samples += fs;
    }
    av_freep(&tmp[0]);
    av_freep(&tmp);
    return r;
}

// Converted (48 kHz, planar) samples into the encoder fifo, through the upmix when it's on.
static int to_fifo(SBBridge *b, uint8_t **data, int n) {
    if (!b->upmixing) return av_audio_fifo_write(b->fifo, (void **)data, n) < n ? -1 : 0;
    int gen = atomic_load(&b->upmix_gen);
    if (gen != b->um_gen) {
        PCUpmix u = upmix_settings(b);
        if (pc_upmix_update(b->um, &u) != 0) {      // the crossover: rebuild (a short dropout)
            pc_upmix_free(&b->um);
            char e[256] = "";
            b->um = pc_upmix_create(&u, 48000, &b->enc_layout, b->fmt, e, sizeof(e));
            if (!b->um) return -1;
        }
        b->um_gen = gen;
    }
    if (n > 0 && pc_upmix_send(b->um, (const float *const *)data, n) < 0) return -1;
    AVFrame *f = av_frame_alloc();
    while (pc_upmix_receive(b->um, f) >= 0) {
        av_audio_fifo_write(b->fifo, (void **)f->extended_data, f->nb_samples);
        av_frame_unref(f);
    }
    av_frame_free(&f);
    return 0;
}

// PCM: `seconds` of silence into the output ring. PCM plays the moment it arrives (there's no
// burst gate building a cushion as with bitstreams), so the ring starts with one and is topped up
// at once when it runs low; the drift control alone (+-1000 ppm = 1 ms/s) would take far too long.
static void pcm_pad(SBBridge *b, double seconds, int serial) {
    int ch = b->enc_layout.nb_channels, frames = (int)(seconds * 48000);
    if (frames <= 0) return;
    float *z = av_calloc((size_t)frames * ch, sizeof(float));
    if (!z) return;
    pc_out_write(b->out, (const uint8_t *)z, frames * ch * (int)sizeof(float), NAN, &b->out_serial, serial);
    av_free(z);
}

// Holds the output ring at its target fill by trimming the resampler (input vs output clock).
//  - auto target (target_buffer_ms 0): 2 s after audio starts, lock onto the fill reached then
//    (the device's own buffer pulls ~30-40 ms ahead at start, so a fixed ring target only adds
//    delay), but never below one burst + 8 ms nor above one burst + 24 ms: on Windows the start-up
//    backlog (VB-Cable) left it at ~72 ms, and the excess is cut once at the lock. Each output gap
//    after that raises the target 8 ms (to one burst + 100 ms at most).
//  - PCM: after an underrun (pc_out_gaps went up) the ring is refilled to the target with silence.
//  - a jump of more than 40 ms over target (a virtual device delivering a backlog when its
//    clients start/stop) is cut at once by dropping that much input; following it at +-1000 ppm
//    would take a minute of wrong lip sync.
static void steer(SBBridge *b, int64_t produced, int serial) {
    if (b->cfg.dump_path) return;   // a file sink consumes instantly: nothing to steer against
    double fill = pc_out_buffered_seconds(b->out);
    b->fill_ema = b->fill_ema * 0.95 + fill * 0.05;
    atomic_store(&b->st_fill, b->fill_ema);
    // the output's write granularity: one burst, or (PCM) up to one worker chunk at a time
    double burst_s = b->pcm ? CHUNK / 48000.0 : pc_enc_burst_bytes(b->enc) / 4 / 48000.0;
    double want = b->target_locked ? b->target : burst_s + 0.008;
    long long pg = b->pcm ? pc_out_gaps(b->out) : 0;
    if (pg > b->pcm_gaps) {   // PCM underrun (the device played silence): refill the cushion at once
        b->pcm_gaps = pg;
        if (fill < want) pcm_pad(b, want - fill, serial);
        b->fill_ema = want;
        b->cooldown_until = produced + 48000 / 2;
        atomic_fetch_add(&b->jumps, 1);
    }
    if (!b->target_locked) {
        if (produced < b->settle_at) return;
        double min_safe = burst_s + 0.008, max_auto = burst_s + 0.024;
        b->target = b->cfg.target_buffer_ms > 0 ? b->cfg.target_buffer_ms / 1000.0
                  : b->fill_ema < min_safe ? min_safe : b->fill_ema > max_auto ? max_auto : b->fill_ema;
        b->gaps_seen = pc_out_gaps(b->out);
        b->target_locked = 1;
        b->integ = 0;
        b->comp_next = produced;
        double excess = b->fill_ema - b->target;   // start-up backlog above the target: cut it once
        if (excess > 0.004) {
            b->skip_in += (int64_t)(excess * b->in_rate);
            b->fill_ema = b->target;
            b->cooldown_until = produced + 2 * 48000;
        }
    }
    long long gaps = pc_out_gaps(b->out);   // an underrun after locking: the target was too low
    if (gaps > b->gaps_seen) {
        b->gaps_seen = gaps;
        if (b->cfg.target_buffer_ms <= 0 && b->target < burst_s + 0.100) b->target += 0.008;
    }
    if (produced < b->comp_next) return;
    double err = b->fill_ema - b->target;               // >0: too much buffered: input clock faster
    if (err > 0.040 && produced >= b->cooldown_until) {
        int64_t drop = (int64_t)(err * b->in_rate);
        b->skip_in += drop;                             // discarded as it arrives (the ring may hold less)
        atomic_fetch_add(&b->jumps, 1);
        b->cooldown_until = produced + 2 * 48000;       // let the ring and its average catch up
        b->fill_ema -= (double)drop / b->in_rate;
        return;
    }
    double dt = COMP_DISTANCE / 48000.0;
    // PI in ppm. Real clocks differ by tens of ppm, so: P 40 ppm per ms of error (an offset
    // closes with a ~25 s time constant); I slowly learns the steady drift, held to +-300 ppm
    // so it can't wind up (it used to, and pinned the correction at the limit).
    b->integ += -err * 1000.0 * KI_PPM_PER_MS_S * dt;
    if (b->integ > MAX_I_PPM) b->integ = MAX_I_PPM;
    if (b->integ < -MAX_I_PPM) b->integ = -MAX_I_PPM;
    double ppm = -err * 1000.0 * KP_PPM_PER_MS + b->integ;
    if (ppm > MAX_PPM) ppm = MAX_PPM;
    if (ppm < -MAX_PPM) ppm = -MAX_PPM;
    atomic_store(&b->st_ppm, ppm);
    swr_set_compensation(b->swr, (int)lrint(ppm * 1e-6 * COMP_DISTANCE), COMP_DISTANCE);
    b->comp_next = produced + COMP_DISTANCE;
}

static void update_peaks(SBBridge *b, const float *x, int n) {
    for (int i = 0; i < n; i++)
        for (int c = 0; c < b->in_ch && c < 8; c++) {
            float v = fabsf(x[i * b->in_ch + c]);
            if (v > b->peaks[c]) b->peaks[c] = v;
        }
    b->peak_frames += n;
}

static void *worker_thread(void *arg) {
    SBBridge *b = arg;
    int serial = atomic_load(&b->out_serial);
    float *chunk = av_malloc((size_t)CHUNK * b->in_ch * sizeof(float));
    int planes_ch = b->upmixing ? 2 : b->enc_layout.nb_channels;
    enum AVSampleFormat pfmt = b->upmixing ? AV_SAMPLE_FMT_FLTP : b->fmt;
    uint8_t **cvt = NULL;
    int cvt_max = swr_get_out_samples(b->swr, CHUNK) + 256;
    av_samples_alloc_array_and_samples(&cvt, NULL, planes_ch, cvt_max, pfmt, 0);
    int64_t produced = 0;
    int primed = 0;
    for (;;) {
        if (atomic_load(&b->quit)) goto done;
        int n = ring_pop(b, chunk, CHUNK);
        if (n == 0) {
            if (atomic_load(&b->input_eof)) break;
            pc_sleep_us(1000);
            continue;
        }
        if (b->skip_in > 0) {   // a latency trim: discard input from the front
            int k = n < b->skip_in ? n : (int)b->skip_in;
            b->skip_in -= k;
            n -= k;
            if (n == 0) continue;
            memmove(chunk, chunk + (size_t)k * b->in_ch, (size_t)n * b->in_ch * sizeof(float));
        }
        if (!primed) {   // first real audio: the auto target locks 2 s from now
            primed = 1;
            b->settle_at = produced + 2 * 48000;
        }
        update_peaks(b, chunk, n);
        const uint8_t *in[1] = { (const uint8_t *)chunk };
        int need = swr_get_out_samples(b->swr, n) + 256;
        if (need > cvt_max) {
            av_freep(&cvt[0]); av_freep(&cvt);
            cvt_max = need;
            av_samples_alloc_array_and_samples(&cvt, NULL, planes_ch, cvt_max, pfmt, 0);
        }
        int out = swr_convert(b->swr, cvt, cvt_max, in, n);
        if (out > 0) {
            produced += out;
            if (to_fifo(b, cvt, out) < 0 || send_bursts(b, serial) < 0) goto done;
        }
        steer(b, produced, serial);
    }
    // end of a file input: drain the resampler and the upmix graph, pad the last frame
    {
        int out = swr_convert(b->swr, cvt, cvt_max, NULL, 0);
        if (out > 0) to_fifo(b, cvt, out);
        if (b->upmixing) {
            int z = pc_upmix_flush_samples(b->um);
            uint8_t **sil = NULL;
            if (av_samples_alloc_array_and_samples(&sil, NULL, 2, z, AV_SAMPLE_FMT_FLTP, 0) >= 0) {
                av_samples_set_silence(sil, 0, z, 2, AV_SAMPLE_FMT_FLTP);
                to_fifo(b, sil, z);
                av_freep(&sil[0]); av_freep(&sil);
            }
        }
        int left = b->pcm ? 0 : av_audio_fifo_size(b->fifo) % pc_enc_frame_size(b->enc);
        if (left) {
            int pad = pc_enc_frame_size(b->enc) - left;
            uint8_t **sil = NULL;
            if (av_samples_alloc_array_and_samples(&sil, NULL, b->enc_layout.nb_channels, pad, b->fmt, 0) >= 0) {
                av_samples_set_silence(sil, 0, pad, b->enc_layout.nb_channels, b->fmt);
                av_audio_fifo_write(b->fifo, (void **)sil, pad);
                av_freep(&sil[0]); av_freep(&sil);
            }
        }
        send_bursts(b, serial);
        // let a device play what's queued
        while (!b->cfg.dump_path && !atomic_load(&b->quit) && pc_out_buffered_seconds(b->out) > 0.005) pc_sleep_us(5000);
    }
done:
    if (cvt) { av_freep(&cvt[0]); av_freep(&cvt); }
    av_free(chunk);
    atomic_store(&b->worker_done, 1);
    return NULL;
}

// ---------------------------------------------------------------------------
// start / stop

int sb_bridge_start(SBBridge *b, const SBConfig *cfg, char *err, int errlen) {
    sb_bridge_stop(b);
    err[0] = 0;
    b->cfg = *cfg;
    b->s_input = cfg->input_uid ? av_strdup(cfg->input_uid) : NULL;
    b->s_file = cfg->input_file ? av_strdup(cfg->input_file) : NULL;
    b->s_output = cfg->output_uid ? av_strdup(cfg->output_uid) : NULL;
    b->s_dump = cfg->dump_path ? av_strdup(cfg->dump_path) : NULL;
    b->cfg.input_uid = b->s_input; b->cfg.input_file = b->s_file; b->cfg.output_uid = b->s_output; b->cfg.dump_path = b->s_dump;
    atomic_store(&b->quit, 0);
    atomic_store(&b->input_eof, 0);
    atomic_store(&b->worker_done, 0);
    atomic_store(&b->wpos, 0);
    atomic_store(&b->rpos, 0);
    atomic_store(&b->overruns, 0);
    b->fill_ema = b->integ = 0;
    b->out_samples = b->comp_next = 0;
    b->target = 0;
    b->target_locked = 0;
    b->skip_in = 0;
    b->pcm_gaps = 0;
    b->settle_at = INT64_MAX;
    b->cooldown_until = 0;
    atomic_store(&b->jumps, 0);
    memset(b->peaks, 0, sizeof(b->peaks));

    // 1. input
    if (b->cfg.input_file) {
        if (open_input_file(b, b->cfg.input_file, err, errlen) < 0) goto fail;
    } else {
#if PC_HAVE_CAPTURE
        b->cap = pccap_open(b->cfg.input_uid, &b->capfmt, err, errlen);
        if (!b->cap) goto fail;
        b->in_ch = b->capfmt.channels > 8 ? 8 : b->capfmt.channels;
        b->in_rate = b->capfmt.rate;
#else
        snprintf(err, errlen, "No capture back-end on this platform yet");
        goto fail;
#endif
    }
    b->ring_frames = (int)(RING_SECONDS * (b->in_rate > 0 ? b->in_rate : 48000));
    b->ring = av_calloc((size_t)b->ring_frames * b->in_ch, sizeof(float));
    if (!b->ring) { snprintf(err, errlen, "Out of memory"); goto fail; }

    // 2. what goes out: upmixed 5.1 (stereo in, upmix on), 5.1 (multichannel in) or 2.0
    PCUpmix up = upmix_settings(b);
    b->upmixing = up.enabled && b->in_ch <= 2;
    // upmixed: the graph makes 5.1 with back surrounds, its aformat stage moves them to the side
    // pair, the layout both encoders take (FFmpeg's DTS encoder has no 5.1(back)) and the one
    // HDMI receivers expect for 5.1 PCM
    if (b->upmixing) b->enc_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1;
    else if (b->in_ch >= 6) b->enc_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1;
    else b->enc_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    b->pcm = b->cfg.codec == SB_CODEC_PCM;
    int dts = b->cfg.codec == SB_CODEC_DTS;
    if (b->pcm) {
        // 3. multichannel PCM, exclusive: ask for the layout, then lay the audio out the way the
        //    device opened (its channel count and speaker order; pc_output converts the samples)
        if (!b->upmixing && b->in_ch >= 8) b->enc_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_7POINT1;
        PCDeviceFormat req = { 0 };
        req.channels = b->enc_layout.nb_channels;
        req.rate = 48000;
        req.channel_mask = (uint32_t)b->enc_layout.u.mask;   // FFmpeg AV_CH_* bits == WAVE SPEAKER_* bits
        if (b->cfg.dump_path)
            b->out = pc_out_open_file(b->cfg.dump_path, 0, req.channels, 48000, req.channel_mask, err, errlen);
        else
            b->out = pc_out_open(b->cfg.output_uid ? b->cfg.output_uid : "", PC_STREAM_PCM_EXCLUSIVE, &req, err, errlen);
        if (!b->out) goto fail;
        if (pc_out_rate(b->out) != 48000) {
            snprintf(err, errlen, "The output opened at %.0f Hz; multichannel PCM needs it at 48 kHz", pc_out_rate(b->out));
            goto fail;
        }
        int ch = pc_out_channels(b->out) > 8 ? 8 : pc_out_channels(b->out);
        uint32_t mask = pc_out_channel_mask(b->out);
        AVChannelLayout got = { 0 };
        if (!mask || av_channel_layout_from_mask(&got, mask) < 0 || got.nb_channels != ch) av_channel_layout_default(&got, ch);
        b->enc_layout = got;
        b->fmt = AV_SAMPLE_FMT_FLT;
    } else {
        int64_t rate = dts ? (b->cfg.dts_bitrate_kbps > 0 && b->cfg.dts_bitrate_kbps < 1000 ? 754500 : 1509750)
                           : (int64_t)(b->cfg.ac3_bitrate_kbps > 0 ? FFMIN(b->cfg.ac3_bitrate_kbps, 640) : 640) * 1000;
        b->enc = pc_enc_open(dts ? AV_CODEC_ID_DTS : AV_CODEC_ID_AC3, &b->enc_layout, rate, err, errlen);
        if (!b->enc) goto fail;
        b->fmt = pc_enc_sample_fmt(b->enc);

        // 3. output (exclusive IEC 61937), or a file
        if (b->cfg.dump_path) b->out = pc_out_open_file(b->cfg.dump_path, 1, 2, 48000, 0, err, errlen);
        else b->out = pc_out_open(b->cfg.output_uid ? b->cfg.output_uid : "", dts ? PC_STREAM_IEC61937_DTS : PC_STREAM_IEC61937_AC3,
                                  NULL, err, errlen);
        if (!b->out) goto fail;
        pc_out_set_burst_size(b->out, pc_enc_burst_bytes(b->enc));
        const uint8_t *sil = NULL;
        int sn = pc_enc_silence_burst(b->enc, &sil);
        if (sn > 0) {
            pc_out_set_silence_burst(b->out, sil, sn);
            memcpy(b->silence, sil, sn);
            b->silence_len = sn;
        }
    }

    // 4. converters: the input's channels/rate -> 48 kHz planar (stereo for the upmix)
    AVChannelLayout in_lay = { 0 }, stereo = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_default(&in_lay, b->in_ch);
    if (swr_alloc_set_opts2(&b->swr, b->upmixing ? &stereo : &b->enc_layout,
                            b->upmixing ? AV_SAMPLE_FMT_FLTP : b->fmt, 48000,
                            &in_lay, AV_SAMPLE_FMT_FLT, (int)lrint(b->in_rate), 0, NULL) < 0 || swr_init(b->swr) < 0) {
        snprintf(err, errlen, "Resampler setup failed");
        goto fail;
    }
    if (b->upmixing) {
        b->um = pc_upmix_create(&up, 48000, &b->enc_layout, b->fmt, err, errlen);
        if (!b->um) goto fail;
        b->um_gen = atomic_load(&b->upmix_gen);
    }
    b->fifo = av_audio_fifo_alloc(b->fmt, b->enc_layout.nb_channels, b->pcm ? CHUNK * 2 : pc_enc_frame_size(b->enc) * 4);

    char lay[64] = "";
    av_channel_layout_describe(&b->enc_layout, lay, sizeof(lay));
    pc_mutex_lock(&b->lock);
    if (b->pcm)
        snprintf(b->out_desc, sizeof(b->out_desc), "PCM %s 48 kHz%s, exclusive -> %s", lay,
                 b->upmixing ? " (upmixed from stereo)" : "", pc_out_description(b->out));
    else
        snprintf(b->out_desc, sizeof(b->out_desc), "%s %s %lld kb/s%s, IEC 61937 -> %s", dts ? "DTS" : "Dolby Digital", lay,
                 (long long)(pc_enc_bit_rate(b->enc) / 1000), b->upmixing ? " (upmixed from stereo)" : "", pc_out_description(b->out));
    pc_mutex_unlock(&b->lock);

    // 5. go: output first (it plays keep-alive silence until data arrives), then the input
    if (b->pcm && !b->cfg.dump_path) pcm_pad(b, CHUNK / 48000.0 + 0.008, atomic_load(&b->out_serial));   // the cushion
    if (!b->cfg.dump_path && pc_out_start(b->out) != 0) { snprintf(err, errlen, "Could not start the output"); goto fail; }
    if (pc_thread_create(&b->worker, worker_thread, b) != 0) { snprintf(err, errlen, "Thread start failed"); goto fail; }
    b->have_worker = 1;
    if (b->cfg.input_file) {
        if (pc_thread_create(&b->feeder, feeder_thread, b) != 0) { snprintf(err, errlen, "Thread start failed"); goto fail; }
        b->have_feeder = 1;
    } else {
#if PC_HAVE_CAPTURE
        if (pccap_start(b->cap, on_capture, b) != 0) { snprintf(err, errlen, "Could not start capturing"); goto fail; }
#endif
    }
    b->running = 1;
    return 0;
fail:
    sb_bridge_stop(b);
    return -1;
}

void sb_bridge_wait(SBBridge *b) {
    if (b->have_feeder) { pc_thread_join(b->feeder); b->have_feeder = 0; }
    if (b->have_worker) { pc_thread_join(b->worker); b->have_worker = 0; }
}

void sb_bridge_stop(SBBridge *b) {
    atomic_store(&b->quit, 1);
    atomic_fetch_add(&b->out_serial, 1);       // unblocks a pending pc_out_write
#if PC_HAVE_CAPTURE
    if (b->cap) { pccap_close(b->cap); b->cap = NULL; }
#endif
    sb_bridge_wait(b);
    if (b->out) { pc_out_close(b->out); b->out = NULL; }   // restores the device (hog, format)
    pc_enc_close(&b->enc);
    pc_upmix_free(&b->um);
    swr_free(&b->swr);
    if (b->fifo) { av_audio_fifo_free(b->fifo); b->fifo = NULL; }
    avcodec_free_context(&b->idec);
    avformat_close_input(&b->ifmt);
    av_freep(&b->ring);
    free_cfg_strings(b);
    b->running = 0;
    b->upmixing = 0;
    b->pcm = 0;
}

void sb_bridge_status(SBBridge *b, SBStatus *st) {
    memset(st, 0, sizeof(*st));
    st->running = b->running && !atomic_load(&b->worker_done);   // a file input finishes by itself
    st->upmixing = b->upmixing;
    st->channels = b->in_ch;
    pc_mutex_lock(&b->lock);
    snprintf(st->input, sizeof(st->input), "%s", b->capfmt.description);
    snprintf(st->output, sizeof(st->output), "%s", b->out_desc);
    pc_mutex_unlock(&b->lock);
    st->buffer_ms = atomic_load(&b->st_fill) * 1000;
    st->drift_ppm = atomic_load(&b->st_ppm);
    st->overruns = atomic_load(&b->overruns);
    st->jumps = atomic_load(&b->jumps);
    st->gaps = b->out ? pc_out_gaps(b->out) : 0;
    st->target_ms = b->target_locked ? b->target * 1000 : 0;
    double lat = b->capfmt.latency + (b->upmixing ? 2048 / 48000.0 : 0) + (CHUNK / 2) / (b->in_rate > 0 ? b->in_rate : 48000)
               + (b->enc ? pc_enc_frame_size(b->enc) / 48000.0 : 0) + atomic_load(&b->st_fill)
               + (b->out && !b->cfg.dump_path ? pc_out_latency(b->out) : 0);
    st->latency_ms = lat * 1000;
    for (int c = 0; c < 8; c++) {
        st->peak_db[c] = b->peaks[c] > 1e-6f ? 20 * log10f(b->peaks[c]) : -120;
        if (b->peak_frames > 24000) b->peaks[c] *= 0.5f;   // decay so the meter follows the music
    }
    if (b->peak_frames > 24000) b->peak_frames = 0;
}

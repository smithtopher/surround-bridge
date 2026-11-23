// Portable audio output: lock-free ring between the engine's audio thread and the
// device's real-time render callback, IEC 61937 burst handling with keep-alive
// silence, pause/flush, and the audio clock. The OS-specific device lives behind
// pc_audio_device.h (CoreAudio on macOS, WASAPI on Windows).
#include "pc_output.h"
#include "pc_audio_device.h"
#include "pc_platform.h"
#include "pc_types.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MARKERS 1024

typedef struct { int64_t pos; double pts; } PtsMarker;

enum { BS_BOUNDARY = 0, BS_REAL = 1, BS_SILENCE = 2 };

struct PCOut {
    PCAudioDevice *dev;             // NULL for the file sink
    PCDeviceFormat fmt;             // render buffer format
    int bitstream;
    // ring format
    int channels;
    double rate;
    double bps;                     // ring bytes per second
    int ring_bpf;                   // ring bytes per frame
    // ring buffer (single producer / render-callback consumer)
    uint8_t *ring;
    int64_t cap;
    _Atomic int64_t wpos, rpos;
    _Atomic int64_t flush_target;
    _Atomic int flush_gen, seen_gen;
    _Atomic int paused, running;
    // bitstream burst state (render callback only)
    int bs_state, bs_phase;
    uint8_t silence[PC_MAX_BURST_BYTES];
    int burst;                      // bitstream burst size in bytes
    _Atomic int have_silence;
    int played_real;                // a real burst has played (render callback only)
    _Atomic long long gaps;         // silence bursts played after real data started: underruns
    // clock record, written by the render callback (seqlock)
    _Atomic uint32_t seq;
    _Atomic int64_t rec_pos, rec_len;
    _Atomic int64_t rec_ns;         // pc_now_ns() time the recorded position is presented
    // pts markers
    pc_mutex mlock;
    PtsMarker markers[MAX_MARKERS];
    int mstart, mcount;
    double frozen;
    // file sink
    FILE *file;
    double file_pts;
    char desc[512];
};

int pc_list_output_devices(PCDevice *out, int max) { return pcad_list(out, max); }
void pc_set_device_change_callback(PCDeviceChangeCallback cb, void *ctx) { pcad_set_change_callback(cb, ctx); }
int pc_out_lost(PCOut *o) { return o && o->dev && pcad_lost(o->dev); }

// ---------------------------------------------------------------------------
// render callback (device real-time thread: no locks, no allocation)

static void store_record(PCOut *o, int64_t pos, int64_t len, int64_t ns) {
    uint32_t s = atomic_load_explicit(&o->seq, memory_order_relaxed);
    atomic_store_explicit(&o->seq, s + 1, memory_order_release);
    atomic_store_explicit(&o->rec_pos, pos, memory_order_relaxed);
    atomic_store_explicit(&o->rec_len, len, memory_order_relaxed);
    atomic_store_explicit(&o->rec_ns, ns, memory_order_relaxed);
    atomic_store_explicit(&o->seq, s + 2, memory_order_release);
}

// Write `frames` stereo s16 frames from src into the device buffer in its format.
static void emit_s16(PCOut *o, uint8_t *dst, const int16_t *src, int frames) {
    int n = frames * 2;
    if (!o->fmt.is_float && o->fmt.bits == 16) {
        memcpy(dst, src, (size_t)n * 2);
    } else if (o->fmt.is_float) {
        float *d = (float *)dst;
        for (int i = 0; i < n; i++) d[i] = (float)src[i] * (1.0f / 32768.0f);
    } else { // 32-bit integer container
        int32_t *d = (int32_t *)dst;
        for (int i = 0; i < n; i++) d[i] = (int32_t)src[i] * 65536;
    }
}

// PCM: the ring holds float; exclusive devices may want 16- or 32-bit integers.
static void emit_float(PCOut *o, uint8_t *dst, const float *src, int frames) {
    int n = frames * o->channels;
    if (o->fmt.is_float) {
        memcpy(dst, src, (size_t)n * 4);
    } else if (o->fmt.bits == 16) {
        int16_t *d = (int16_t *)dst;
        for (int i = 0; i < n; i++) {
            float v = src[i] * 32768.0f;
            d[i] = (int16_t)(v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : lrintf(v));
        }
    } else {
        int32_t *d = (int32_t *)dst;
        for (int i = 0; i < n; i++) {
            double v = (double)src[i] * 2147483648.0;
            d[i] = (int32_t)(v >= 2147483647.0 ? 2147483647 : v <= -2147483648.0 ? INT32_MIN : llrint(v));
        }
    }
}

static void emit_ring(PCOut *o, uint8_t *dst, int64_t pos, int frames) {
    int dbpf = o->fmt.channels * (o->fmt.bits / 8);
    while (frames > 0) {
        int64_t off = pos % o->cap;
        int chunk = (int)((o->cap - off) / o->ring_bpf);
        if (chunk > frames) chunk = frames;
        if (o->bitstream) emit_s16(o, dst, (const int16_t *)(o->ring + off), chunk);
        else emit_float(o, dst, (const float *)(o->ring + off), chunk);
        dst += chunk * dbpf;
        pos += (int64_t)chunk * o->ring_bpf;
        frames -= chunk;
    }
}

static void render(void *ctx, uint8_t *dst, int frames, int64_t present_ns) {
    PCOut *o = ctx;
    int dbpf = o->fmt.channels * (o->fmt.bits / 8);

    int g = atomic_load_explicit(&o->flush_gen, memory_order_acquire);
    int64_t r = atomic_load_explicit(&o->rpos, memory_order_relaxed);
    if (g != atomic_load_explicit(&o->seen_gen, memory_order_relaxed)) {
        int64_t t = atomic_load_explicit(&o->flush_target, memory_order_relaxed);
        if (t > r) r = t;
        if (o->bs_state == BS_REAL) { o->bs_state = BS_BOUNDARY; o->bs_phase = 0; }
        atomic_store_explicit(&o->seen_gen, g, memory_order_release);
    }
    int64_t w = atomic_load_explicit(&o->wpos, memory_order_acquire);
    int paused = atomic_load_explicit(&o->paused, memory_order_relaxed);

    int done = 0, first_real_frame = -1, real_contig = 1;
    int64_t first_real_pos = 0, real_len = 0;

    if (o->bitstream) {
        // Bursts are always written whole, so we only ever switch between real data
        // and silence on burst boundaries; the receiver never sees a torn frame.
        while (done < frames) {
            int left = frames - done;
            if (o->bs_state == BS_SILENCE) {
                int n = (o->burst - o->bs_phase) / 4;
                if (n > left) n = left;
                emit_s16(o, dst + done * dbpf, (const int16_t *)(o->silence + o->bs_phase), n);
                o->bs_phase += n * 4; done += n;
                if (first_real_frame >= 0) real_contig = 0;
                if (o->bs_phase >= o->burst) { o->bs_phase = 0; o->bs_state = BS_BOUNDARY; }
            } else if (o->bs_state == BS_REAL) {
                int n = (o->burst - o->bs_phase) / 4;
                int avail = (int)((w - r) / 4);
                if (n > left) n = left;
                if (n > avail) n = avail;
                if (n <= 0) { o->bs_state = BS_BOUNDARY; o->bs_phase = 0; continue; }
                emit_ring(o, dst + done * dbpf, r, n);
                if (first_real_frame < 0) { first_real_frame = done; first_real_pos = r; }
                if (real_contig) real_len += n * 4;
                r += n * 4; o->bs_phase += n * 4; done += n;
                if (o->bs_phase >= o->burst) { o->bs_phase = 0; o->bs_state = BS_BOUNDARY; }
            } else {
                if (!paused && w - r >= o->burst) {
                    o->bs_state = BS_REAL; o->bs_phase = 0; o->played_real = 1;
                } else if (atomic_load_explicit(&o->have_silence, memory_order_acquire)) {
                    o->bs_state = BS_SILENCE; o->bs_phase = 0;
                    if (o->played_real && !paused) atomic_fetch_add_explicit(&o->gaps, 1, memory_order_relaxed);
                } else {
                    memset(dst + done * dbpf, 0, (size_t)left * dbpf);
                    done = frames;
                }
            }
        }
    } else {
        int avail = paused ? 0 : (int)((w - r) / o->ring_bpf);
        int n = avail < frames ? avail : frames;
        if (n > 0) {
            emit_ring(o, dst, r, n);
            first_real_frame = 0; first_real_pos = r; real_len = (int64_t)n * o->ring_bpf;
            r += real_len;
        }
        if (n < frames) memset(dst + n * dbpf, 0, (size_t)(frames - n) * dbpf);
    }

    atomic_store_explicit(&o->rpos, r, memory_order_release);

    if (first_real_frame >= 0)
        store_record(o, first_real_pos, real_len, present_ns + (int64_t)((double)first_real_frame / o->rate * 1e9));
}

// ---------------------------------------------------------------------------
// open / close

static PCOut *alloc_out(void) {
    PCOut *o = calloc(1, sizeof(*o));
    pc_mutex_init(&o->mlock);
    o->frozen = NAN;
    o->burst = PC_AC3_BURST_BYTES;
    return o;
}

static void alloc_ring(PCOut *o, double seconds) {
    int64_t cap = (int64_t)(o->bps * seconds);
    if (o->bitstream) cap = (cap / 24576 + 1) * 24576;   // multiple of every burst size
    else cap -= cap % o->ring_bpf;
    o->cap = cap;
    o->ring = calloc(1, (size_t)cap);
}

PCOut *pc_out_open(const char *device_uid, PCStreamType type, const PCDeviceFormat *req, char *err, int errlen) {
    PCOut *o = alloc_out();
    o->bitstream = type == PC_STREAM_IEC61937_AC3 || type == PC_STREAM_IEC61937_DTS;
    if (req) o->fmt = *req;     // PC_STREAM_PCM_EXCLUSIVE reads its request from here
    o->dev = pcad_open(device_uid, type, &o->fmt, err, errlen);
    if (!o->dev) { pc_out_close(o); return NULL; }
    if (o->bitstream) {
        o->channels = 2;
        o->rate = 48000.0;
        o->ring_bpf = 4;            // the ring holds s16 stereo IEC 61937 frames
    } else {
        int ok = o->fmt.is_float ? o->fmt.bits == 32
                                 : type == PC_STREAM_PCM_EXCLUSIVE && (o->fmt.bits == 16 || o->fmt.bits == 32);
        if (!ok || o->fmt.channels < 1) {
            snprintf(err, errlen, "Device back-end returned an unsupported PCM format");
            pc_out_close(o);
            return NULL;
        }
        o->channels = o->fmt.channels;
        o->rate = o->fmt.rate;
        o->ring_bpf = o->channels * 4;
    }
    o->bps = o->rate * o->ring_bpf;
    snprintf(o->desc, sizeof(o->desc), "%s", o->fmt.description);
    alloc_ring(o, 0.4);
    return o;
}

PCOut *pc_out_open_file(const char *path, int bitstream, int pcm_channels, double pcm_rate, uint32_t channel_mask,
                         char *err, int errlen) {
    PCOut *o = alloc_out();
    o->file = fopen(path, "wb");
    if (!o->file) { snprintf(err, errlen, "Cannot open %s", path); pc_out_close(o); return NULL; }
    o->bitstream = bitstream;
    o->fmt.channel_mask = bitstream ? 0 : channel_mask;
    o->channels = bitstream ? 2 : pcm_channels;
    o->rate = bitstream ? 48000.0 : pcm_rate;
    o->ring_bpf = bitstream ? 4 : pcm_channels * 4;
    o->bps = o->rate * o->ring_bpf;
    o->file_pts = NAN;
    snprintf(o->desc, sizeof(o->desc), "file %s (%s)", path, bitstream ? "IEC 61937 s16le 48 kHz" : "f32le");
    return o;
}

void pc_out_close(PCOut *o) {
    if (!o) return;
    if (o->file) fclose(o->file);
    pcad_close(o->dev);             // stops the callback before the ring goes away
    pc_mutex_destroy(&o->mlock);
    free(o->ring);
    free(o);
}

int pc_out_start(PCOut *o) {
    if (o->file || atomic_load(&o->running)) return 0;
    if (pcad_start(o->dev, render, o) != 0) return -1;
    atomic_store(&o->running, 1);
    return 0;
}

double pc_out_rate(PCOut *o) { return o->rate; }
uint32_t pc_out_channel_mask(PCOut *o) { return o->fmt.channel_mask; }
int pc_out_channels(PCOut *o) { return o->channels; }
double pc_out_bytes_per_sec(PCOut *o) { return o->bps; }
const char *pc_out_description(PCOut *o) { return o->desc; }

void pc_out_set_burst_size(PCOut *o, int bytes) {
    if (bytes <= 0 || bytes > PC_MAX_BURST_BYTES || bytes % 4 || atomic_load(&o->running)) return;
    if (bytes != o->burst) atomic_store(&o->have_silence, 0);
    o->burst = bytes;
}

int pc_out_burst_size(PCOut *o) { return o->burst; }

void pc_out_set_silence_burst(PCOut *o, const uint8_t *burst, int len) {
    // set once per output; a reused output keeps the identical burst it already has
    if (len != o->burst || atomic_load(&o->have_silence)) return;
    memcpy(o->silence, burst, (size_t)len);
    atomic_store_explicit(&o->have_silence, 1, memory_order_release);
}

// ---------------------------------------------------------------------------
// producer side

static void add_marker(PCOut *o, int64_t pos, double pts) {
    pc_mutex_lock(&o->mlock);
    // drop markers that are long played (keep ~2 s of history for latency lookback)
    int64_t keep_from = atomic_load(&o->rpos) - (int64_t)(o->bps * 2);
    while (o->mcount > 1) {
        PtsMarker *next = &o->markers[(o->mstart + 1) % MAX_MARKERS];
        if (next->pos > keep_from) break;
        o->mstart = (o->mstart + 1) % MAX_MARKERS;
        o->mcount--;
    }
    if (o->mcount == MAX_MARKERS) { o->mstart = (o->mstart + 1) % MAX_MARKERS; o->mcount--; }
    o->markers[(o->mstart + o->mcount) % MAX_MARKERS] = (PtsMarker){ pos, pts };
    o->mcount++;
    pc_mutex_unlock(&o->mlock);
}

int pc_out_write(PCOut *o, const uint8_t *data, int len, double pts, const _Atomic int *serial, int my_serial) {
    if (o->file) {
        fwrite(data, 1, (size_t)len, o->file);
        o->file_pts = pts + len / o->bps;
        return 0;
    }
    if (len > o->cap) return -1;
    int64_t w = atomic_load_explicit(&o->wpos, memory_order_relaxed);
    for (;;) {
        if (serial && atomic_load(serial) != my_serial) return -1;
        int64_t r = atomic_load_explicit(&o->rpos, memory_order_acquire);
        int flush_pending = atomic_load(&o->flush_gen) != atomic_load(&o->seen_gen);
        if (!flush_pending && o->cap - (w - r) >= len) break;
        pc_sleep_us(2000);
    }
    if (!isnan(pts)) add_marker(o, w, pts);
    int64_t off = w % o->cap;
    int first = (int)(o->cap - off < len ? o->cap - off : len);
    memcpy(o->ring + off, data, (size_t)first);
    if (first < len) memcpy(o->ring, data + first, (size_t)(len - first));
    atomic_store_explicit(&o->wpos, w + len, memory_order_release);
    return 0;
}

void pc_out_flush(PCOut *o) {
    if (o->file) { o->file_pts = NAN; return; }
    pc_mutex_lock(&o->mlock);
    o->mcount = 0;
    pc_mutex_unlock(&o->mlock);
    atomic_store(&o->flush_target, atomic_load(&o->wpos));
    int g = atomic_fetch_add(&o->flush_gen, 1) + 1;
    if (!atomic_load(&o->running)) {
        atomic_store(&o->rpos, atomic_load(&o->flush_target));
        atomic_store(&o->seen_gen, g);
        return;
    }
    for (int i = 0; i < 100 && atomic_load(&o->seen_gen) != g; i++) pc_sleep_us(2000);
}

void pc_out_set_paused(PCOut *o, int paused) {
    if (paused) {
        o->frozen = pc_out_clock(o);
        atomic_store(&o->paused, 1);
    } else {
        atomic_store(&o->paused, 0);
        o->frozen = NAN;
    }
}

double pc_out_buffered_seconds(PCOut *o) {
    if (o->file) return 0;
    int64_t w = atomic_load(&o->wpos), r = atomic_load(&o->rpos);
    return (double)(w - r) / o->bps;
}

long long pc_out_gaps(PCOut *o) { return o ? atomic_load(&o->gaps) : 0; }

double pc_out_latency(PCOut *o) { return o->file ? 0 : o->fmt.latency; }

double pc_out_clock(PCOut *o) {
    if (o->file) return o->file_pts;
    if (atomic_load(&o->paused) && !isnan(o->frozen)) return o->frozen;
    int64_t pos, len, ns;
    for (;;) {
        uint32_t s1 = atomic_load_explicit(&o->seq, memory_order_acquire);
        if (s1 & 1) continue;
        pos = atomic_load_explicit(&o->rec_pos, memory_order_relaxed);
        len = atomic_load_explicit(&o->rec_len, memory_order_relaxed);
        ns = atomic_load_explicit(&o->rec_ns, memory_order_relaxed);
        if (atomic_load_explicit(&o->seq, memory_order_acquire) == s1) break;
    }
    if (ns == 0) return NAN;
    double elapsed = (double)(pc_now_ns() - ns) * 1e-9 - o->fmt.latency;
    double p = (double)pos + elapsed * o->bps;
    if (p > (double)(pos + len)) p = (double)(pos + len);

    double pts = NAN;
    pc_mutex_lock(&o->mlock);
    for (int i = o->mcount - 1; i >= 0; i--) {
        PtsMarker *m = &o->markers[(o->mstart + i) % MAX_MARKERS];
        if ((double)m->pos <= p) { pts = m->pts + (p - (double)m->pos) / o->bps; break; }
    }
    pc_mutex_unlock(&o->mlock);
    return pts;
}

void pc_out_debug(PCOut *o) {
    if (!o) return;
    fprintf(stderr, "[out] w=%lld r=%lld flush_gen=%d seen=%d state=%d phase=%d paused=%d markers=%d rec_pos=%lld rec_len=%lld clock=%.3f\n",
            (long long)atomic_load(&o->wpos), (long long)atomic_load(&o->rpos), atomic_load(&o->flush_gen),
            atomic_load(&o->seen_gen), o->bs_state, o->bs_phase, atomic_load(&o->paused), o->mcount,
            (long long)atomic_load(&o->rec_pos), (long long)atomic_load(&o->rec_len), pc_out_clock(o));
}

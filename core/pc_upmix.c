// PORTABLE: stereo -> 5.1 upmix graph (see pc_upmix.h). FFmpeg only.
//
//   abuffer (stereo fltp)
//     -> bass@ub      low shelf, the "bass boost"
//     -> bass@ut      the "treble boost": a high shelf of +T dB at 6 kHz, built as a low shelf of
//                     -T dB there plus +T dB of gain (surround's fl_in/fr_in; its level_in is
//                     overridden by those per-channel levels). For FFmpeg's shelving
//                     filters highshelf(+T) == 10^(T/20) * lowshelf(-T) exactly (same SVF poles,
//                     mix coefficients scaled by A^2), and our FFmpeg is built without "treble".
//     -> surround@us  FFT ambience extraction to 5.1 (FL FR FC LFE BL BR)
//     -> adelay@ud    rear delay
//     -> lowpass@ul   rear high-cut (BL|BR only)
//     -> alimiter     keeps boosts from clipping the encoder (latency-compensated)
//     -> aformat      the output's layout / sample format
//   -> abuffersink
//
// Mapping of the user's settings onto the surround filter (af_surround.c):
//   surround_db  -> bl_out/br_out   rear output level
//   center       -> fc_out + flx/frx: fc_out scales the centre channel (0 at center = 0, i.e. a
//                   phantom centre); above 0.5, flx/frx (the fronts' x-spread exponent) rise, so
//                   centred sound leaves FL/FR and stays in the centre speaker
//   width        -> angle 45..135 degrees (90 = as recorded)
//   lfe_db       -> lfe on/off + lfe_out;  crossover_hz -> lfe_low/lfe_high;  lfe_move -> lfe_mode
//   level_out 1.26 (+2 dB): the filter alone measured -1.3 (centred) to -3 dB (panned) quieter
//   than the stereo it came from, so switching upmix on shouldn't sound like a volume drop
//
// Tone only (pc_tone_create): the same bass and treble shelves for audio that isn't upmixed
// (native multichannel, or stereo with the upmix off), in any layout and sample format:
//   abuffer -> bass@ub -> bass@ut (-T) -> alimiter@ul (level_in +T: the treble make-up gain, and
//   it keeps the boosts from clipping) -> aformat -> abuffersink
// alimiter's latency=1 compensates its lookahead, so output sample k is input sample k (no
// restamping), and an EOF (pc_upmix_end) flushes the last few ms exactly.
#include "pc_upmix.h"

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define WIN_SIZE 4096                  // surround filter window; latency is half of it
#define TREBLE_HZ "6000"               // treble shelf midpoint

struct PCUpmixer {
    AVFilterGraph *graph;
    AVFilterContext *src, *sink;
    PCUpmix cur;
    int rate;
    int tone;                      // tone-only graph (pc_tone_create)
    AVChannelLayout in_layout;     // what pc_upmix_send takes
    enum AVSampleFormat in_fmt;
    AVFrame *in;
};

static float clampf(float v, float lo, float hi) { return isnan(v) ? lo : v < lo ? lo : v > hi ? hi : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void pc_upmix_sanitize(PCUpmix *u) {
    u->enabled = u->enabled != 0;
    u->include_video = u->include_video != 0;
    u->surround_db = clampf(u->surround_db, -40, 6);
    u->center = clampf(u->center, 0, 1);
    u->width = clampf(u->width, 0, 1);
    u->lfe_db = clampf(u->lfe_db, -40, 6);
    u->crossover_hz = clampi(u->crossover_hz, 40, 250);
    u->lfe_move = u->lfe_move != 0;
    u->bass_db = clampf(u->bass_db, 0, 12);
    u->rear_delay_ms = clampf(u->rear_delay_ms, 0, 30);
    u->treble_db = clampf(u->treble_db, 0, 12);
    if (u->rear_highcut_hz != 0) u->rear_highcut_hz = clampi(u->rear_highcut_hz, 2000, 20000);
}

static float db_gain(float db) { return db <= -40 ? 0 : powf(10.f, db / 20.f); }

// Every value the filters get, derived from the settings (one place, used for create + update).
typedef struct {
    float rear, fc_out, front_x, angle, lfe_out, bass, treble, treble_gain, delay_ms, highcut;
    int lfe_on, lfe_low, lfe_high, lfe_mode;
} Params;

static Params params(const PCUpmix *u, int rate) {
    Params q;
    q.rear = db_gain(u->surround_db);
    q.fc_out = fminf(1.f, u->center * 2.f);
    q.front_x = 0.5f + fmaxf(0.f, u->center - 0.5f) * 5.f;      // 0.5 (filter default) .. 3
    q.angle = 45.f + u->width * 90.f;
    q.lfe_on = u->lfe_db > -40;
    q.lfe_out = q.lfe_on ? db_gain(u->lfe_db) : 0;
    q.lfe_low = clampi(u->crossover_hz * 3 / 4, 1, 256);          // full LFE below, ramp up to high
    q.lfe_high = clampi(u->crossover_hz * 5 / 4, q.lfe_low + 1, 512);
    q.lfe_mode = u->lfe_move;
    q.bass = u->bass_db;
    q.treble = u->treble_db;
    q.treble_gain = powf(10.f, u->treble_db / 20.f);
    q.delay_ms = u->rear_delay_ms;
    float nyq = rate * 0.45f;
    q.highcut = u->rear_highcut_hz > 0 ? fminf((float)u->rear_highcut_hz, nyq) : nyq;
    return q;
}

static PCUpmixer *build(PCUpmixer *m, const char *desc, char *err, int errlen);

PCUpmixer *pc_upmix_create(const PCUpmix *settings, int rate, const AVChannelLayout *out_layout,
                           enum AVSampleFormat out_fmt, char *err, int errlen) {
    PCUpmixer *m = av_mallocz(sizeof(*m));
    if (!m) return NULL;
    m->cur = *settings;
    pc_upmix_sanitize(&m->cur);
    m->rate = rate;
    m->in_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    m->in_fmt = AV_SAMPLE_FMT_FLTP;
    Params q = params(&m->cur, rate);
    char desc[1536], lay[64] = "";
    av_channel_layout_describe(out_layout, lay, sizeof(lay));
    snprintf(desc, sizeof(desc),
             "bass@ub=g=%.2f:f=100:t=q:w=0.7,"
             "bass@ut=g=%.2f:f=" TREBLE_HZ ":t=q:w=0.7,"
             "surround@us=chl_out=5.1:win_size=%d:fl_in=%.4f:fr_in=%.4f:level_out=1.26:lfe=%d:lfe_low=%d:lfe_high=%d:lfe_mode=%d:"
             "angle=%.2f:fc_out=%.3f:flx=%.3f:frx=%.3f:bl_out=%.4f:br_out=%.4f:lfe_out=%.4f,"
             "adelay@ud=delays=0|0|0|0|%.2f|%.2f,"
             "lowpass@ul=f=%.0f:c=BL|BR,"
             "alimiter=limit=0.98:attack=5:release=50:level=0:latency=1,"
             "aformat=sample_fmts=%s:channel_layouts=%s",
             q.bass, -q.treble, WIN_SIZE, q.treble_gain, q.treble_gain, q.lfe_on, q.lfe_low, q.lfe_high, q.lfe_mode,
             q.angle, q.fc_out, q.front_x, q.front_x, q.rear, q.rear, q.lfe_out,
             q.delay_ms, q.delay_ms, q.highcut, av_get_sample_fmt_name(out_fmt), lay);
    return build(m, desc, err, errlen);
}

PCUpmixer *pc_tone_create(const PCUpmix *settings, int rate, const AVChannelLayout *layout,
                          enum AVSampleFormat fmt, char *err, int errlen) {
    PCUpmixer *m = av_mallocz(sizeof(*m));
    if (!m) return NULL;
    m->cur = *settings;
    pc_upmix_sanitize(&m->cur);
    m->rate = rate;
    m->tone = 1;
    m->in_fmt = fmt;
    if (av_channel_layout_copy(&m->in_layout, layout) < 0) { pc_upmix_free(&m); return NULL; }
    Params q = params(&m->cur, rate);
    char desc[512], lay[64] = "";
    av_channel_layout_describe(layout, lay, sizeof(lay));
    snprintf(desc, sizeof(desc),
             "bass@ub=g=%.2f:f=100:t=q:w=0.7,"
             "bass@ut=g=%.2f:f=" TREBLE_HZ ":t=q:w=0.7,"
             "alimiter@ul=level_in=%.4f:limit=0.98:attack=5:release=50:level=0:latency=1,"
             "aformat=sample_fmts=%s:channel_layouts=%s",
             q.bass, -q.treble, q.treble_gain, av_get_sample_fmt_name(fmt), lay);
    return build(m, desc, err, errlen);
}

// abuffer (m->in_layout, m->in_fmt) -> desc -> abuffersink. Frees m on failure.
static PCUpmixer *build(PCUpmixer *m, const char *desc, char *err, int errlen) {
    AVFilterInOut *outputs = NULL, *inputs = NULL;
    char args[512], lay[64] = "";
    int r;

    m->graph = avfilter_graph_alloc();
    if (!m->graph) goto fail;
    m->graph->nb_threads = 1;    // audio-rate work; keep it on the audio thread

    av_channel_layout_describe(&m->in_layout, lay, sizeof(lay));
    snprintf(args, sizeof(args), "sample_rate=%d:sample_fmt=%s:channel_layout=%s:time_base=1/%d",
             m->rate, av_get_sample_fmt_name(m->in_fmt), lay, m->rate);
    r = avfilter_graph_create_filter(&m->src, avfilter_get_by_name("abuffer"), "in", args, NULL, m->graph);
    if (r < 0) { snprintf(err, errlen, "upmix: abuffer failed"); goto fail; }
    r = avfilter_graph_create_filter(&m->sink, avfilter_get_by_name("abuffersink"), "out", NULL, NULL, m->graph);
    if (r < 0) { snprintf(err, errlen, "upmix: abuffersink failed"); goto fail; }

    outputs = avfilter_inout_alloc();
    inputs = avfilter_inout_alloc();
    if (!outputs || !inputs) goto fail;
    outputs->name = av_strdup("in");
    outputs->filter_ctx = m->src;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = m->sink;
    r = avfilter_graph_parse_ptr(m->graph, desc, &inputs, &outputs, NULL);
    if (r < 0) { snprintf(err, errlen, "upmix: filter graph rejected (%d)", r); goto fail; }
    r = avfilter_graph_config(m->graph, NULL);
    if (r < 0) { snprintf(err, errlen, "upmix: filter graph config failed (%d)", r); goto fail; }
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    m->in = av_frame_alloc();
    if (!m->in) goto fail;
    return m;
fail:
    if (!err[0]) snprintf(err, errlen, "upmix: out of memory");
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    pc_upmix_free(&m);
    return NULL;
}

void pc_upmix_free(PCUpmixer **pm) {
    PCUpmixer *m = *pm;
    if (!m) return;
    avfilter_graph_free(&m->graph);
    av_frame_free(&m->in);
    av_channel_layout_uninit(&m->in_layout);
    av_free(m);
    *pm = NULL;
}

static int cmd(PCUpmixer *m, const char *target, const char *name, const char *fmt, double v) {
    char arg[64];
    snprintf(arg, sizeof(arg), fmt, v);
    return avfilter_graph_send_command(m->graph, target, name, arg, NULL, 0, 0);
}

int pc_upmix_update(PCUpmixer *m, const PCUpmix *settings) {
    PCUpmix u = *settings;
    pc_upmix_sanitize(&u);
    Params a = params(&m->cur, m->rate), b = params(&u, m->rate);
    int r = 0;
    if (m->tone) {
        if (a.bass != b.bass) r |= cmd(m, "bass@ub", "g", "%.2f", b.bass);
        if (a.treble != b.treble) {
            r |= cmd(m, "bass@ut", "g", "%.2f", -b.treble);
            r |= cmd(m, "alimiter@ul", "level_in", "%.4f", b.treble_gain);
        }
        m->cur = u;
        return r < 0 ? -1 : 0;
    }
    if (a.lfe_low != b.lfe_low || a.lfe_high != b.lfe_high) return 1;   // not a runtime option
    if (a.bass != b.bass) r |= cmd(m, "bass@ub", "g", "%.2f", b.bass);
    if (a.treble != b.treble) {
        r |= cmd(m, "bass@ut", "g", "%.2f", -b.treble);
        r |= cmd(m, "surround@us", "fl_in", "%.4f", b.treble_gain);
        r |= cmd(m, "surround@us", "fr_in", "%.4f", b.treble_gain);
    }
    if (a.angle != b.angle) r |= cmd(m, "surround@us", "angle", "%.2f", b.angle);
    if (a.fc_out != b.fc_out) r |= cmd(m, "surround@us", "fc_out", "%.3f", b.fc_out);
    if (a.front_x != b.front_x) {
        r |= cmd(m, "surround@us", "flx", "%.3f", b.front_x);
        r |= cmd(m, "surround@us", "frx", "%.3f", b.front_x);
    }
    if (a.rear != b.rear) {
        r |= cmd(m, "surround@us", "bl_out", "%.4f", b.rear);
        r |= cmd(m, "surround@us", "br_out", "%.4f", b.rear);
    }
    if (a.lfe_on != b.lfe_on) r |= cmd(m, "surround@us", "lfe", "%.0f", b.lfe_on);
    if (a.lfe_out != b.lfe_out) r |= cmd(m, "surround@us", "lfe_out", "%.4f", b.lfe_out);
    if (a.lfe_mode != b.lfe_mode) r |= cmd(m, "surround@us", "lfe_mode", "%.0f", b.lfe_mode);
    if (a.delay_ms != b.delay_ms) {
        char arg[64];
        snprintf(arg, sizeof(arg), "0|0|0|0|%.2f|%.2f", b.delay_ms, b.delay_ms);
        r |= avfilter_graph_send_command(m->graph, "adelay@ud", "delays", arg, NULL, 0, 0);
    }
    if (a.highcut != b.highcut) r |= cmd(m, "lowpass@ul", "f", "%.0f", b.highcut);
    m->cur = u;
    return r < 0 ? -1 : 0;
}

int pc_upmix_send(PCUpmixer *m, uint8_t *const *data, int n) {
    if (n <= 0) return 0;
    AVFrame *f = m->in;
    av_frame_unref(f);
    f->nb_samples = n;
    f->format = m->in_fmt;
    f->sample_rate = m->rate;
    if (av_channel_layout_copy(&f->ch_layout, &m->in_layout) < 0 || av_frame_get_buffer(f, 0) < 0) return -1;
    av_samples_copy(f->extended_data, data, 0, 0, n, m->in_layout.nb_channels, m->in_fmt);
    int r = av_buffersrc_add_frame_flags(m->src, f, 0);
    av_frame_unref(f);
    return r < 0 ? -1 : 0;
}

int pc_upmix_receive(PCUpmixer *m, AVFrame *out) { return av_buffersink_get_frame(m->sink, out); }

int pc_upmix_end(PCUpmixer *m) { return av_buffersrc_add_frame_flags(m->src, NULL, 0) < 0 ? -1 : 0; }

int pc_upmix_latency(const PCUpmixer *m) { return m->tone ? 0 : WIN_SIZE / 2; }

int pc_upmix_flush_samples(const PCUpmixer *m) { return m->tone ? 0 : WIN_SIZE; }   // latency + one hop

int pc_tone_wanted(const PCUpmix *u) { return u->bass_db >= 0.05f || u->treble_db >= 0.05f; }

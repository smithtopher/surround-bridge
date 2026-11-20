// PORTABLE: AC3/DTS encode + IEC 61937 packing (see pc_encode.h). FFmpeg only.
#include "pc_encode.h"
#include "pc_output.h"   // PC_MAX_BURST_BYTES

#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <stdio.h>
#include <string.h>

struct PCEncoder {
    AVCodecContext *ctx;
    AVFormatContext *spdif;
    AVFrame *frame;
    AVPacket *pkt;
    int64_t pts, spdif_ts;
    int burst_bytes;
    uint8_t *buf;
    int len, cap;
};

static int write_cb(void *opaque, const uint8_t *data, int size) {
    PCEncoder *e = opaque;
    if (e->len + size > e->cap) {
        int cap = (e->len + size) * 2;
        uint8_t *nb = av_realloc(e->buf, cap);
        if (!nb) return AVERROR(ENOMEM);
        e->buf = nb;
        e->cap = cap;
    }
    memcpy(e->buf + e->len, data, size);
    e->len += size;
    return size;
}

static AVCodecContext *open_codec(enum AVCodecID id, const AVChannelLayout *layout, int64_t bit_rate) {
    const AVCodec *c = id == AV_CODEC_ID_AC3 ? avcodec_find_encoder_by_name("ac3") : avcodec_find_encoder_by_name("dca");
    if (!c) c = avcodec_find_encoder(id);
    if (!c) return NULL;
    AVCodecContext *x = avcodec_alloc_context3(c);
    x->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;   // FFmpeg's DTS encoder is flagged experimental
    x->sample_fmt = AV_SAMPLE_FMT_FLTP;
    const enum AVSampleFormat *fmts = NULL;
    int nf = 0;
    if (avcodec_get_supported_config(NULL, c, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, (const void **)&fmts, &nf) >= 0 && fmts && nf > 0) {
        int ok = 0;
        for (int i = 0; i < nf; i++) if (fmts[i] == AV_SAMPLE_FMT_FLTP) ok = 1;
        if (!ok) x->sample_fmt = fmts[0];
    }
    x->sample_rate = 48000;
    av_channel_layout_copy(&x->ch_layout, layout);
    x->bit_rate = bit_rate;
    x->time_base = (AVRational){ 1, 48000 };
    if (avcodec_open2(x, c, NULL) < 0) { avcodec_free_context(&x); return NULL; }
    return x;
}

PCEncoder *pc_enc_open(enum AVCodecID codec, const AVChannelLayout *layout, int64_t bit_rate, char *err, int errlen) {
    PCEncoder *e = av_mallocz(sizeof(*e));
    if (!e) return NULL;
    e->ctx = open_codec(codec, layout, bit_rate);
    if (!e->ctx) { snprintf(err, errlen, "Could not open the %s encoder", codec == AV_CODEC_ID_AC3 ? "AC3" : "DTS"); goto fail; }
    e->burst_bytes = e->ctx->frame_size * 4;
    if (e->burst_bytes > PC_MAX_BURST_BYTES) { snprintf(err, errlen, "Encoder frame too long for IEC 61937"); goto fail; }
    e->frame = av_frame_alloc();
    e->pkt = av_packet_alloc();
    if (!e->frame || !e->pkt) goto fail;
    e->frame->nb_samples = e->ctx->frame_size;
    e->frame->format = e->ctx->sample_fmt;
    e->frame->sample_rate = 48000;
    av_channel_layout_copy(&e->frame->ch_layout, &e->ctx->ch_layout);
    if (av_frame_get_buffer(e->frame, 0) < 0) goto fail;

    if (avformat_alloc_output_context2(&e->spdif, NULL, "spdif", NULL) < 0) { snprintf(err, errlen, "No IEC 61937 muxer"); goto fail; }
    AVStream *st = avformat_new_stream(e->spdif, NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id = codec;
    st->codecpar->sample_rate = 48000;
    av_channel_layout_copy(&st->codecpar->ch_layout, layout);
    st->time_base = (AVRational){ 1, 48000 };
    int iosize = 32768;
    uint8_t *iobuf = av_malloc(iosize);
    e->spdif->pb = avio_alloc_context(iobuf, iosize, 1, e, NULL, write_cb, NULL);
    e->spdif->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_write_header(e->spdif, NULL) < 0) { snprintf(err, errlen, "IEC 61937 muxer failed"); goto fail; }
    avio_flush(e->spdif->pb);
    e->len = 0;
    return e;
fail:
    if (!err[0]) snprintf(err, errlen, "Encoder: out of memory");
    pc_enc_close(&e);
    return NULL;
}

void pc_enc_close(PCEncoder **pe) {
    PCEncoder *e = *pe;
    if (!e) return;
    if (e->spdif) {
        if (e->spdif->pb) {
            av_freep(&e->spdif->pb->buffer);
            avio_context_free(&e->spdif->pb);
        }
        avformat_free_context(e->spdif);
    }
    avcodec_free_context(&e->ctx);
    av_frame_free(&e->frame);
    av_packet_free(&e->pkt);
    av_free(e->buf);
    av_free(e);
    *pe = NULL;
}

int pc_enc_frame_size(const PCEncoder *e) { return e->ctx->frame_size; }
int pc_enc_burst_bytes(const PCEncoder *e) { return e->burst_bytes; }
enum AVSampleFormat pc_enc_sample_fmt(const PCEncoder *e) { return e->ctx->sample_fmt; }
const AVChannelLayout *pc_enc_layout(const PCEncoder *e) { return &e->ctx->ch_layout; }
int64_t pc_enc_bit_rate(const PCEncoder *e) { return e->ctx->bit_rate; }

// Wraps the packet in e->pkt into IEC 61937 bursts appended to e->buf.
static int pack(PCEncoder *e) {
    e->pkt->stream_index = 0;
    e->pkt->pts = e->pkt->dts = e->spdif_ts;
    e->pkt->duration = e->ctx->frame_size;
    e->spdif_ts += e->ctx->frame_size;
    if (av_write_frame(e->spdif, e->pkt) < 0) return -1;
    avio_flush(e->spdif->pb);
    int rem = e->len % e->burst_bytes;   // keep the output burst-aligned whatever the muxer wrote
    if (rem) {
        uint8_t zeros[PC_MAX_BURST_BYTES] = { 0 };
        write_cb(e, zeros, e->burst_bytes - rem);
    }
    return 0;
}

int pc_enc_encode(PCEncoder *e, uint8_t *const *planes, const uint8_t **burst) {
    e->len = 0;
    if (av_frame_make_writable(e->frame) < 0) return -1;
    // planar (AC3: one plane per channel) or packed (FFmpeg's DTS encoder: one interleaved plane)
    int bps = av_get_bytes_per_sample(e->ctx->sample_fmt);
    int planar = av_sample_fmt_is_planar(e->ctx->sample_fmt);
    int nplanes = planar ? e->ctx->ch_layout.nb_channels : 1;
    size_t plane_bytes = (size_t)e->ctx->frame_size * bps * (planar ? 1 : e->ctx->ch_layout.nb_channels);
    for (int c = 0; c < nplanes; c++)
        if (planes[c] != e->frame->extended_data[c])   // pc_enc_silence_burst passes the frame itself
            memcpy(e->frame->extended_data[c], planes[c], plane_bytes);
    e->frame->pts = e->pts;
    e->pts += e->ctx->frame_size;
    if (avcodec_send_frame(e->ctx, e->frame) < 0) return -1;
    while (avcodec_receive_packet(e->ctx, e->pkt) >= 0) {
        int r = pack(e);
        av_packet_unref(e->pkt);
        if (r < 0) return -1;
    }
    *burst = e->buf;
    return e->len;
}

int pc_enc_silence_burst(PCEncoder *e, const uint8_t **burst) {
    // a separate encoder, so the live one's state (and the stream's timestamps) are untouched
    char err[128] = "";
    PCEncoder *s = pc_enc_open(e->ctx->codec_id, &e->ctx->ch_layout, e->ctx->bit_rate, err, sizeof(err));
    if (!s) return 0;
    av_samples_set_silence(s->frame->extended_data, 0, s->frame->nb_samples, s->frame->ch_layout.nb_channels,
                           s->ctx->sample_fmt);
    int n = pc_enc_encode(s, s->frame->extended_data, burst);
    // AC3 can need a frame or two before the first packet comes out
    for (int i = 0; i < 4 && n == 0; i++) n = pc_enc_encode(s, s->frame->extended_data, burst);
    int ok = n >= e->burst_bytes;
    if (ok) {
        if (e->cap < e->burst_bytes) {
            uint8_t *nb = av_realloc(e->buf, e->burst_bytes);
            if (!nb) { pc_enc_close(&s); return 0; }
            e->buf = nb;
            e->cap = e->burst_bytes;
        }
        memcpy(e->buf, *burst, e->burst_bytes);   // the first burst only
        e->len = 0;
        *burst = e->buf;
    }
    pc_enc_close(&s);
    return ok ? e->burst_bytes : 0;
}

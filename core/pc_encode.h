// PORTABLE: Dolby Digital (AC3) / DTS encoder plus IEC 61937 packing (FFmpeg's "spdif" muxer).
// Extracted from Surround Player's engine.c (open_bs_encoder, spdif_*, make_silence_burst).
#ifndef PC_ENCODE_H
#define PC_ENCODE_H

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

typedef struct PCEncoder PCEncoder;

// codec: AV_CODEC_ID_AC3 or AV_CODEC_ID_DTS, 48 kHz. bit_rate: AC3 up to 640000, DTS 1509750 or 754500.
PCEncoder *pc_enc_open(enum AVCodecID codec, const AVChannelLayout *layout, int64_t bit_rate, char *err, int errlen);
void pc_enc_close(PCEncoder **e);

int pc_enc_frame_size(const PCEncoder *e);                 // samples per frame (AC3 1536, DTS 512)
int pc_enc_burst_bytes(const PCEncoder *e);                // IEC 61937 burst (AC3 6144, DTS 2048)
enum AVSampleFormat pc_enc_sample_fmt(const PCEncoder *e); // AC3: fltp; DTS: s32 (packed)
const AVChannelLayout *pc_enc_layout(const PCEncoder *e);
int64_t pc_enc_bit_rate(const PCEncoder *e);

// Encodes one frame (frame_size samples in sample_fmt: one plane per channel if planar, one
// interleaved plane if packed, as FFmpeg's DTS encoder takes) and packs it.
// *burst points at an internal buffer, valid until the next call. Returns the burst length
// (a multiple of the burst size), 0 if the encoder produced nothing yet, -1 on error.
int pc_enc_encode(PCEncoder *e, uint8_t *const *planes, const uint8_t **burst);

// One burst of encoded digital silence (keep-alive while nothing plays). Returns its length.
int pc_enc_silence_burst(PCEncoder *e, const uint8_t **burst);

#endif

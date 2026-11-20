// PORTABLE: stereo -> 5.1 upmix (FFmpeg's "surround" filter plus bass shelf, rear delay, rear
// high-cut and a limiter) as a libavfilter graph. No OS code.
//
// Timing: output sample k corresponds to input sample k - pc_upmix_latency() (the surround
// filter's half window); the caller restamps. The graph only emits whole hops, so to get the
// tail out at the end of a stream, push pc_upmix_flush_samples() of silence.
#ifndef PC_UPMIX_H
#define PC_UPMIX_H

#include "pc_types.h"
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>

typedef struct PCUpmixer PCUpmixer;

// Input: stereo planar float at `rate`. Output: `out_layout` (5.1 back, or what the device
// took) in `out_fmt`.
PCUpmixer *pc_upmix_create(const PCUpmix *u, int rate, const AVChannelLayout *out_layout,
                           enum AVSampleFormat out_fmt, char *err, int errlen);
void pc_upmix_free(PCUpmixer **m);

// Applies new settings. Returns 0 if done live (runtime filter commands), 1 if the graph must
// be rebuilt (the bass crossover is fixed at creation), -1 on error.
int pc_upmix_update(PCUpmixer *m, const PCUpmix *u);

// Feed `n` stereo planar float samples (data[0] = L, data[1] = R).
int pc_upmix_send(PCUpmixer *m, const float *const *data, int n);
// Next output frame, or AVERROR(EAGAIN) when it needs more input.
int pc_upmix_receive(PCUpmixer *m, AVFrame *out);

int pc_upmix_latency(const PCUpmixer *m);        // samples
int pc_upmix_flush_samples(const PCUpmixer *m);  // silence to push at the end

// Clamps every field into its documented range (playercore.h), in place.
void pc_upmix_sanitize(PCUpmix *u);

#endif

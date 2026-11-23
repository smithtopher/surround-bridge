// PORTABLE: stereo -> 5.1 upmix (FFmpeg's "surround" filter plus bass and treble shelves, rear
// delay, rear high-cut and a limiter) as a libavfilter graph, and the tone shelves alone for
// audio that isn't upmixed (pc_tone_create). No OS code.
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

// Tone only: bass + treble shelves and the limiter, `layout` / `fmt` in and out, no latency
// (output sample k = input sample k). Settings apply live through pc_upmix_update.
PCUpmixer *pc_tone_create(const PCUpmix *u, int rate, const AVChannelLayout *layout,
                          enum AVSampleFormat fmt, char *err, int errlen);
// 1 if the settings boost anything (a tone graph is worth running).
int pc_tone_wanted(const PCUpmix *u);

// Feed `n` samples: stereo planar float for an upmixer (data[0] = L, data[1] = R), the
// created layout / format for a tone graph.
int pc_upmix_send(PCUpmixer *m, uint8_t *const *data, int n);
// End of input: pc_upmix_receive then returns what's left, then AVERROR_EOF. The graph takes
// nothing more after this.
int pc_upmix_end(PCUpmixer *m);
// Next output frame, or AVERROR(EAGAIN) when it needs more input.
int pc_upmix_receive(PCUpmixer *m, AVFrame *out);

int pc_upmix_latency(const PCUpmixer *m);        // samples
int pc_upmix_flush_samples(const PCUpmixer *m);  // silence to push at the end

// Clamps every field into its documented range (playercore.h), in place.
void pc_upmix_sanitize(PCUpmix *u);

#endif

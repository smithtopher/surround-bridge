// sbridge: command-line front end for the bridge (prototype; the menu-bar app comes later).
//   sbridge devices
//   sbridge run  [--input DEV] [--output DEV] [--dts] [--bitrate N] [--no-upmix] [--upmix-set k=v,...] [--seconds N]
//   sbridge file <media> <out.spdif> [--realtime] [--dts] [--no-upmix] [--upmix-set k=v,...]
// DEV: a device UID or part of its name ("BlackHole", "SAMSUNG"; Windows: "CABLE Output", or
// "loopback" devices: what plays on an output, e.g. "CABLE Input (loopback)").
#include "bridge.h"
#include "pc_platform.h"

#include <ctype.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <mmsystem.h>   // timeBeginPeriod
#define DEFAULT_INPUT "CABLE Output"   // VB-Audio Virtual Cable's capture side
#else
#define DEFAULT_INPUT "BlackHole"
#endif

static volatile sig_atomic_t g_stop;
static _Atomic int g_started;

// Opening a capture device blocks inside CoreAudio until the user answers the microphone
// permission prompt (or forever, if it was dismissed). Say so instead of hanging silently.
static void *permission_hint(void *arg) {
    (void)arg;
    pc_sleep_us(3000000);
    if (!atomic_load(&g_started))
        fprintf(stderr, "still starting... capturing an audio input needs microphone permission: allow the prompt,\n"
                        "or enable this terminal app in System Settings > Privacy & Security > Microphone\n");
    return NULL;
}
static void on_signal(int s) { (void)s; g_stop = 1; }

static int icontains(const char *hay, const char *needle) {
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (!*n) return 1;
    }
    return 0;
}

// UID as given, or the first device whose UID or name contains it.
static const char *resolve(const char *want, PCDevice *list, int n) {
    if (!want) return NULL;
    for (int i = 0; i < n; i++) if (!strcmp(list[i].uid, want)) return list[i].uid;
    for (int i = 0; i < n; i++) if (icontains(list[i].name, want) || icontains(list[i].uid, want)) return list[i].uid;
    return NULL;
}

static void parse_upmix(const char *spec, PCUpmix *u) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", spec);
    for (char *kv = strtok(buf, ","); kv; kv = strtok(NULL, ",")) {
        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = 0;
        double v = atof(eq + 1);
        if (!strcmp(kv, "surround")) u->surround_db = (float)v;
        else if (!strcmp(kv, "center")) u->center = (float)v;
        else if (!strcmp(kv, "width")) u->width = (float)v;
        else if (!strcmp(kv, "lfe")) u->lfe_db = (float)v;
        else if (!strcmp(kv, "xover")) u->crossover_hz = (int)v;
        else if (!strcmp(kv, "move")) u->lfe_move = (int)v;
        else if (!strcmp(kv, "bass")) u->bass_db = (float)v;
        else if (!strcmp(kv, "delay")) u->rear_delay_ms = (float)v;
        else if (!strcmp(kv, "highcut")) u->rear_highcut_hz = (int)v;
        else fprintf(stderr, "unknown upmix key %s\n", kv);
    }
}

static void list_devices(void) {
    PCDevice d[64];
    int n = sb_list_inputs(d, 64);
    printf("inputs (capture):\n");
    for (int i = 0; i < n; i++) printf("  %-34s %d ch%s  uid=%s\n", d[i].name, d[i].output_channels,
                                       d[i].is_digital ? "  [virtual]" : "", d[i].uid);
    n = sb_list_outputs(d, 64);
    printf("outputs:\n");
    for (int i = 0; i < n; i++) printf("  %-34s %d ch%s%s%s  uid=%s\n", d[i].name, d[i].output_channels,
                                       d[i].is_hdmi ? "  HDMI" : "", d[i].has_compressed_formats ? "  bitstream" : "",
                                       d[i].is_default ? "  (default)" : "", d[i].uid);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: sbridge devices\n"
                        "       sbridge run  [--input DEV | --input-file F] [--output DEV] [--dts] [--bitrate N] [--no-upmix] [--upmix-set k=v,...]\n"
                        "                    [--seconds N] [--buffer-ms N] [--dump F]\n"
                        "                    (--input-file: F at real time, e.g. silent media to test a device;\n"
                        "                     --dump: write the IEC 61937 stream to F instead of a device, to test the capture side)\n"
                        "       sbridge file <media> <out.spdif> [--realtime] [--dts] [--no-upmix] [--upmix-set k=v,...]\n"
                        "DEV: a UID or part of the device name. Upmix keys: surround center width lfe xover move bass delay highcut\n");
        return 1;
    }
    if (!strcmp(argv[1], "devices")) { list_devices(); return 0; }
    int file = !strcmp(argv[1], "file");
    if (!file && strcmp(argv[1], "run")) { fprintf(stderr, "unknown command %s\n", argv[1]); return 1; }
    if (file && argc < 4) { fprintf(stderr, "usage: sbridge file <media> <out.spdif> [...]\n"); return 1; }

    SBConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    PCUpmix up = { .enabled = 1, .include_video = 1, .surround_db = -3, .center = 0.4f, .width = 0.55f, .lfe_db = -6,
                   .crossover_hz = 100, .lfe_move = 0, .bass_db = 0, .rear_delay_ms = 10, .rear_highcut_hz = 7000 };
    const char *in_want = DEFAULT_INPUT, *out_want = NULL;
    double seconds = 0;
    int first = file ? 4 : 2;
    if (file) { cfg.input_file = argv[2]; cfg.dump_path = argv[3]; }
    for (int i = first; i < argc; i++) {
        if (!strcmp(argv[i], "--input") && i + 1 < argc) in_want = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) out_want = argv[++i];
        else if (!strcmp(argv[i], "--dts")) cfg.codec = SB_CODEC_DTS;
        else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) cfg.ac3_bitrate_kbps = cfg.dts_bitrate_kbps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-upmix")) up.enabled = 0;
        else if (!strcmp(argv[i], "--upmix-set") && i + 1 < argc) parse_upmix(argv[++i], &up);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--realtime")) cfg.input_realtime = 1;
        else if (!strcmp(argv[i], "--input-file") && i + 1 < argc) { cfg.input_file = argv[++i]; cfg.input_realtime = 1; }
        else if (!strcmp(argv[i], "--buffer-ms") && i + 1 < argc) cfg.target_buffer_ms = atof(argv[++i]);
        else if (!file && !strcmp(argv[i], "--dump") && i + 1 < argc) cfg.dump_path = argv[++i];   // live input, no device out
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 1; }
    }
    PCDevice ins[64], outs[64];
    if (!file) {
        int ni = sb_list_inputs(ins, 64), no = sb_list_outputs(outs, 64);
        if (!cfg.input_file) {   // --input-file: a file at real time stands in for the virtual device
            cfg.input_uid = resolve(in_want, ins, ni);
            if (!cfg.input_uid) { fprintf(stderr, "no input device matching '%s' (see: sbridge devices)\n", in_want); return 2; }
        }
        if (out_want) {
            cfg.output_uid = resolve(out_want, outs, no);
            if (!cfg.output_uid) { fprintf(stderr, "no output device matching '%s'\n", out_want); return 2; }
        }
        // capturing the loopback of the output itself would feed the bitstream back into the input
        const char *out_uid = cfg.output_uid;
        for (int i = 0; !out_uid && i < no; i++) if (outs[i].is_default) out_uid = outs[i].uid;
        const char *lb = "loopback:";
        if (!cfg.dump_path && cfg.input_uid && out_uid && !strncmp(cfg.input_uid, lb, strlen(lb)) && !strcmp(cfg.input_uid + strlen(lb), out_uid)) {
            fprintf(stderr, "the input is the loopback of the output device itself: pick another --output\n");
            return 2;
        }
    }

    SBBridge *b = sb_bridge_create();
    sb_bridge_set_upmix(b, &up);
    char err[512] = "";
#ifdef _WIN32
    timeBeginPeriod(1);   // pc_sleep_us granularity (the worker polls the input ring every 1 ms)
    (void)permission_hint;
#else
    pc_thread hint;
    if (!file && pc_thread_create(&hint, permission_hint, NULL) == 0) {}   // detached in effect: exits on its own
#endif
    int rc = sb_bridge_start(b, &cfg, err, sizeof(err));
    atomic_store(&g_started, 1);
    if (rc != 0) { fprintf(stderr, "start failed: %s\n", err); return 3; }
    SBStatus st;
    sb_bridge_status(b, &st);
    printf("input:  %s\noutput: %s\n", st.input, st.output);
    if (file) {
        sb_bridge_wait(b);
        sb_bridge_destroy(b);
        printf("done\n");
        return 0;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    int64_t t0 = pc_now_ns();
    while (!g_stop && (seconds <= 0 || (pc_now_ns() - t0) / 1e9 < seconds)) {
        pc_sleep_us(1000000);
        sb_bridge_status(b, &st);
        if (cfg.input_file && !st.running) break;
        printf("buffer %5.1f ms (target %4.1f)  drift %+7.1f ppm  latency ~%3.0f ms  overruns %lld  jumps %lld  peak L %5.1f R %5.1f dB\n",
               st.buffer_ms, st.target_ms, st.drift_ppm, st.latency_ms, st.overruns, st.jumps, st.peak_db[0],
               st.channels > 1 ? st.peak_db[1] : -120.f);
    }
    sb_bridge_destroy(b);   // stops and restores the output device
    printf("stopped\n");
    return 0;
}

#!/bin/bash
# Behavioural tests of the bridge's output stream, in file mode (nothing plays on any device):
# test media -> `sbridge file` (the whole live pipeline, input ring included) -> IEC 61937 dump,
# checked with ffprobe/ffmpeg: codec/layout, burst spacing, where each test tone ends up.
#   scripts/regress.sh [sbridge]   (default build/<arch>/sbridge)
# Needs bash, ffmpeg, ffprobe, python3.
set -uo pipefail
cd "$(dirname "$0")/.."
SB="${1:-build/$(uname -m)/sbridge}"
[ -x "$SB" ] || { echo "$SB not found (run scripts/build.sh)"; exit 2; }
PY=$(command -v python3 || command -v python) || { echo "python3 not found"; exit 2; }
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
FAIL=0
pass() { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAIL=1; }
ff() { timeout 60 ffmpeg -nostdin -hide_banner -loglevel error -y "$@"; }
gen() { ff -f lavfi -i "aevalsrc=${1}:s=${3:-44100}:d=6" -t 6 -c:a flac "$W/${2}.flac"; }   # always capped at 6 s

echo "generating test media in $W"
gen "0.25*sin(2*PI*1000*t)|0" left
gen "0.25*sin(2*PI*1000*t)|0.25*sin(2*PI*1000*t)" centre
gen "0.25*sin(2*PI*1000*t)|-0.25*sin(2*PI*1000*t)" anti
# 5.1 with one tone per channel (FL 300, FR 500, FC 700, LFE 60, SL 1100, SR 1300)
ff -f lavfi -i sine=f=300:sample_rate=48000 -f lavfi -i sine=f=500:sample_rate=48000 -f lavfi -i sine=f=700:sample_rate=48000 \
   -f lavfi -i sine=f=60:sample_rate=48000 -f lavfi -i sine=f=1100:sample_rate=48000 -f lavfi -i sine=f=1300:sample_rate=48000 \
   -filter_complex "join=inputs=6:channel_layout=5.1(side):map=0.0-FL|1.0-FR|2.0-FC|3.0-LFE|4.0-SL|5.0-SR[a]" \
   -map "[a]" -t 6 -c:a flac "$W/six.flac"

cat > "$W/levels.py" <<'EOF'
# levels (dB) per channel of a 6-ch f32le decode, seconds 2..4; then a rule
import sys, math
from array import array
a = array('f'); a.frombytes(open(sys.argv[1], 'rb').read()); ch = 6; rate = 48000
seg = a[2 * rate * ch: 4 * rate * ch]
db = []
for c in range(ch):
    x = seg[c::ch]; e = math.fsum(v * v for v in x) / max(1, len(x)); db.append(10 * math.log10(e) if e > 1e-15 else -150.0)
FL, FR, FC, LFE, SL, SR = db
ok = {'left': FL > max(FR, FC, SR) + 20, 'centre': FC > FL - 6 and max(SL, SR) < FL - 30,
      'anti': min(SL, SR) > max(FL, FR, FC) + 6}[sys.argv[2]]
print(('ok ' if ok else 'BAD ') + ' '.join('%s %.1f' % (n, v) for n, v in zip(['FL', 'FR', 'FC', 'LFE', 'SL', 'SR'], db)))
EOF
cat > "$W/bursts.py" <<'EOF'
import sys
d = open(sys.argv[1], 'rb').read()
offs = [i for i in range(0, len(d) - 4, 2) if d[i:i+4] == b'\x72\xf8\x1f\x4e']
print(sorted(set(offs[i+1] - offs[i] for i in range(len(offs) - 1))) if len(offs) > 1 else [])
EOF
cat > "$W/freqs.py" <<'EOF'
import sys, wave, struct
w = wave.open(sys.argv[1]); ch = w.getnchannels(); sr = w.getframerate()
w.readframes(sr); data = w.readframes(sr * 3)
n = len(data) // (2 * ch); s = struct.unpack('<%dh' % (n * ch), data)
print(' '.join(str(round(sum(1 for i in range(1, n) if (x[i-1] < 0) != (x[i] < 0)) / 2 / (n / sr), -1))
               for x in (s[c::ch] for c in range(ch))))
EOF

run() { timeout 120 "$SB" file "$@" > "$W/log.txt" 2>&1 || { fail "sbridge file $*: $(tail -2 "$W/log.txt")"; return 1; }; }
stream() { # name dump codec layout spacing
    local info; info=$(ffprobe -v error -f spdif -i "$2" -show_entries stream=codec_name,channel_layout,sample_rate -of csv=p=0)
    [ "$info" = "$3,48000,$4" ] && pass "$1: $3 $4 48 kHz" || fail "$1: got '$info', want '$3,48000,$4'"
    local sp; sp=$("$PY" -I "$W/bursts.py" "$2")
    [ "$sp" = "[$5]" ] && pass "$1: bursts every $5 bytes" || fail "$1: burst spacing $sp"
}
place() { # name dump rule
    ff -f spdif -i "$2" -f f32le -ac 6 "$W/dec.raw"
    local r; r=$("$PY" -I "$W/levels.py" "$W/dec.raw" "$3")
    case "$r" in ok*) pass "$1: ${r#ok }";; *) fail "$1: ${r#BAD }";; esac
}

for codec in ac3 dts; do
    flag=""; spacing=6144; [ $codec = dts ] && { flag=--dts; spacing=2048; }
    echo "$codec, stereo upmixed to 5.1"
    run "$W/centre.flac" "$W/c.spdif" $flag && { stream "$codec upmix" "$W/c.spdif" $codec "5.1(side)" $spacing; place "$codec centred" "$W/c.spdif" centre; }
    run "$W/left.flac" "$W/l.spdif" $flag && place "$codec hard left" "$W/l.spdif" left
    run "$W/anti.flac" "$W/a.spdif" $flag && place "$codec ambience" "$W/a.spdif" anti
    echo "$codec, upmix off"
    run "$W/centre.flac" "$W/s.spdif" $flag --no-upmix && stream "$codec stereo" "$W/s.spdif" $codec stereo $spacing
done

echo "5.1 input stays 5.1 (no upmix), channels in place"
run "$W/six.flac" "$W/6.spdif" && {
    stream "5.1 input" "$W/6.spdif" ac3 "5.1(side)" 6144
    ff -f spdif -i "$W/6.spdif" -c:a pcm_s16le "$W/6.wav"
    f=$("$PY" -I "$W/freqs.py" "$W/6.wav")
    [ "$f" = "300.0 500.0 700.0 60.0 1100.0 1300.0" ] && pass "5.1 input: channel mapping ($f)" || fail "5.1 input: channels '$f'"
}

echo "real-time pacing (the stand-in for a device's clock)"
t0=$("$PY" -c 'import time; print(time.time())')
run "$W/left.flac" "$W/rt.spdif" --realtime && {
    dt=$("$PY" -c "import time, sys; print('%.2f' % (time.time() - float(sys.argv[1])))" "$t0")
    "$PY" -c "import sys; sys.exit(0 if 5.8 < float(sys.argv[1]) < 7.0 else 1)" "$dt" && pass "6 s file took $dt s" || fail "6 s file took $dt s"
}

cat > "$W/rawfreqs.py" <<'EOF'
# f32le file with N channels: frames, then the zero-crossing frequency of each channel (s 1..4)
import sys
from array import array
a = array('f'); a.frombytes(open(sys.argv[1], 'rb').read()); ch = int(sys.argv[2]); rate = 48000
n = len(a) // ch; seg = a[rate * ch: 4 * rate * ch]; m = len(seg) // ch
fr = []
for c in range(ch):
    x = seg[c::ch]; peak = max(abs(v) for v in x)
    fr.append('-' if peak < 1e-4 else str(round(sum(1 for i in range(1, m) if (x[i-1] < 0) != (x[i] < 0)) / 2 / (m / rate), -1)))
print(n, ' '.join(fr))
EOF
cat > "$W/tone.py" <<'EOF'
# level (dB) at given frequencies summed over the front channels (FL FR FC) of a 6-ch f32le dump
import sys, math
from array import array
a = array('f'); a.frombytes(open(sys.argv[1], 'rb').read()); ch = 6; rate = 48000
seg = a[2 * rate * ch: 4 * rate * ch]; m = len(seg) // ch
out = []
for f in map(float, sys.argv[2:]):
    w = 2 * math.pi * f / rate; e = 0.0
    for c in (0, 1, 2):
        x = seg[c::ch]
        re = math.fsum(x[i] * math.cos(w * i) for i in range(m)); im = math.fsum(x[i] * math.sin(w * i) for i in range(m))
        e += (re * re + im * im) / (m * m)
    out.append('%.2f' % (10 * math.log10(e) if e > 0 else -150))
print(' '.join(out))
EOF

echo "multichannel PCM (--pcm: no encoding, the device's layout)"
run "$W/centre.flac" "$W/pc.raw" --pcm && {
    r=$("$PY" -I "$W/levels.py" "$W/pc.raw" centre)
    case "$r" in ok*) pass "pcm upmix centred: ${r#ok }";; *) fail "pcm upmix centred: ${r#BAD }";; esac
}
run "$W/left.flac" "$W/pl.raw" --pcm && {
    r=$("$PY" -I "$W/levels.py" "$W/pl.raw" left)
    case "$r" in ok*) pass "pcm upmix hard left: ${r#ok }";; *) fail "pcm upmix hard left: ${r#BAD }";; esac
}
run "$W/anti.flac" "$W/pa.raw" --pcm && {
    r=$("$PY" -I "$W/levels.py" "$W/pa.raw" anti)
    case "$r" in ok*) pass "pcm upmix ambience: ${r#ok }";; *) fail "pcm upmix ambience: ${r#BAD }";; esac
}
run "$W/six.flac" "$W/p6.raw" --pcm && {
    r=$("$PY" -I "$W/rawfreqs.py" "$W/p6.raw" 6)
    [ "${r#* }" = "300.0 500.0 700.0 60.0 1100.0 1300.0" ] && pass "pcm 5.1 input: channels in place (${r#* })" || fail "pcm 5.1 input: '$r'"
    "$PY" -c "import sys; sys.exit(0 if abs(int(sys.argv[1]) - 288000) <= 1536 else 1)" "${r%% *}" \
        && pass "pcm 5.1 input: ${r%% *} frames (6 s at 48 kHz)" || fail "pcm 5.1 input: ${r%% *} frames, want ~288000"
}
gen "0.25*sin(2*PI*440*t)|0.25*sin(2*PI*660*t)" lr 48000
run "$W/lr.flac" "$W/p2.raw" --pcm --no-upmix && {
    r=$("$PY" -I "$W/rawfreqs.py" "$W/p2.raw" 2)
    [ "${r#* }" = "440.0 660.0" ] && pass "pcm upmix off: stereo, L/R in place (${r#* })" || fail "pcm upmix off: '$r'"
}

echo "tone: bass and treble shelves (front channels, PCM so no codec in the way)"
gen "0.1*sin(2*PI*60*t)+0.1*sin(2*PI*1000*t)+0.1*sin(2*PI*12000*t)|0.1*sin(2*PI*60*t)+0.1*sin(2*PI*1000*t)+0.1*sin(2*PI*12000*t)" tones 48000
run "$W/tones.flac" "$W/t0.raw" --pcm --upmix-set lfe=-40 && t0=$("$PY" -I "$W/tone.py" "$W/t0.raw" 60 1000 12000)
run "$W/tones.flac" "$W/tt.raw" --pcm --upmix-set lfe=-40,treble=6 && tt=$("$PY" -I "$W/tone.py" "$W/tt.raw" 60 1000 12000)
run "$W/tones.flac" "$W/tb.raw" --pcm --upmix-set lfe=-40,bass=6 && tb=$("$PY" -I "$W/tone.py" "$W/tb.raw" 60 1000 12000)
run "$W/tones.flac" "$W/tl.raw" --pcm --upmix-set lfe=-40,bass=9,treble=9 && tl=$("$PY" -I "$W/tone.py" "$W/tl.raw" 60 1000 12000)
tonecheck() { # name "levels" lo,hi per frequency (dB change from no boost)
    local r; r=$("$PY" -c '
import sys
b = [float(v) for v in sys.argv[1].split()]; x = [float(v) for v in sys.argv[2].split()]
d = [xi - bi for xi, bi in zip(x, b)]
ok = all(lo <= di <= hi for di, (lo, hi) in zip(d, [tuple(map(float, r.split(","))) for r in sys.argv[3:]]))
print(("ok " if ok else "BAD ") + "60 Hz %+.1f, 1 kHz %+.1f, 12 kHz %+.1f dB" % tuple(d))' "$t0" "$2" $3)
    case "$r" in ok*) pass "$1: ${r#ok }";; *) fail "$1: ${r#BAD }";; esac
}
[ -n "${t0:-}" ] && {
    [ -n "${tt:-}" ] && tonecheck "treble +6 dB" "$tt" "-0.7,0.7 -0.7,0.9 5.0,6.8"
    [ -n "${tb:-}" ] && tonecheck "bass +6 dB" "$tb" "5.0,6.8 -0.7,0.7 -0.7,0.7"
    [ -n "${tl:-}" ] && tonecheck "loudness (bass +9, treble +9)" "$tl" "8.0,10.0 -1.0,1.0 8.0,10.0"
}

echo "tone without the upmix: native 5.1 and plain stereo (PCM), and encoded"
cat > "$W/tonen.py" <<'EOF'
# level (dB) at given frequencies summed over some channels of an f32le dump, seconds 2..4
import sys, math
from array import array
a = array('f'); a.frombytes(open(sys.argv[1], 'rb').read()); ch = int(sys.argv[2]); rate = 48000
chans = [int(c) for c in sys.argv[3].split(',')]
seg = a[2 * rate * ch: 4 * rate * ch]; m = len(seg) // ch
out = []
for f in map(float, sys.argv[4:]):
    w = 2 * math.pi * f / rate; e = 0.0
    for c in chans:
        x = seg[c::ch]
        re = math.fsum(x[i] * math.cos(w * i) for i in range(m)); im = math.fsum(x[i] * math.sin(w * i) for i in range(m))
        e += (re * re + im * im) / (m * m)
    out.append('%.2f' % (10 * math.log10(e) if e > 0 else -150))
print(' '.join(out))
EOF
TN="0.1*sin(2*PI*60*t)+0.1*sin(2*PI*1000*t)+0.1*sin(2*PI*12000*t)"
ff -f lavfi -i "aevalsrc=$TN|$TN|$TN|$TN|$TN|$TN:s=48000:d=6:c=5.1(side)" -t 6 -c:a flac "$W/tn6.flac"
tone_set() { # label source args channel-count channels
    local base t b l
    run "$2" "$W/q0.raw" $3 && base=$("$PY" -I "$W/tonen.py" "$W/q0.raw" $4 $5 60 1000 12000) || return
    run "$2" "$W/qt.raw" $3 --upmix-set treble=6 && t=$("$PY" -I "$W/tonen.py" "$W/qt.raw" $4 $5 60 1000 12000)
    run "$2" "$W/qb.raw" $3 --upmix-set bass=6 && b=$("$PY" -I "$W/tonen.py" "$W/qb.raw" $4 $5 60 1000 12000)
    run "$2" "$W/ql.raw" $3 --upmix-set bass=9,treble=9 && l=$("$PY" -I "$W/tonen.py" "$W/ql.raw" $4 $5 60 1000 12000)
    t0=$base
    [ -n "${t:-}" ] && tonecheck "$1 treble +6 dB" "$t" "-0.7,0.7 -0.7,0.9 5.0,6.8"
    [ -n "${b:-}" ] && tonecheck "$1 bass +6 dB" "$b" "5.0,6.8 -0.7,0.7 -0.7,0.7"
    [ -n "${l:-}" ] && tonecheck "$1 loudness (bass +9, treble +9)" "$l" "8.0,10.0 -1.0,1.0 8.0,10.0"
}
tone_set "native 5.1" "$W/tn6.flac" "--pcm" 6 0,1,2,4,5
tone_set "stereo, upmix off" "$W/tones.flac" "--pcm --no-upmix" 2 0,1
# flat tone: no graph, bit-identical; boosted: no samples lost or added, channels in place
run "$W/six.flac" "$W/f0.raw" --pcm && run "$W/six.flac" "$W/f1.raw" --pcm --upmix-set bass=0,treble=0 && {
    cmp -s "$W/f0.raw" "$W/f1.raw" && pass "tone flat: bit-identical to no tone" || fail "tone flat: output differs"
}
run "$W/six.flac" "$W/f2.raw" --pcm --upmix-set treble=6 && {
    [ "$(wc -c < "$W/f2.raw")" = "$(wc -c < "$W/f0.raw")" ] && pass "tone: no samples lost or added ($(wc -c < "$W/f2.raw") bytes)" \
        || fail "tone: $(wc -c < "$W/f2.raw") bytes, want $(wc -c < "$W/f0.raw")"
    r=$("$PY" -I "$W/rawfreqs.py" "$W/f2.raw" 6)
    [ "${r#* }" = "300.0 500.0 700.0 60.0 1100.0 1300.0" ] && pass "tone 5.1: channels in place (${r#* })" || fail "tone 5.1: '$r'"
}
# encoded: 5.1 AC3 (planar float) and DTS (packed s32), and stereo AC3 with the upmix off
run "$W/six.flac" "$W/e1.spdif" --upmix-set bass=6,treble=6 && stream "tone 5.1 ac3" "$W/e1.spdif" ac3 "5.1(side)" 6144
run "$W/six.flac" "$W/e2.spdif" --dts --upmix-set bass=6,treble=6 && stream "tone 5.1 dts" "$W/e2.spdif" dts "5.1(side)" 2048
run "$W/centre.flac" "$W/e3.spdif" --no-upmix --upmix-set bass=6,treble=6 && stream "tone stereo ac3" "$W/e3.spdif" ac3 stereo 6144
# switched on and off while it runs (real-time input): no click (a filter started cold on a
# running signal spiked to 2.5x), nothing lost
ff -f lavfi -i "aevalsrc=0.25*sin(2*PI*1000*t)|0.25*sin(2*PI*1000*t):s=48000:d=6" -t 6 -c:a flac "$W/tg.flac"
run "$W/tg.flac" "$W/tg.raw" --realtime --pcm --no-upmix --upmix-set bass=12,treble=12 --tone-toggle-ms 150 && {
    r=$("$PY" -I -c "
import sys
from array import array
a = array('f'); a.frombytes(open(sys.argv[1], 'rb').read()); x = a[0::2]
step = max(abs(x[i] - x[i - 1]) for i in range(1000, len(x)))
print(('ok ' if abs(len(x) - 288000) <= 1536 and step < 0.05 else 'BAD ') + '%d frames, largest step %.4f (the sine alone: 0.033)' % (len(x), step))" "$W/tg.raw")
    n=$(grep -o '^tone toggles: [0-9]*' "$W/log.txt" | grep -o '[0-9]*$')
    case "$r" in ok*) pass "tone toggled live ${n}x: ${r#ok }";; *) fail "tone toggled live: ${r#BAD }";; esac
}

echo "more than 2 input channels pass through (upmix on, so nothing is upmixed), speakers kept"
# a file with one tone per channel: layout name, then channel=frequency pairs
tones() { # name layout CH=f ...
    local name=$1 lay=$2; shift 2
    local args=() map="" i=0
    for cf in "$@"; do args+=(-f lavfi -i "sine=f=${cf#*=}:sample_rate=48000"); map+="${map:+|}$i.0-${cf%%=*}"; i=$((i + 1)); done
    ff "${args[@]}" -filter_complex "join=inputs=$i:channel_layout=$lay:map=$map[a]" -map "[a]" -t 6 -c:a flac "$W/$name.flac"
}
passthru() { # label file want(6 ch FL FR FC LFE SL SR) [extra args]
    local label=$1 f=$2 want=$3; shift 3
    run "$W/$f.flac" "$W/pt.raw" --pcm "$@" && {
        local r; r=$("$PY" -I "$W/rawfreqs.py" "$W/pt.raw" 6)
        [ "${r#* }" = "$want" ] && pass "$label: ${r#* }" || fail "$label: '${r#* }', want '$want'"
    }
}
tones three 3.0 FL=300 FR=500 FC=700
tones quad quad FL=300 FR=500 BL=1100 BR=1300
tones five "5.0(side)" FL=300 FR=500 FC=700 SL=1100 SR=1300
tones sevenone 7.1 FL=300 FR=500 FC=700 LFE=60 BL=1500 BR=1700 SL=1100 SR=1300
passthru "3.0 input (pcm)" three "300.0 500.0 700.0 - - -"
passthru "quad input, back pair to the surrounds (pcm)" quad "300.0 500.0 - - 1100.0 1300.0"
passthru "5.0 input (pcm)" five "300.0 500.0 700.0 - 1100.0 1300.0"
passthru "5.0 input with tone (pcm)" five "300.0 500.0 700.0 - 1100.0 1300.0" --upmix-set bass=6,treble=6
run "$W/sevenone.flac" "$W/p8.raw" --pcm && {
    r=$("$PY" -I "$W/rawfreqs.py" "$W/p8.raw" 8)
    [ "${r#* }" = "300.0 500.0 700.0 60.0 1500.0 1700.0 1100.0 1300.0" ] && pass "7.1 input stays 7.1 (pcm): ${r#* }" || fail "7.1 input (pcm): '${r#* }'"
}
run "$W/quad.flac" "$W/q.spdif" && {
    stream "quad input (ac3)" "$W/q.spdif" ac3 "5.1(side)" 6144
    ff -f spdif -i "$W/q.spdif" -f f32le "$W/q.raw"
    r=$("$PY" -I "$W/rawfreqs.py" "$W/q.raw" 6)
    [ "${r#* }" = "300.0 500.0 - - 1100.0 1300.0" ] && pass "quad input (ac3): ${r#* }" || fail "quad input (ac3): '${r#* }'"
}

echo "auto: a 5.1 cable carrying stereo, then 5.1 (stereo is upmixed, 5.1 passes through)"
# 13 s of 5.1: one tone per channel (0-4 s), stereo only (a centred 1 kHz in FL/FR, the rest
# exact zeros, as Windows fills a 5.1 cable for a stereo app: 4-9 s), then 5.1 again (9-13 s)
B='between(t\,4\,9)'
ff -f lavfi -i "aevalsrc=0.2*if($B\,sin(2*PI*1000*t)\,sin(2*PI*300*t))|0.2*if($B\,sin(2*PI*1000*t)\,sin(2*PI*500*t))|0.2*sin(2*PI*700*t)*(1-$B)|0.2*sin(2*PI*60*t)*(1-$B)|0.2*sin(2*PI*1100*t)*(1-$B)|0.2*sin(2*PI*1300*t)*(1-$B):s=48000:d=13:c=5.1(side)" \
   -t 13 -c:a flac -sample_fmt s32 "$W/auto.flac"
cat > "$W/seg.py" <<'EOF'
# f32le, N channels, seconds t0..t1: the zero-crossing frequency of each channel ('-' = silent)
import sys
from array import array
a = array('f'); a.frombytes(open(sys.argv[1], 'rb').read()); ch = int(sys.argv[2]); rate = 48000
t0, t1 = float(sys.argv[3]), float(sys.argv[4])
seg = a[int(t0 * rate) * ch: int(t1 * rate) * ch]; m = len(seg) // ch
fr = []
for c in range(ch):
    x = seg[c::ch]; peak = max(abs(v) for v in x)
    fr.append('-' if peak < 1e-3 else str(round(sum(1 for i in range(1, m) if (x[i-1] < 0) != (x[i] < 0)) / 2 / (m / rate), -1)))
print(' '.join(fr))
EOF
cat > "$W/whole.py" <<'EOF'
# f32le, N channels: frame count and peak
import sys
from array import array
a = array('f'); a.frombytes(open(sys.argv[1], 'rb').read()); ch = int(sys.argv[2])
print(len(a) // ch, '%.3f' % max(abs(v) for v in a))
EOF
SIX="300.0 500.0 700.0 60.0 1100.0 1300.0"
auto_check() { # label raw
    local r
    r=$("$PY" -I "$W/seg.py" "$2" 6 1 3.5);    [ "$r" = "$SIX" ] && pass "$1, 5.1 at the start: passed through ($r)" || fail "$1, 5.1 at the start: '$r'"
    r=$("$PY" -I "$W/seg.py" "$2" 6 4.5 6.5);  [ "$r" = "1000.0 1000.0 - - - -" ] && pass "$1, stereo, first 3 s: still passed through ($r)" || fail "$1, stereo, first 3 s: '$r'"
    r=$("$PY" -I "$W/seg.py" "$2" 6 7.6 8.9);  case "$r" in "1000.0 1000.0 1000.0 "*) pass "$1, stereo after 3 s: upmixed, centre speaker on ($r)";; *) fail "$1, stereo after 3 s: not upmixed '$r'";; esac
    r=$("$PY" -I "$W/seg.py" "$2" 6 9.2 12.5); [ "$r" = "$SIX" ] && pass "$1, 5.1 again: passed through at once ($r)" || fail "$1, 5.1 again: '$r'"
    r=$("$PY" -I "$W/whole.py" "$2" 6)
    "$PY" -c "import sys; n, pk = int(sys.argv[1]), float(sys.argv[2]); sys.exit(0 if abs(n - 624000) <= 1536 * 2 and pk <= 1.0 else 1)" ${r} \
        && pass "$1: $r (frames, peak)" || fail "$1: frames/peak $r, want ~624000 and <= 1"
}
run "$W/auto.flac" "$W/au.raw" --pcm && auto_check "auto pcm" "$W/au.raw"
run "$W/auto.flac" "$W/au.spdif" && { ff -f spdif -i "$W/au.spdif" -f f32le "$W/au3.raw"; auto_check "auto ac3" "$W/au3.raw"; }
run "$W/auto.flac" "$W/ad.spdif" --dts && { ff -f spdif -i "$W/ad.spdif" -f f32le "$W/aud.raw"; auto_check "auto dts" "$W/aud.raw"; }
run "$W/auto.flac" "$W/at.raw" --pcm --upmix-set bass=6,treble=6 && auto_check "auto pcm with tone" "$W/at.raw"
ff -f lavfi -i "aevalsrc=0.2*sin(2*PI*1000*t)|0.2*sin(2*PI*1000*t)|0|0|0|0:s=48000:d=6:c=5.1(side)" -t 6 -c:a flac -sample_fmt s32 "$W/auto2.flac"
run "$W/auto2.flac" "$W/a2.raw" --pcm && {
    r=$("$PY" -I "$W/seg.py" "$W/a2.raw" 6 0.2 2)
    case "$r" in "1000.0 1000.0 1000.0 "*) pass "auto: stereo from the start is upmixed at once ($r)";; *) fail "auto: stereo from the start: '$r'";; esac
}

echo "auto stream: upmix off, a 5.1 cable: stereo goes out as a 2.0 stream (the amp can upmix it)"
cat > "$W/frames.py" <<'EOF'
# ffprobe frame lines "pts_time,channels": the channel count in each time span, '?' if mixed
import sys
spans = [tuple(map(float, a.split('-'))) for a in sys.argv[2:]]
fr = []
for line in open(sys.argv[1]):
    t, c = line.strip().split(',')[:2]
    fr.append((float(t), int(c)))
out = []
for t0, t1 in spans:
    cs = set(c for t, c in fr if t0 <= t < t1)
    out.append(str(cs.pop()) if len(cs) == 1 else '?' + ''.join(sorted(map(str, cs))))
print(' '.join(out))
EOF
for codec in ac3 dts; do
    flag=""; [ $codec = dts ] && flag=--dts
    run "$W/auto.flac" "$W/ae.spdif" $flag --no-upmix && {
        ffprobe -v error -f spdif -i "$W/ae.spdif" -show_entries frame=pts_time,channels -of csv=p=0 > "$W/fr.txt"
        r=$("$PY" -I "$W/frames.py" "$W/fr.txt" 0.2-3.9 4.5-6.5 7.2-8.9 9.1-12.8)
        [ "$r" = "6 6 2 6" ] && pass "$codec auto stream: 5.1, 5.1 (first 3 s of stereo), 2.0, 5.1 again ($r)" || fail "$codec auto stream: channels per span '$r', want '6 6 2 6'"
        ff -f spdif -i "$W/ae.spdif" -f f32le -ac 6 "$W/ae.raw"
        r=$("$PY" -I "$W/seg.py" "$W/ae.raw" 6 1 3.5);   [ "$r" = "$SIX" ] && pass "$codec auto stream: 5.1 in place ($r)" || fail "$codec auto stream, 5.1: '$r'"
        r=$("$PY" -I "$W/seg.py" "$W/ae.raw" 6 7.6 8.9); case "$r" in "1000.0 1000.0 "*) pass "$codec auto stream: 2.0 carries the stereo ($r)";; *) fail "$codec auto stream, 2.0: '$r'";; esac
        r=$("$PY" -I "$W/seg.py" "$W/ae.raw" 6 9.2 12.5); [ "$r" = "$SIX" ] && pass "$codec auto stream: 5.1 again at once ($r)" || fail "$codec auto stream, 5.1 again: '$r'"
    }
done
run "$W/auto2.flac" "$W/a2.spdif" --no-upmix && {
    ffprobe -v error -f spdif -i "$W/a2.spdif" -show_entries frame=pts_time,channels -of csv=p=0 > "$W/fr.txt"
    r=$("$PY" -I "$W/frames.py" "$W/fr.txt" 0-5.9)
    [ "$r" = "2" ] && pass "auto stream: stereo from the start is 2.0 throughout ($r)" || fail "auto stream, stereo from the start: '$r'"
}
run "$W/auto.flac" "$W/au5.spdif" && {
    ffprobe -v error -f spdif -i "$W/au5.spdif" -show_entries frame=pts_time,channels -of csv=p=0 > "$W/fr.txt"
    r=$("$PY" -I "$W/frames.py" "$W/fr.txt" 0-12.8)
    [ "$r" = "6" ] && pass "upmix on: the stream stays 5.1 throughout ($r)" || fail "upmix on, stream: '$r'"
}

echo "auto stream, PCM: upmix off, a 5.1 cable: the output reopens at 2 channels while stereo plays"
cat > "$W/pcmsegs.py" <<'EOF'
# a PCM dump whose channel count changes ("pcm format: N ch from output frame F" in the log):
# the formats with the time they start, then the zero-crossing frequencies in given time spans
import sys, re
from array import array
a = array('f'); a.frombytes(open(sys.argv[1], 'rb').read()); rate = 48000
sw = [(0, int(sys.argv[3]))] + [(int(f), int(c)) for c, f in re.findall(r'pcm format: (\d+) ch from output frame (\d+)', open(sys.argv[2]).read())]
segs, pos = [], 0   # (start frame, channels, sample offset)
for i, (f, c) in enumerate(sw):
    segs.append((f, c, pos))
    end = sw[i + 1][0] if i + 1 < len(sw) else (len(a) - pos) // c + f
    pos += (end - f) * c
print('formats:', ' '.join('%dch@%.2fs' % (c, f / rate) for f, c, _ in segs))
for span in sys.argv[4:]:
    t0, t1 = map(float, span.split('-')); f0, f1 = int(t0 * rate), int(t1 * rate)
    seg = [s for s in segs if s[0] <= f0][-1]; f, c, off = seg
    x = a[off + (f0 - f) * c: off + (f1 - f) * c]; m = len(x) // c
    fr = []
    for k in range(c):
        y = x[k::c]; pk = max(abs(v) for v in y)
        fr.append('-' if pk < 1e-3 else str(round(sum(1 for i in range(1, m) if (y[i-1] < 0) != (y[i] < 0)) / 2 / (m / rate), -1)))
    print('%s: %s' % (span, ' '.join(fr)))
EOF
run "$W/auto.flac" "$W/ap.raw" --pcm --no-upmix && {
    "$PY" -I "$W/pcmsegs.py" "$W/ap.raw" "$W/log.txt" 6 1-3.5 7.5-8.6 9.6-12.5 > "$W/ps.txt"
    f=$(sed -n 1p "$W/ps.txt")
    "$PY" -c "import re, sys; m = re.fullmatch(r'formats: 6ch@0.00s 2ch@([\d.]+)s 6ch@([\d.]+)s', sys.argv[1]); sys.exit(0 if m and 6.9 < float(m[1]) < 7.2 and 8.95 < float(m[2]) < 9.1 else 1)" "$f" \
        && pass "pcm auto stream: $f (2 channels 3 s into the stereo, 6 as soon as 5.1 is back)" || fail "pcm auto stream: $f, want 6ch, then 2ch at ~7 s, 6ch at ~9 s"
    r=$(sed -n 2p "$W/ps.txt"); [ "${r#*: }" = "$SIX" ] && pass "pcm auto stream: 5.1 in place ($r)" || fail "pcm auto stream, 5.1: '$r'"
    r=$(sed -n 3p "$W/ps.txt"); [ "${r#*: }" = "1000.0 1000.0" ] && pass "pcm auto stream: stereo as 2 channels ($r)" || fail "pcm auto stream, stereo: '$r'"
    r=$(sed -n 4p "$W/ps.txt"); [ "${r#*: }" = "$SIX" ] && pass "pcm auto stream: 5.1 again ($r)" || fail "pcm auto stream, 5.1 again: '$r'"
}
run "$W/auto2.flac" "$W/ap2.raw" --pcm --no-upmix && {
    "$PY" -I "$W/pcmsegs.py" "$W/ap2.raw" "$W/log.txt" 6 1-5 > "$W/ps.txt"
    f=$(sed -n 1p "$W/ps.txt"); r=$(sed -n 2p "$W/ps.txt")
    case "$f" in "formats: 6ch@0.00s 2ch@0."*"s") [ "${r#*: }" = "1000.0 1000.0" ] && pass "pcm auto stream: stereo from the start goes to 2 channels at once ($f; $r)" || fail "pcm, stereo from the start: '$r'";;
        *) fail "pcm, stereo from the start: $f";; esac
}
run "$W/auto.flac" "$W/apu.raw" --pcm && ! grep -q "pcm format" "$W/log.txt" && pass "pcm, upmix on: the output never reopens" || fail "pcm, upmix on: reopened"

[ $FAIL = 0 ] && echo "ALL PASSED" || echo "SOME CHECKS FAILED"
exit $FAIL

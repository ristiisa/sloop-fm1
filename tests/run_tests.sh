#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
# Host tests (no hardware). Run from the repo root after ./build.sh:
#   tests/run_tests.sh
# The tests build and run in parallel, JOBS at a time (default: the CPU count), their output printed
# in this order once all are done; the ones that time themselves (costs, the CPU budget) run after
# them, one at a time. JOBS=1: one at a time throughout.
#
# Regression suite (tests/regress.c, tests/target_budget.py; details at the top of regress.c):
#   golden renders  every engine x preset, the drum kit, voice modes, FX sends, a 4-track mix: one hash
#                   each in tests/golden.txt. A change of the sound fails with the list of renders.
#   health          clipping, DC, peak level, voices free after the release, silence at the end.
#   CPU             cost / sample per preset and mix, relative to the idle + drums mix (tests/cpu_baseline.txt,
#                   +25 %; counted by the kernel or under callgrind, else timed at +35 %), ns printed;
#                   target: loop instructions of the render functions in build/felucca.dis
#                   (tests/target_budget.txt, +10 %; exact, static).
#   voices          the budget of 8, steal fades, MONO / LEGATO / UNISON keep their note, the VOICE cap,
#                   no hanging notes on any MIDI / key routing.
# FM6 (tests/fm6_test.c): the 6-operator FM engine (firmware/src/eng_fm6.c, fm6_core.c, fm6_bank.c): the 32
#                   algorithms' carriers, the operator envelopes ending the voice, retrigger, DC / clipping, the
#                   eight macros, the patch formats (packed, SysEx), the 6-voice cap, the patch bank on a
#                   simulated NOR; demos in build/fm6_demo/.
# After an intended change of the sound: GOLDEN_UPDATE=1 sh tests/run_tests.sh, review the diff
# of tests/golden.txt, commit it with the change. After an intended change of the cost (or a new
# compiler): BUDGET_UPDATE=1 (rewrites cpu_baseline.txt and target_budget.txt). VERBOSE=1: every render.
set -e
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"
cd "$(dirname "$0")/.."
OUT=build/host
LOG=$OUT/log
rm -rf "$LOG"
mkdir -p "$OUT" "$LOG" "$OUT/ub" build/tracks_demo build/slicer_demo build/color_demo build/fm6_demo
CC="${CC:-cc} -O1 -Wall -Wno-unused-function"
SIM="-O2 -w -Ibuild/gen -Ifirmware/src -Ifirmware/hal"   # the tests that build the firmware's sources
JOBS="${JOBS:-$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 4)}"
fail=0
n=0
run() { echo "== $1"; shift; "$@" || fail=1; }

# job "what it tests" command...: in the background, JOBS at a time; its output and status in $LOG
job() {
    n=$((n + 1))
    k=$(printf %03d $n)
    echo "$1" > "$LOG/$k.name"
    shift
    while [ $((n - 1 - $(ls "$LOG" | grep -c '\.rc$'))) -ge "$JOBS" ]; do sleep 0.05; done
    ( if "$@" > "$LOG/$k.out" 2>&1; then echo 0; else echo 1; fi > "$LOG/$k.rc" ) &
}
# t name flags args...: build tests/name.c into $OUT/name with $CC flags, then run it with args
t() { tn=$1; tf=$2; shift 2; $CC $tf -o "$OUT/$tn" "tests/$tn.c" -lm && "$OUT/$tn" "$@"; }

[ -f build/felucca.fwsc ] || { echo "run ./build.sh first"; exit 1; }
# the generated headers the FM6 engine needs (tools/build.py generate() makes them too; no Pillow needed)
mkdir -p build/gen
[ build/gen/felucca_tables.h -nt tools/gen_tables.py ] || python3 tools/gen_tables.py build/gen/felucca_tables.h
[ build/gen/felucca_fm6.h -nt tools/gen_fm6_patches.py ] || python3 tools/gen_fm6_patches.py build/gen/felucca_fm6.h

# ---- built first, used by several tests
$CC $SIM -o "$OUT/hostsim" tests/hostsim.c -lm
head -c 200000 build/felucca.bin > "$OUT/old_app.bin"
python3 tools/fm1pkg_make.py "$OUT/old_app.bin" build/loader/ota.bin "$OUT/old.fwsc" >/dev/null
HALF=$(sed -n 's/^#define HALF_FRAMES \([0-9]*\).*/\1/p' firmware/src/core.h)
$CC -DT_CDC=1 -DHALF_FRAMES=$HALF -o "$OUT/uac_test" tests/uac_test.c
$CC -DT_CDC=0 -DHALF_FRAMES=$HALF -o "$OUT/uac_test_nocdc" tests/uac_test.c
$CC -DT_CDC=2 -DHALF_FRAMES=$HALF -o "$OUT/uac_test_seroff" tests/uac_test.c
uac_same() { [ "$(UAC_DUMP=1 "$OUT/uac_test_seroff" | tail -n 2)" = "$(UAC_DUMP=1 "$OUT/uac_test_nocdc" | tail -n 2)" ] && echo same; }
uac_in_app() { ${CC%% *} -E -Ibuild/gen -Ifirmware/hal -Ifirmware/src firmware/src/felucca.c 2>/dev/null | grep -q uac_service; }
# no divide by 0 (the FM-1 runs with the div0 trap off, hal/fm1_irq.h: a real one would give a wrong value
# silently): the UI fuzz, the sequencer, the projects and a minute of random live use, with UBSan
UBSAN="${CC_UB:-cc} -O1 -w -fsanitize=integer-divide-by-zero -fno-sanitize-recover=integer-divide-by-zero -Ibuild/gen -Ifirmware/src -Ifirmware/hal"
ub() {
    $UBSAN -o "$OUT/ui_pages_ub" tests/ui_pages_test.c -lm && $UBSAN -o "$OUT/seq2_ub" tests/seq2_test.c -lm &&
        $UBSAN -o "$OUT/project_ub" tests/project_test.c -lm && $UBSAN -o "$OUT/soak_ub" tests/soak_test.c -lm &&
        "$OUT/ui_pages_ub" "$OUT/ub" >/dev/null && "$OUT/seq2_ub" >/dev/null && "$OUT/project_ub" >/dev/null &&
        "$OUT/soak_ub" 1 >/dev/null && echo 'no divide by zero'
}
# the stress test under AddressSanitizer + UBSan: any read or write out of bounds stops it (left shifts of negative
# values and the FM6 phase's wrap-around are left out: the DSP's two's complement idioms, as every compiler builds them)
ASAN="${CC_UB:-cc} -O1 -g -w -fsanitize=address,undefined -fno-sanitize=shift-base,signed-integer-overflow -fno-sanitize-recover=all -Ibuild/gen -Ifirmware/src -Ifirmware/hal"
asan() {
    if $ASAN -o "$OUT/stress_asan" tests/stress_test.c -lm 2>/dev/null; then
        "$OUT/stress_asan" "${STRESS_ASAN_FRAMES:-15000}" 7
    else
        echo "(stress under ASan: this compiler has no AddressSanitizer, skipped)"
    fi
}
web() { if command -v node >/dev/null 2>&1; then node web/test_web.mjs; else echo "skip web tests (no node)"; fi; }

# ---- in parallel
job "flash storage (A/B, torn writes)" t storage_test ""
job "application USB recovery and boot-loop guard" t recovery_test ""
job "song order, timing, repeats and missing scenes" t arranger_test ""
job "song: four simultaneous tracks, scene transition and stop" t song_audio_test "$SIM" "$OUT/song-demo.wav"
job "song screen: commands, load (OCT+ twice), display bounds" t song_ui_test "$SIM" "$OUT/song-screen.ppm"
job "drum lanes, kit audio, metronome, record arm, free take" t studio_drums_test "$SIM" "$OUT/drum-styles.wav"
job "sequencer 2.0: no drift, ratchets, roll, erase / undo, ghost / hard, chords, mute / solo, nudge, locks" t seq2_test "$SIM"
job "user drum kits (KIT USR1..USR3): a user slot's sounds on the drum lanes" t userkit_test "$SIM"
job "punch-in FX: 16 effects, bounded, dry after release, FX-held keys, LATCH" t punch_test "$SIM" "$OUT/punch-fx.wav"
job "live UI: pages, layers (punch, steps, erase, roll, key, mix), holds, drums, REC, fuzz" t ui_pages_test "$SIM" "$OUT"
job "text: both font sizes pixel-exact (every glyph, clipped, offset), the cost of a line" t font_test "-O2 -w -Ibuild/gen"
job "no divide by zero (UBSan): UI fuzz, sequencer, projects, a minute of live use" ub
job "soak: ${SOAK_MIN:-10} minutes of random live use (bounded, no hanging voices, idle after stop)" t soak_test "$SIM" "${SOAK_MIN:-10}"
job "stress (2.4): FONT_L = FONT_S at 2x, hard random use of every 2.4 addition, clean stop" t stress_test "$SIM" "${STRESS_FRAMES:-40000}"
job "stress under ASan + UBSan (no access out of bounds)" asan
job "user presets (UP_PUT parser, bank round trip, versions)" t upreset_test ""
job "TRS MIDI parser" t midi_uart_test ""
job "knobs: one click = one step (slow, fast, pauses, bounce)" t encoder_test "-Ifirmware/hal"
job "USB audio input: descriptors (with CDC), ring and packets" "$OUT/uac_test"
job "USB audio input: descriptors (without CDC), ring and packets" "$OUT/uac_test_nocdc"
job "USB audio input: descriptors (CDC built in, menu USB SERIAL OFF), ring and packets" "$OUT/uac_test_seroff"
job "USB SERIAL OFF: the descriptors of a build without CDC, byte for byte" uac_same
job "USB audio input: built into the firmware (FELUCCA_UAC set before usb.c)" uac_in_app
job "M-UPGRADE entry" t ota_test "" build/felucca.fwsc
job "update loader: other app -> this build" t ldr_test "" "$OUT/old.fwsc" build/felucca.fwsc
job "scales: white-key mapping and note lifecycle" t scale_test "$SIM"
job "arp: the modes on C E G B, accents, HITS of STEPS, ratchets, ROT, SYNC, RHYM, DEJA, SHIFT, recording" t arp_test "$SIM"
job "mutate: invariants over thousands of passes, kicks on the beats, a little a pass, exact undo" t mutate_test "$SIM"
job "turing: TURN 0 / 100 %, the rates, scale and register, structure, kicks on the beats, recording, undo, locks" t turing_test "$SIM" "$OUT"
job "evolve: EVOL / BACK on their bars, only playing, muted / recording kept, new starts, undo, the JAM page, invariants" t evolve_test "$SIM" "$OUT"
job "prog: a chord a bar, every progression, exact in every scale, arp, drums, OFF, sections, held notes, the JAM page" t prog_test "$SIM" "$OUT"
job "dice: every style, thousands of rolls: invariants, signatures, scale and register, turned back exactly, undo, the gesture" t dice_test "$SIM" "$OUT"
job "grids: the map as Grids, density, levels, ratchets, chaos, lanes / conditions / locks kept, the MAP page, undo" t grids_test "$SIM" "$OUT"
job "step conditions: chance, a:b, FIRST, FILL, AFILL (by the bars), drums and synths, shift / x2 / undo, recording" t cond_test "$SIM"
job "parameter locks (2.4's) with 2.5's: COLOR / SLICER lock, the gestures, follow their steps (shift, x2, undo, DICE), saved" t locks_test "$SIM" "$OUT"
job "MIDI expression: bend and its range, mod wheel, sustain, CC120 / 121 / 123, no hanging note, drums, recording" t midi_expr_test "$SIM"
job "DSP render (ANALOG preset 0)" "$OUT/hostsim" 0 0 1 "$OUT/render.wav"
job "project formats (FUN4 / FUN3 / FUN2 / FUN1 -> FUN5), conditions, locks, the extension record, capture / apply, autosave, backup" t project_test "-w -Ibuild/gen -Ifirmware/src"
job "installer CLI (fm1_install.py) against a simulated FM-1" python3 tests/install_test.py
job "web pages: editor protocol, samples, packages, update protocol" web
# SLICE (tests/slice_test.c) needs a FELUCCA_SLICE=1 build; the engine is not built by default
wait

k=1
while [ $k -le $n ]; do
    f=$LOG/$(printf %03d $k)
    echo "== $(cat "$f.name")"
    cat "$f.out"
    [ "$(cat "$f.rc")" = 0 ] || fail=1
    k=$((k + 1))
done

# ---- one at a time: these time themselves
$CC $SIM -o "$OUT/drumkit_test" tests/drumkit_test.c -lm
run "synthesised drum kits: every kit x sound bounded, audible, finite, levels, cost" "$OUT/drumkit_test" "$OUT/drum-kits.wav" "$OUT/drum-kits.txt"
run "TRACKS: 4-track pattern, live recording (lengths, swing), voice budget, engine switch, cost" env TRACKS=build/tracks_demo "$OUT/hostsim" 0 0 1 "$OUT/tracks.wav"
$CC $SIM -o "$OUT/slicer_test" tests/slicer_test.c -lm
run "SLICER: no clicks, timing, sync with the sequencer, STUT, cost, demos" "$OUT/slicer_test" build/slicer_demo
$CC $SIM -o "$OUT/color_test" tests/color_test.c -lm
run "COLOR: OFF skipped, PHASR notches, WAH envelope, FOLD harmonics, RING sidebands, bounded, release, locks, cost" "$OUT/color_test" build/color_demo
$CC $SIM -o "$OUT/fm6_test" tests/fm6_test.c -lm
run "FM6: algorithms, envelopes, retrigger, DC, clipping, macros, patch formats, voices, the bank, demos" "$OUT/fm6_test" build/fm6_demo
$CC $SIM -o "$OUT/regress" tests/regress.c -lm
# the CPU budget: counted by the kernel on macOS; elsewhere under callgrind when valgrind is there (exact, ~45 s;
# SKIP_CPU_VALGRIND=1 to time instead, which is only a rough check)
if [ -z "$SKIP_CPU_VALGRIND" ] && command -v valgrind >/dev/null 2>&1; then export CPU_VALGRIND=1; fi
run "regression: golden renders, health, voices, CPU budget" "$OUT/regress" tests/golden.txt tests/cpu_baseline.txt
run "regression: target cost of the render loops" python3 tests/target_budget.py \
    build/felucca.dis tests/target_budget.txt

[ $fail -eq 0 ] && echo "ALL HOST TESTS PASSED" || { echo "HOST TESTS FAILED"; exit 1; }

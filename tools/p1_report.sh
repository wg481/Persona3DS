#!/usr/bin/env bash
# One-file report of a Persona PC run, for the maintainer to upload.
#   bash tools/p1_report.sh <run number> [max dispatches] [iso] [input script] [stop vblank]
# Pulls p1-retarget, builds, runs CTest and the PC runner (frames every 60
# vblanks), summarizes the events, disassembles the code before the last
# transfer and converts frames 120 and 300, every distinct screen from frame
# 1800 on (at most 16) and the last frame to PNG. The run also ends at
# [stop vblank] (default 9000, about 150 s of game time), so a game waiting at
# a menu still produces a report. An input
# script (core/include/p3p3ds/input.hpp format, default
# profiles/p1p/input/boot_presses.txt, "none" for no input) is passed as --input. Everything textual goes to
# .tmp/p1/report<N>.txt; the PNGs stay in .tmp/p1/frames<N>/.
set -u
N=${1:?run number}
DISPATCHES=${2:-5000000}
ISO=${3:-$(ls *.iso 2>/dev/null | head -1)}
INPUT=${4:-profiles/p1p/input/boot_presses.txt}   # "none" for no input
[ "$INPUT" = "none" ] && INPUT=""
STOP_VBLANK=${5:-9000}
INPUT_ARGS=()
[ -n "$INPUT" ] && INPUT_ARGS=(--input "$INPUT")
OUT=.tmp/p1
R=$OUT/report$N.txt
mkdir -p "$OUT"
{
  echo "=== git ==="
  git pull origin p1-retarget 2>&1 | tail -2
  git log -1 --oneline
  echo; echo "=== build ==="
  cmake --build build 2>&1 | grep -E " error|FAILED" || echo "build OK"
  echo; echo "=== ctest ==="
  ctest --test-dir build 2>&1 | tail -12
  echo; echo "=== run $N: $ISO, $DISPATCHES dispatches, stop vblank $STOP_VBLANK, input: ${INPUT:-none} ==="
  ./build/p3p_pc_bootstrap --umd "$ISO" --run-until-blocker --max-dispatches "$DISPATCHES" --stop-vblank "$STOP_VBLANK" "${INPUT_ARGS[@]}" \
      --frames-dir "$OUT/frames$N" --frame-every 60 --dump-events "$OUT/run$N.json" > "$OUT/run$N.txt" 2>&1
  grep -aE "Stop Reason|Final Guest PC|Blocker type|Last executed transfer|\[AUDIO\]|\[THREAD|\[INTERPRETER\]" "$OUT/run$N.txt"
  echo; echo "=== event summary ==="
  python .claude/skills/p3p-run-triage/scripts/summarize_events.py "$OUT/run$N.json" --last 25
  FROM=$(grep -aoE "Last executed transfer: 0x[0-9A-Fa-f]+" "$OUT/run$N.txt" | grep -oE "0x[0-9A-Fa-f]+$")
  if [ -n "$FROM" ]; then
    START=$(printf "0x%08X" $(( FROM - 0x40 )))
    echo; echo "=== disassembly from $START (last transfer $FROM) ==="
    ./build/recomp/PSPRecomp/dump_function profiles/p1p/game/eboot.elf "$START" 24
  fi
  echo; echo "=== frames ==="
  LIST=$(python .claude/skills/p3p-run-triage/scripts/frames_to_png.py "$OUT/frames$N")
  echo "$LIST"
  python .claude/skills/p3p-run-triage/scripts/frames_to_png.py "$OUT/frames$N" 120
  python .claude/skills/p3p-run-triage/scripts/frames_to_png.py "$OUT/frames$N" 300
  # Distinct screens after the intro (the list prints one line per change of picture).
  for F in $(echo "$LIST" | grep -oE "frame_[0-9]+\.bmp" | grep -oE "[0-9]+" | sed 's/^0*//' | awk '$1 >= 1800' | head -16); do
    python .claude/skills/p3p-run-triage/scripts/frames_to_png.py "$OUT/frames$N" "$F"
  done
  LAST=$(ls "$OUT/frames$N" | grep -oE "frame_[0-9]+\.bmp" | sort | tail -1 | grep -oE "[0-9]+" | sed 's/^0*//')
  [ -n "$LAST" ] && python .claude/skills/p3p-run-triage/scripts/frames_to_png.py "$OUT/frames$N" "$LAST"
} > "$R" 2>&1
echo "wrote $R"
ls "$OUT/frames$N"/*.png 2>/dev/null

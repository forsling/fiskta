#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<EOF
Usage:
  ./fuzz.sh [--asan] [--cases N] [--seed N] [--minimize]
            [--timeout-ms N] [--max-depth N] [--max-repeat N]
            [--artifacts DIR]
            [--repro-case N]
            [--repro INPUT.bin OPS.txt]
            [--quiet]

Examples:
  ./fuzz.sh --asan --cases 20000 --minimize
  ./fuzz.sh --repro-case 123
  ./fuzz.sh --repro artifacts/case_7.input.bin artifacts/case_7.ops.txt
EOF
}

# Defaults
CASES=10000
SEED=$(date +%s)
ASAN=0
MINIMIZE=0
TIMEOUT_MS=1500
MAX_DEPTH=12
MAX_REPEAT=3
ARTIFACTS="artifacts"
REPRO_CASE=""
REPRO_INPUT=""
REPRO_OPS=""
QUIET=0

# Parse args
while [[ $# -gt 0 ]]; do
  case "$1" in
    --asan) ASAN=1; shift ;;
    --cases) CASES="$2"; shift 2 ;;
    --seed) SEED="$2"; shift 2 ;;
    --minimize) MINIMIZE=1; shift ;;
    --timeout-ms) TIMEOUT_MS="$2"; shift 2 ;;
    --max-depth) MAX_DEPTH="$2"; shift 2 ;;
    --max-repeat) MAX_REPEAT="$2"; shift 2 ;;
    --artifacts) ARTIFACTS="$2"; shift 2 ;;
    --repro-case) REPRO_CASE="$2"; shift 2 ;;
    --repro) REPRO_INPUT="$2"; REPRO_OPS="$3"; shift 3 ;;
    --quiet) QUIET=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; usage; exit 1 ;;
  esac
done

log() { if [[ $QUIET -eq 0 ]]; then echo "$@"; fi; }

mkdir -p "$ARTIFACTS"

# Repro mode helper (no rebuild)
run_repro() {
  local fiskta_bin="$1"
  shift
  local out_log="$ARTIFACTS/repro.stdout.log"
  local err_log="$ARTIFACTS/repro.stderr.log"

  : >"$out_log"; : >"$err_log" # truncate logs

  luajit tests/fiskta_fuzz.lua "$@" \
    >"$out_log" 2>"$err_log"

  log ""
  log "Repro done. Logs:"
  log "  stdout: $out_log"
  log "  stderr: $err_log"
}

# Repro mode?
if [[ -n "$REPRO_CASE" || -n "$REPRO_INPUT" ]]; then
  FISKTA_BIN="${ASAN:+./fiskta-asan}"
  FISKTA_BIN="${FISKTA_BIN:-./fiskta}"
  if [[ ! -x "$FISKTA_BIN" ]]; then
    echo "Error: $FISKTA_BIN not found or not executable. Build first (add --asan to pick asan build) or run ./fuzz.sh once." >&2
    exit 2
  fi

  if [[ -n "$REPRO_CASE" ]]; then
    run_repro "$FISKTA_BIN" \
      --fiskta-path "$FISKTA_BIN" \
      --artifacts "$ARTIFACTS" \
      --repro-case "$REPRO_CASE"
  else
    run_repro "$FISKTA_BIN" \
      --fiskta-path "$FISKTA_BIN" \
      --artifacts "$ARTIFACTS" \
      --repro "$REPRO_INPUT" "$REPRO_OPS"
  fi
  exit 0
fi

# Build
if [[ $ASAN -eq 1 ]]; then
  log "Building fiskta with ASan/UBSan..."
  VERSION=$(cat VERSION 2>/dev/null || echo "dev")
  CC=${CC:-cc}
  CFLAGS="-std=c11 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined"
  CFLAGS="$CFLAGS -Wall -Wextra -Wconversion -Wshadow -Wcast-qual -Wpointer-arith -Wbad-function-cast -Wundef"
  CFLAGS="$CFLAGS -pedantic -Wcast-align -Wmissing-declarations -Wwrite-strings -Wstrict-aliasing=2"
  set -x
  $CC $CFLAGS -DFISKTA_VERSION=\"$VERSION\" -D_POSIX_C_SOURCE=199309L \
      src/main.c src/parse.c src/engine.c src/iosearch.c src/reprog.c src/util.c \
      -o fiskta-asan
  set +x
  FISKTA_BIN=./fiskta-asan
  # Helpful defaults for ASan runs (won't error if unset)
  export ASAN_OPTIONS=${ASAN_OPTIONS:-"abort_on_error=1:detect_leaks=1:symbolize=1"}
  export UBSAN_OPTIONS=${UBSAN_OPTIONS:-"print_stacktrace=1"}
else
  log "Building fiskta (release)..."
  ./build.sh
  FISKTA_BIN=./fiskta
fi

# Clean prior artifacts (including fuzzer logs)
rm -f "$ARTIFACTS"/case_*.{ops.txt,input.bin,meta.txt,stderr.txt,stdout.txt} \
      "$ARTIFACTS"/tmp_*.bin \
      "$ARTIFACTS"/fuzzer.{stdout,stderr}.log 2>/dev/null || true

log "Running fuzzer with $CASES cases (seed: $SEED), artifacts: $ARTIFACTS"

OUT_LOG="$ARTIFACTS/fuzzer.stdout.log"
ERR_LOG="$ARTIFACTS/fuzzer.stderr.log"
: >"$OUT_LOG"; : >"$ERR_LOG" # truncate logs

# Assemble fuzzer args
ARGS=(
  --fiskta-path "$FISKTA_BIN"
  --readme README.md
  --cases "$CASES"
  --seed "$SEED"
  --artifacts "$ARTIFACTS"
  --timeout-ms "$TIMEOUT_MS"
  --max-depth "$MAX_DEPTH"
  --max-repeat "$MAX_REPEAT"
)
if [[ $MINIMIZE -eq 1 ]]; then ARGS+=( --minimize ); fi

# Run fuzzer quietly; logs go to files
luajit tests/fiskta_fuzz.lua "${ARGS[@]}" \
  >"$OUT_LOG" 2>"$ERR_LOG"

# Summary
echo ""
log "Fuzzing complete. Checking for artifacts..."
CRASHES=$(ls -1 "$ARTIFACTS"/case_*.ops.txt 2>/dev/null | wc -l | awk '{print $1}')
if [[ "$CRASHES" -gt 0 ]]; then
  echo "Found $CRASHES interesting case(s):"
  ls -1 "$ARTIFACTS"/case_*.ops.txt
  echo ""
  echo "Reproduce one (with ASan build):"
  echo "  ./fuzz.sh --asan --repro-case <N>"
else
  echo "No crashes/timeouts found."
fi

echo ""
echo "Fuzzer logs:"
echo "  stdout: $OUT_LOG"
echo "  stderr: $ERR_LOG"


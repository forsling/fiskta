#!/bin/bash
set -euo pipefail

# Clean, single-file benchmark script for fiskta
# Usage: ./benchmark.sh [options] [binary]
# Options:
#   -s, --small SIZE     Small file size in MB (default: 1)
#   -m, --medium SIZE    Medium file size in MB (default: 2)
#   -r, --runs N         Number of runs per test (default: 3)
#   -w, --warmup N       Number of warmup runs (default: 1)
#   -q, --quiet          Suppress verbose output
#   -h, --help           Show this help

# Default values
SMALL_MB=1
MEDIUM_MB=2
RUNS=3
WARMUPS=1
QUIET=false
BIN="./fiskta"

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -s|--small)
            SMALL_MB="$2"
            shift 2
            ;;
        -m|--medium)
            MEDIUM_MB="$2"
            shift 2
            ;;
        -r|--runs)
            RUNS="$2"
            shift 2
            ;;
        -w|--warmup)
            WARMUPS="$2"
            shift 2
            ;;
        -q|--quiet)
            QUIET=true
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [options] [binary]"
            echo "Options:"
            echo "  -s, --small SIZE     Small file size in MB (default: 1)"
            echo "  -m, --medium SIZE    Medium file size in MB (default: 2)"
            echo "  -r, --runs N         Number of runs per test (default: 3)"
            echo "  -w, --warmup N       Number of warmup runs (default: 1)"
            echo "  -q, --quiet          Suppress verbose output"
            echo "  -h, --help           Show this help"
            exit 0
            ;;
        -*)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
        *)
            BIN="$1"
            shift
            ;;
    esac
done

# Check if binary exists
if [[ ! -x "$BIN" ]]; then
    echo "ERROR: Binary $BIN not found or not executable" >&2
    exit 1
fi

# Check if time command is available
if ! command -v /usr/bin/time >/dev/null 2>&1; then
    echo "ERROR: /usr/bin/time not found" >&2
    exit 1
fi

# Create data directory
DATADIR="bench/data"
mkdir -p "$DATADIR"

# Test cases: name | dataset_file | size_mb | ops
declare -a CASES=(
    "find_forward_small|uniform_${SMALL_MB}MB.txt|${SMALL_MB}|find ERROR take 80b"
    "find_backward_window|uniform_${MEDIUM_MB}MB.txt|${MEDIUM_MB}|goto EOF find to BOF ERROR"
    "line_nav|uniform_${MEDIUM_MB}MB.txt|${MEDIUM_MB}|goto BOF skip 10000l take -500l take 500l"
    "emit_throughput|uniform_${MEDIUM_MB}MB.txt|${MEDIUM_MB}|take to EOF"
    "utf8_chars|utf8_${SMALL_MB}MB.txt|${SMALL_MB}|skip 1000c take -500c take 2000c"
    "take_until_line_start|uniform_${SMALL_MB}MB.txt|${SMALL_MB}|take until ERROR at line-start"
    "binary_until|binary_${SMALL_MB}MB.bin|${SMALL_MB}|skip 100000b take until END at match-end"
    "clauses_labels|uniform_${SMALL_MB}MB.txt|${SMALL_MB}|label A THEN find NOTPRESENT take 10b THEN goto A skip 500000b label B THEN goto B take 100b"
)

# Generate datasets if they don't exist
generate_datasets() {
    local generated=false

    if [[ ! -f "$DATADIR/uniform_${SMALL_MB}MB.txt" ]]; then
        [[ "$QUIET" == "false" ]] && echo "Generating uniform_${SMALL_MB}MB.txt..."
        awk -v size=$((SMALL_MB * 1024 * 1024)) '
        BEGIN {
            srand(42)
            bytes = 0
            while (bytes < size) {
                if (rand() < 0.1) {
                    line = "ERROR Something went wrong id=" substr($0, 1, 12)
                } else {
                    line = "2024-01-01T00:00:00Z host app[1234]: " substr($0, 1, 20)
                }
                print line
                bytes += length(line) + 1  # +1 for newline
            }
        }' > "$DATADIR/uniform_${SMALL_MB}MB.txt"
        generated=true
    fi

    if [[ ! -f "$DATADIR/uniform_${MEDIUM_MB}MB.txt" ]]; then
        [[ "$QUIET" == "false" ]] && echo "Generating uniform_${MEDIUM_MB}MB.txt..."
        awk -v size=$((MEDIUM_MB * 1024 * 1024)) '
        BEGIN {
            srand(42)
            bytes = 0
            while (bytes < size) {
                if (rand() < 0.1) {
                    line = "ERROR Something went wrong id=" substr($0, 1, 12)
                } else {
                    line = "2024-01-01T00:00:00Z host app[1234]: " substr($0, 1, 20)
                }
                print line
                bytes += length(line) + 1  # +1 for newline
            }
        }' > "$DATADIR/uniform_${MEDIUM_MB}MB.txt"
        generated=true
    fi

    if [[ ! -f "$DATADIR/utf8_${SMALL_MB}MB.txt" ]]; then
        [[ "$QUIET" == "false" ]] && echo "Generating utf8_${SMALL_MB}MB.txt..."
        awk -v size=$((SMALL_MB * 1024 * 1024)) '
        BEGIN {
            srand(42)
            bytes = 0
            while (bytes < size) {
                if (rand() < 0.1) {
                    line = "ERROR Something went wrong id=" substr($0, 1, 12)
                } else {
                    line = "2024-01-01T00:00:00Z host app[1234]: " substr($0, 1, 20)
                }
                print line
                bytes += length(line) + 1  # +1 for newline
            }
        }' > "$DATADIR/utf8_${SMALL_MB}MB.txt"
        generated=true
    fi

    if [[ ! -f "$DATADIR/binary_${SMALL_MB}MB.bin" ]]; then
        [[ "$QUIET" == "false" ]] && echo "Generating binary_${SMALL_MB}MB.bin..."
        dd if=/dev/urandom of="$DATADIR/binary_${SMALL_MB}MB.bin" bs=1M count=$SMALL_MB 2>/dev/null
        generated=true
    fi

    if [[ "$generated" == "true" && "$QUIET" == "false" ]]; then
        echo "Dataset generation complete."
    fi
}

# Convert time format to seconds
to_seconds() {
    local time_str="$1"
    if [[ "$time_str" =~ ^([0-9]+):([0-9]+)\.([0-9]+)$ ]]; then
        local minutes="${BASH_REMATCH[1]}"
        local seconds="${BASH_REMATCH[2]}"
        local ms="${BASH_REMATCH[3]}"
        echo "scale=3; $minutes * 60 + $seconds + $ms / 1000" | bc -l
    elif [[ "$time_str" =~ ^([0-9]+)\.([0-9]+)$ ]]; then
        echo "$time_str"
    else
        echo "0"
    fi
}

# Measure one test case
measure_one() {
    local bin="$1" label="$2" dataset="$3" size_mb="$4" ops="$5" run_idx="$6"
    local t_out t_err
    t_out="$(mktemp)"
    t_err="$(mktemp)"

    # Check if dataset exists
    if [[ ! -f "$dataset" ]]; then
        echo "ERROR: Dataset $dataset not found" >&2
        rm -f "$t_out" "$t_err"
        return 1
    fi

    # Build command
    local cmd="$bin $ops $dataset"

    # Run warmup if this is the first run
    if [[ $run_idx -eq 1 ]]; then
        for ((w=1; w<=WARMUPS; w++)); do
            $cmd >/dev/null 2>&1 || true
        done
    fi

    # Measure with time
    set +e
    LC_ALL=C /usr/bin/time -v $cmd >"$t_out" 2>"$t_err"
    local exit_code=$?
    set -e

    # Parse timing data
    local wall raw_user raw_sys rss
    wall="$(grep -E '^[[:space:]]*Elapsed \(wall clock\) time' "$t_err" | awk -F': ' '{print $2}')"
    raw_user="$(grep -E '^[[:space:]]*User time \(seconds\)' "$t_err" | awk -F': ' '{print $2}')"
    raw_sys="$(grep -E '^[[:space:]]*System time \(seconds\)' "$t_err" | awk -F': ' '{print $2}')"
    rss="$(grep -E '^[[:space:]]*Maximum resident set size \(kbytes\)' "$t_err" | awk -F': ' '{print $2}')"

    local wall_s user_s sys_s rss_kb
    wall_s="$(to_seconds "${wall:-0}")"
    user_s="$(awk "BEGIN{print (${raw_user:-0})+0}")"
    sys_s="$(awk "BEGIN{print (${raw_sys:-0})+0}")"
    rss_kb="${rss:-0}"

    # Store results
    echo "$wall_s,$user_s,$sys_s,$rss_kb,$exit_code"

    rm -f "$t_out" "$t_err"
}

# Run benchmark
run_benchmark() {
    echo "Running benchmark for: $BIN"
    echo "Configuration: Small=${SMALL_MB}MB, Medium=${MEDIUM_MB}MB, Runs=${RUNS}, Warmups=${WARMUPS}"
    echo

    # Generate datasets
    generate_datasets

    # Results storage
    declare -A results
    declare -A times
    declare -A memory

    # Run all test cases
    for entry in "${CASES[@]}"; do
        IFS="|" read -r name dataset_file size_mb ops <<< "$entry"
        local dataset="$DATADIR/$dataset_file"

        echo "Testing: $name (${size_mb}MB)"

        # Run multiple times and collect results
        local wall_times=()
        local user_times=()
        local sys_times=()
        local rss_values=()
        local exit_codes=()

        for ((r=1; r<=RUNS; r++)); do
            local result
            result="$(measure_one "$BIN" "$name" "$dataset" "$size_mb" "$ops" "$r")"
            IFS=',' read -r wall user sys rss exit_code <<< "$result"

            wall_times+=("$wall")
            user_times+=("$user")
            sys_times+=("$sys")
            rss_values+=("$rss")
            exit_codes+=("$exit_code")
        done

        # Calculate statistics
        local wall_avg wall_min wall_max
        local user_avg user_min user_max
        local sys_avg sys_min sys_max
        local rss_avg rss_min rss_max

        # Wall time stats
        wall_avg="$(printf '%s\n' "${wall_times[@]}" | awk '{sum+=$1; if(NR==1||$1<min) min=$1; if(NR==1||$1>max) max=$1} END{printf "%.3f", sum/NR}')"
        wall_min="$(printf '%s\n' "${wall_times[@]}" | awk '{if(NR==1||$1<min) min=$1} END{printf "%.3f", min}')"
        wall_max="$(printf '%s\n' "${wall_times[@]}" | awk '{if(NR==1||$1>max) max=$1} END{printf "%.3f", max}')"

        # User time stats
        user_avg="$(printf '%s\n' "${user_times[@]}" | awk '{sum+=$1; if(NR==1||$1<min) min=$1; if(NR==1||$1>max) max=$1} END{printf "%.3f", sum/NR}')"
        user_min="$(printf '%s\n' "${user_times[@]}" | awk '{if(NR==1||$1<min) min=$1} END{printf "%.3f", min}')"
        user_max="$(printf '%s\n' "${user_times[@]}" | awk '{if(NR==1||$1>max) max=$1} END{printf "%.3f", max}')"

        # System time stats
        sys_avg="$(printf '%s\n' "${sys_times[@]}" | awk '{sum+=$1; if(NR==1||$1<min) min=$1; if(NR==1||$1>max) max=$1} END{printf "%.3f", sum/NR}')"
        sys_min="$(printf '%s\n' "${sys_times[@]}" | awk '{if(NR==1||$1<min) min=$1} END{printf "%.3f", min}')"
        sys_max="$(printf '%s\n' "${sys_times[@]}" | awk '{if(NR==1||$1>max) max=$1} END{printf "%.3f", max}')"

        # Memory stats
        rss_avg="$(printf '%s\n' "${rss_values[@]}" | awk '{sum+=$1; if(NR==1||$1<min) min=$1; if(NR==1||$1>max) max=$1} END{printf "%.0f", sum/NR}')"
        rss_min="$(printf '%s\n' "${rss_values[@]}" | awk '{if(NR==1||$1<min) min=$1} END{printf "%.0f", min}')"
        rss_max="$(printf '%s\n' "${rss_values[@]}" | awk '{if(NR==1||$1>max) max=$1} END{printf "%.0f", max}')"

        # Check for failures
        local failures=0
        for exit_code in "${exit_codes[@]}"; do
            if [[ $exit_code -ne 0 ]]; then
                ((failures++))
            fi
        done

        # Store results
        results["$name"]="$wall_avg"
        times["$name"]="$user_avg"
        memory["$name"]="$rss_avg"

        # Print results
        if [[ $failures -eq 0 ]]; then
            printf "  ✓ Wall: %.3fs (%.3f-%.3f) | User: %.3fs (%.3f-%.3f) | Sys: %.3fs (%.3f-%.3f) | Memory: %.0fKB (%.0f-%.0f)\n" \
                "$wall_avg" "$wall_min" "$wall_max" \
                "$user_avg" "$user_min" "$user_max" \
                "$sys_avg" "$sys_min" "$sys_max" \
                "$rss_avg" "$rss_min" "$rss_max"
        else
            printf "  ✗ FAILED (%d/%d runs failed)\n" "$failures" "$RUNS"
        fi
        echo
    done

    # Summary
    echo "Summary:"
    echo "--------"
    for name in "${!results[@]}"; do
        printf "%-20s: %.3fs wall, %.3fs user, %.0fKB memory\n" \
            "$name" "${results[$name]}" "${times[$name]}" "${memory[$name]}"
    done
}

# Main execution
run_benchmark

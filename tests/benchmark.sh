#!/bin/bash
# fiskta benchmark - comprehensive performance and memory testing
# Usage: tests/benchmark.sh [options] [fiskta_binary]
# Options:
#   -s, --summary-only    Show only summary (suppress individual test output)
#   -n, --iterations N    Run benchmark N times and show aggregate statistics

set -e

# Parse arguments
SUMMARY_ONLY=0
FISKTA=""
ITERATIONS=1
while [[ $# -gt 0 ]]; do
    case "$1" in
    -s|--summary-only)
        SUMMARY_ONLY=1
        shift
        ;;
    -n|--iterations)
        ITERATIONS="$2"
        shift 2
        ;;
    *)
        if [[ -z "$FISKTA" ]]; then
            FISKTA="$1"
        fi
        shift
        ;;
    esac
done

# Force C locale for deterministic parsing
export LC_ALL=C
export LANG=C

if [ -z "$FISKTA" ]; then
    FISKTA="./fiskta"
fi

if [ ! -x "$FISKTA" ]; then
    echo "Error: $FISKTA not found or not executable"
    exit 1
fi

# Benchmark configuration
BENCH_DIR="/tmp/fiskta_bench_$$"
mkdir -p "$BENCH_DIR"
trap "rm -rf $BENCH_DIR" EXIT

# High-resolution monotonic timestamp (nanoseconds)
now_ns() {
    if ts=$(date +%s%N 2>/dev/null) && [[ "$ts" =~ ^[0-9]+$ ]]; then
        printf '%s\n' "$ts"
        return 0
    fi
    if command -v python3 >/dev/null 2>&1; then
        python3 - <<'PY'
import time
print(int(time.perf_counter() * 1_000_000_000))
PY
        return 0
    fi
    if command -v perl >/dev/null 2>&1; then
        perl -MTime::HiRes=clock_gettime,CLOCK_MONOTONIC -e 'printf "%d\n", clock_gettime(CLOCK_MONOTONIC)*1e9'
        return 0
    fi
    printf '%s000000000\n' "$(date +%s)"
}

# Generate test data
generate_test_data() {
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    echo "Generating test data..."
fi

    # Binary files for throughput testing
    dd if=/dev/urandom of="$BENCH_DIR/10MB.bin" bs=1M count=10 2>/dev/null

    # Log file with realistic error patterns
    perl -e '
        for my $i (1..50000) {
            my @levels = ("ERROR", "WARN", "INFO");
            my $level = $levels[int(rand(3))];
            my $user = "user" . int(rand(1000));
            print "2024-01-15 12:34:56 [$level] Connection timeout user=$user\@example.com host=server$i\n";
        }
    ' > "$BENCH_DIR/logs.txt"

    # Config file with 500 services for complex navigation testing
    cat > "$BENCH_DIR/services.conf" <<'EOF'
# Services configuration file
# Format: [service_NNN] with host, port, timeout settings

EOF

    perl -e '
        for my $i (1..500) {
            printf "[service_%03d]\n", $i;
            printf "host=server%03d.example.com\n", $i;
            printf "port=%d\n", 8000 + $i;
            printf "timeout=%d\n", 30 + int(rand(120));
            print "\n";
        }
    ' >> "$BENCH_DIR/services.conf"
}

# Run benchmark with detailed timing and memory stats
run_bench() {
    local name="$1"
    shift

    local TIME="/usr/bin/time"

    {
        local t0_ns t1_ns elapsed_ns elapsed_ms max_rss
        t0_ns=$(now_ns)

        if ! LC_ALL=C LANG=C "$TIME" -f "%M" "$@" > "$BENCH_DIR/out.txt" 2> "$BENCH_DIR/time_basic.txt"; then
            echo "ERROR: Command failed: $*" >&2
            echo "$name|0|0|0|0"
            exit 0
        fi

        t1_ns=$(now_ns)
        elapsed_ns=$((t1_ns - t0_ns))
        elapsed_ms=$(echo "scale=3; $elapsed_ns / 1000000" | bc)

        read -r max_rss < "$BENCH_DIR/time_basic.txt"

        LC_ALL=C LANG=C "$TIME" -v "$@" > /dev/null 2> "$BENCH_DIR/time_v.txt"
        local minor_faults major_faults
        minor_faults=$(grep "Minor.*page faults" "$BENCH_DIR/time_v.txt" | awk '{print $NF}')
        major_faults=$(grep "Major.*page faults" "$BENCH_DIR/time_v.txt" | awk '{print $NF}')

        echo "$name|$elapsed_ms|$max_rss|$minor_faults|$major_faults"
    } || {
        echo "$name|0|0|0|0"
    }
}

# Print section header
# Print benchmark result line
print_result() {
    local section="$1"
    local name="$2"
    local time="$3"
    local mem="$4"
    local faults="${5:-0}"

    printf "%-12s %-34s %12s %10s %8s\n" "$section" "$name" "$time" "$mem" "$faults"
}

# Calculate stats for a category
calc_stats() {
    local -n results=$1
    local total_time=0
    local peak_mem=0
    local count=${#results[@]}

    for r in "${results[@]}"; do
        IFS='|' read -r name time mem faults_min faults_maj <<< "$r"
        total_time=$(echo "scale=2; $total_time + $time" | bc)
        # Use bc for comparison to handle decimals
        if (( $(echo "$mem > $peak_mem" | bc -l) )); then
            peak_mem=$mem
        fi
    done

    local avg_time=$(echo "scale=2; $total_time / $count" | bc)
    echo "$total_time $peak_mem $avg_time"
}

# Calculate statistical measures (mean, median, stddev, min, max) for an array of values
calc_statistics() {
    local values=("$@")
    if [ ${#values[@]} -eq 0 ]; then
        echo "0.00 0.00 0.00 0.00 0.00"
        return
    fi

    printf '%s\n' "${values[@]}" | sort -n | awk '
    {
        values[NR] = $1
        sum += $1
        if (NR == 1 || $1 < min) min = $1
        if (NR == 1 || $1 > max) max = $1
    }
    END {
        n = NR
        if (n == 0) {
            print "0.00 0.00 0.00 0.00 0.00"
            exit
        }

        mean = sum / n

        # Calculate median (values are already sorted via sort -n)
        if (n % 2 == 1) {
            median = values[int(n/2) + 1]
        } else {
            median = (values[n/2] + values[n/2 + 1]) / 2
        }

        # Calculate standard deviation
        if (n > 1) {
            variance_sum = 0
            for (i = 1; i <= n; i++) {
                diff = values[i] - mean
                variance_sum += diff * diff
            }
            stddev = sqrt(variance_sum / (n - 1))
        } else {
            stddev = 0
        }

        printf "%.2f %.2f %.2f %.2f %.2f\n", mean, median, stddev, min, max
    }'
}

# Generate test data
generate_test_data

# Get binary info
BINARY_SIZE_KB=$(($(stat -c%s "$FISKTA") / 1024))
VERSION=$("$FISKTA" --version 2>&1 | head -1)

# Print header
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    echo "Binary: $FISKTA"
    echo "Version: $VERSION"
    echo "Size (KB): ${BINARY_SIZE_KB}"
    echo "Date: $(date '+%Y-%m-%d %H:%M:%S')"
    echo ""
fi

# Arrays to store results across all iterations
declare -a all_core_sums
declare -a all_grand_totals

# Run benchmarks for specified number of iterations
for iter in $(seq 1 "$ITERATIONS"); do
    if [ "$ITERATIONS" -gt 1 ] && [ "$SUMMARY_ONLY" -eq 0 ]; then
        echo ""
        echo "=== Iteration $iter/$ITERATIONS ==="
    fi

    # Arrays to store results for this iteration
    declare -a throughput
    declare -a search
    declare -a complex
    declare -a core_samples

    if [ "$SUMMARY_ONLY" -eq 0 ]; then
        echo ""
        echo "Running benchmarks..."
        printf "%-12s %-34s %12s %10s %8s\n" "Section" "Test" "Time (ms)" "RSS KB" "Faults"
        printf "%-12s %-34s %12s %10s %8s\n" "------------" "----------------------------------" "------------" "--------" "--------"
    fi

# ============================================================
# THROUGHPUT TESTS (no search, raw I/O performance)
# ============================================================
throughput+=("$(run_bench "10MB extract" "$FISKTA" --input "$BENCH_DIR/10MB.bin" -- take 10000000b)")
IFS='|' read -r name time mem f1 f2 <<< "${throughput[-1]}"
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    print_result "Throughput" "$name" "$time" "$mem" "$f1"
fi
core_samples+=("$time")
ten_mb_time="$time"

throughput+=("$(run_bench "Skip 5MB + take 5MB" "$FISKTA" --input "$BENCH_DIR/10MB.bin" -- skip 5000000b take 5000000b)")
IFS='|' read -r name time mem f1 f2 <<< "${throughput[-1]}"
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    print_result "Throughput" "$name" "$time" "$mem" "$f1"
fi
core_samples+=("$time")

throughput+=("$(run_bench "Line skip + take" "$FISKTA" --input "$BENCH_DIR/logs.txt" -- skip 5000l take 5000l)")
IFS='|' read -r name time mem f1 f2 <<< "${throughput[-1]}"
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    print_result "Throughput" "$name" "$time" "$mem" "$f1"
fi
core_samples+=("$time")

# ============================================================
# SEARCH TESTS (pattern finding, literal and regex)
# ============================================================
search+=("$(run_bench "Find literal (forward)" "$FISKTA" --input "$BENCH_DIR/logs.txt" -- find ERROR take to line-end)")
IFS='|' read -r name time mem f1 f2 <<< "${search[-1]}"
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    print_result "Search" "$name" "$time" "$mem" "$f1"
fi
core_samples+=("$time")

search+=("$(run_bench "Find literal (backward)" "$FISKTA" --input "$BENCH_DIR/services.conf" -- skip to EOF find to BOF '[service_' take to line-end)")
IFS='|' read -r name time mem f1 f2 <<< "${search[-1]}"
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    print_result "Search" "$name" "$time" "$mem" "$f1"
fi
core_samples+=("$time")

search+=("$(run_bench "Take until" "$FISKTA" --input "$BENCH_DIR/logs.txt" -- take until user)")
IFS='|' read -r name time mem f1 f2 <<< "${search[-1]}"
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    print_result "Search" "$name" "$time" "$mem" "$f1"
fi
core_samples+=("$time")

search+=("$(run_bench "Regex simple" "$FISKTA" --input "$BENCH_DIR/logs.txt" -- find:re ERROR take to line-end)")
IFS='|' read -r name time mem f1 f2 <<< "${search[-1]}"
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    print_result "Search" "$name" "$time" "$mem" "$f1"
fi
core_samples+=("$time")

search+=("$(run_bench "Regex email extract" "$FISKTA" --input "$BENCH_DIR/logs.txt" -- "find:re" "[a-z0-9]+@[a-z.]+" take to match-end)")
IFS='|' read -r name time mem f1 f2 <<< "${search[-1]}"
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    print_result "Search" "$name" "$time" "$mem" "$f1"
fi

search+=("$(run_bench "Regex alternation" "$FISKTA" --input "$BENCH_DIR/logs.txt" -- "find:re" "(ERROR|WARN|INFO)" take to line-end)")
IFS='|' read -r name time mem f1 f2 <<< "${search[-1]}"
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    print_result "Search" "$name" "$time" "$mem" "$f1"
fi
core_samples+=("$time")

search+=("$(run_bench "Regex until" "$FISKTA" --input "$BENCH_DIR/logs.txt" -- take "until:re" ERROR)")
IFS='|' read -r name time mem f1 f2 <<< "${search[-1]}"
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    print_result "Search" "$name" "$time" "$mem" "$f1"
fi
core_samples+=("$time")

# ============================================================
# COMPLEX NAVIGATION TESTS (realistic jumpy programs)
# ============================================================

# Test 1: Multi-section extraction (500 iterations)
# Extract hostname from ALL 500 service sections
cat > "$BENCH_DIR/loop.fis" <<'FISPROG'
label LOOP
find "[service_"
skip to line-end
label S
find "["
OR skip to DONE
label E
view S E
find "host="
skip 5b
take to line-end
print "\n"
clear view
skip to E
skip 1b
skip to LOOP
label DONE
FISPROG
complex+=("$(run_bench "500 section extraction" "$FISKTA" --input "$BENCH_DIR/services.conf" --ops "$BENCH_DIR/loop.fis")")
IFS='|' read -r name time mem f1 f2 <<< "${complex[-1]}"
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    print_result "Complex" "$name" "$time" "$mem" "$f1"
fi
core_samples+=("$time")

# Test 2: Section extraction with views
cat > "$BENCH_DIR/view.fis" <<'FISPROG'
find "[service_001]"
label S
find "[service_002]"
label E
view S E
skip to S
find "port="
skip 5b
take to line-end
FISPROG
complex+=("$(run_bench "View-scoped extraction" "$FISKTA" --input "$BENCH_DIR/services.conf" --ops "$BENCH_DIR/view.fis")")
IFS='|' read -r name time mem f1 f2 <<< "${complex[-1]}"
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    print_result "Complex" "$name" "$time" "$mem" "$f1"
fi
core_samples+=("$time")

# Test 3: OR chain fallback
complex+=("$(run_bench "OR chain (3 patterns)" "$FISKTA" --input "$BENCH_DIR/logs.txt" -- find CRITICAL OR find ERROR OR find WARN take to line-end)")
IFS='|' read -r name time mem f1 f2 <<< "${complex[-1]}"
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    print_result "Complex" "$name" "$time" "$mem" "$f1"
fi
core_samples+=("$time")

# Test 4: Backward navigation with context
complex+=("$(run_bench "Back navigation + extract" "$FISKTA" --input "$BENCH_DIR/logs.txt" -- skip to EOF find to BOF ERROR skip to line-start take 1l)")
IFS='|' read -r name time mem f1 f2 <<< "${complex[-1]}"
if [ "$SUMMARY_ONLY" -eq 0 ]; then
    print_result "Complex" "$name" "$time" "$mem" "$f1"
fi
core_samples+=("$time")

# ============================================================
# SUMMARY
# ============================================================
read -r thr_total thr_peak thr_avg <<< "$(calc_stats throughput)"
read -r sea_total sea_peak sea_avg <<< "$(calc_stats search)"
read -r com_total com_peak com_avg <<< "$(calc_stats complex)"

grand_total=$(echo "scale=2; $thr_total + $sea_total + $com_total" | bc)
# Find max memory using bc for decimal comparison
if (( $(echo "$thr_peak > $sea_peak" | bc -l) )); then
    max_mem=$thr_peak
else
    max_mem=$sea_peak
fi
if (( $(echo "$com_peak > $max_mem" | bc -l) )); then
    max_mem=$com_peak
fi
mem_mb=$(echo "scale=1; $max_mem / 1024" | bc)

calc_mean_throughput() {
    local -n results=$1
    local total_bytes=0
    local total_time=0
    local idx=0
    for r in "${results[@]}"; do
        IFS='|' read -r name time mem f1 f2 <<< "$r"
        local bytes=0
        case $idx in
            0) bytes=$((10 * 1024 * 1024)) ;;
            1) bytes=$((5 * 1024 * 1024)) ;;
            2) bytes=0 ;;
        esac
        if (( bytes > 0 )) && (( $(echo "$time > 0" | bc -l) )); then
            total_bytes=$((total_bytes + bytes))
            total_time=$(echo "$total_time + $time" | bc)
        fi
        idx=$((idx + 1))
    done
    if (( total_bytes > 0 )) && (( $(echo "$total_time > 0" | bc -l) )); then
        awk -v bytes="$total_bytes" -v time_ms="$total_time" 'BEGIN { printf("%.1f", (bytes / 1048576) * 1000 / time_ms) }'
    else
        echo "N/A"
    fi
}

    throughput_mbs=$(calc_mean_throughput throughput)

    core_sum=$(printf '%s\n' "${core_samples[@]}" | awk '{sum+=$1} END {if (NR==0) printf("0.00"); else printf("%.2f", sum)}')

    # Store iteration results
    all_core_sums+=("$core_sum")
    all_grand_totals+=("$grand_total")

    if [ "$ITERATIONS" -eq 1 ] || [ "$SUMMARY_ONLY" -eq 0 ]; then
        echo ""
        echo "╔════════════════════════════════════════════════════════════╗"
        if [ "$ITERATIONS" -gt 1 ]; then
            summary_label="Iteration $iter/$ITERATIONS: $FISKTA"
        else
            summary_label="Summary for: $FISKTA"
        fi
        printf "║ %-58.58s ║\n" "$summary_label"
        echo "╠════════════════════════════════════════════════════════════╣"
        printf "║ %-23s %-34s ║\n" "Total runtime:" "${grand_total} ms"
        printf "║ %-23s %-34s ║\n" "Throughput section:" "${thr_total}ms (avg ${thr_avg}ms)"
        printf "║ %-23s %-34s ║\n" "Search section:" "${sea_total}ms (avg ${sea_avg}ms)"
        printf "║ %-23s %-34s ║\n" "Complex section:" "${com_total}ms (avg ${com_avg}ms)"
        printf "║ %-23s %-34s ║\n" "Mean throughput:" "${throughput_mbs} MB/s"
        printf "║ %-23s %-34s ║\n" "Peak memory:" "${mem_mb} MB"
        printf "║ %-23s %-34s ║\n" "Core sum:" "${core_sum} ms"
        echo "╚════════════════════════════════════════════════════════════╝"
    fi

    # Clear iteration arrays for next iteration
    unset throughput search complex core_samples
done

# Print aggregate statistics if multiple iterations
if [ "$ITERATIONS" -gt 1 ]; then
    read -r mean_core median_core stddev_core min_core max_core <<< "$(calc_statistics "${all_core_sums[@]}")"
    read -r mean_total median_total stddev_total min_total max_total <<< "$(calc_statistics "${all_grand_totals[@]}")"

    echo ""
    echo "╔════════════════════════════════════════════════════════════╗"
    printf "║ %-58.58s ║\n" "Aggregate Statistics ($ITERATIONS iterations)"
    echo "╠════════════════════════════════════════════════════════════╣"
    printf "║ %-58.58s ║\n" "Core Sum (ms):"
    printf "║   %-20s %-35s ║\n" "Mean:" "$mean_core"
    printf "║   %-20s %-35s ║\n" "Median:" "$median_core"
    printf "║   %-20s %-35s ║\n" "Std Dev:" "$stddev_core"
    printf "║   %-20s %-35s ║\n" "Min:" "$min_core"
    printf "║   %-20s %-35s ║\n" "Max:" "$max_core"
    echo "╠════════════════════════════════════════════════════════════╣"
    printf "║ %-58.58s ║\n" "Total Runtime (ms):"
    printf "║   %-20s %-35s ║\n" "Mean:" "$mean_total"
    printf "║   %-20s %-35s ║\n" "Median:" "$median_total"
    printf "║   %-20s %-35s ║\n" "Std Dev:" "$stddev_total"
    printf "║   %-20s %-35s ║\n" "Min:" "$min_total"
    printf "║   %-20s %-35s ║\n" "Max:" "$max_total"
    echo "╚════════════════════════════════════════════════════════════╝"
fi

#!/bin/bash
set -euo pipefail

# Enhanced benchmark script for fiskta
# Usage: ./benchmark.sh [options] [binary]
# Options:
#   -s, --small SIZE     Small file size in MB (default: 1)
#   -m, --medium SIZE    Medium file size in MB (default: 10)
#   -l, --large SIZE     Large file size in MB (default: 100)
#   -r, --runs N         Number of runs per test (default: 5)
#   -w, --warmup N       Number of warmup runs (default: 2)
#   -c, --compare        Enable comparative analysis with grep/awk/sed
#   -f, --format FORMAT  Output format: human,csv,json,markdown (default: human)
#   -o, --output FILE    Output file (default: stdout)
#   -p, --profile        Enable profiling with perf
#   -q, --quiet          Suppress verbose output
#   -h, --help           Show this help

# Default values
SMALL_MB=1
MEDIUM_MB=10
LARGE_MB=100
RUNS=5
WARMUPS=2
COMPARE=false
FORMAT="human"
OUTPUT_FILE=""
PROFILE=false
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
        -l|--large)
            LARGE_MB="$2"
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
        -c|--compare)
            COMPARE=true
            shift
            ;;
        -f|--format)
            FORMAT="$2"
            shift 2
            ;;
        -o|--output)
            OUTPUT_FILE="$2"
            shift 2
            ;;
        -p|--profile)
            PROFILE=true
            shift
            ;;
        -q|--quiet)
            QUIET=true
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [options] [binary]"
            echo "Options:"
            echo "  -s, --small SIZE     Small file size in MB (default: 1)"
            echo "  -m, --medium SIZE    Medium file size in MB (default: 10)"
            echo "  -l, --large SIZE     Large file size in MB (default: 100)"
            echo "  -r, --runs N         Number of runs per test (default: 5)"
            echo "  -w, --warmup N       Number of warmup runs (default: 2)"
            echo "  -c, --compare        Enable comparative analysis with grep/awk/sed"
            echo "  -f, --format FORMAT  Output format: human,csv,json,markdown (default: human)"
            echo "  -o, --output FILE    Output file (default: stdout)"
            echo "  -p, --profile        Enable profiling with perf"
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

# Check if awk is available for calculations
if ! command -v awk >/dev/null 2>&1; then
    echo "ERROR: awk not found (required for calculations)" >&2
    exit 1
fi

# Create data directory
DATADIR="bench/data"
mkdir -p "$DATADIR"

# Test cases: name | dataset_file | size_mb | ops | description
declare -a CASES=(
    "find_simple_small|uniform_${SMALL_MB}MB.txt|${SMALL_MB}|find ERROR take 80b|Simple text search on small file"
    "find_simple_medium|uniform_${MEDIUM_MB}MB.txt|${MEDIUM_MB}|find ERROR take 80b|Simple text search on medium file"
    "find_simple_large|uniform_${LARGE_MB}MB.txt|${LARGE_MB}|find ERROR take 80b|Simple text search on large file"
    "find_regex_small|uniform_${SMALL_MB}MB.txt|${SMALL_MB}|findr 'ERROR.*id=[0-9]+' take 80b|Regex search on small file"
    "find_regex_medium|uniform_${MEDIUM_MB}MB.txt|${MEDIUM_MB}|findr 'ERROR.*id=[0-9]+' take 80b|Regex search on medium file"
    "find_backward_window|uniform_${MEDIUM_MB}MB.txt|${MEDIUM_MB}|goto EOF find to BOF ERROR|Backward search with window"
    "line_navigation|uniform_${MEDIUM_MB}MB.txt|${MEDIUM_MB}|goto BOF skip 10000l take -500l take 500l|Line-based navigation"
    "take_throughput_small|uniform_${SMALL_MB}MB.txt|${SMALL_MB}|take to EOF|Full file throughput (small)"
    "take_throughput_medium|uniform_${MEDIUM_MB}MB.txt|${MEDIUM_MB}|take to EOF|Full file throughput (medium)"
    "take_throughput_large|uniform_${LARGE_MB}MB.txt|${LARGE_MB}|take to EOF|Full file throughput (large)"
    "utf8_chars|utf8_${SMALL_MB}MB.txt|${SMALL_MB}|skip 1000c take -500c take 2000c|UTF-8 character processing"
    "take_until_line_start|uniform_${SMALL_MB}MB.txt|${SMALL_MB}|take until ERROR at line-start|Take until with line boundary"
    "binary_until|binary_${SMALL_MB}MB.bin|${SMALL_MB}|skip 100000b take until END at match-end|Binary data processing"
    "clauses_labels|uniform_${SMALL_MB}MB.txt|${SMALL_MB}|label A :: find NOTPRESENT take 10b :: goto A skip 500000b label B :: goto B take 100b|Complex clause operations"
)

# Comparative tools and their commands
declare -A COMPARISON_TOOLS=(
    ["grep"]="grep ERROR"
    ["awk"]="awk '/ERROR/ {print}'"
    ["sed"]="sed -n '/ERROR/p'"
)

# Generate datasets if they don't exist
generate_datasets() {
    local generated=false

    # Generate uniform text files
    for size in "$SMALL_MB" "$MEDIUM_MB" "$LARGE_MB"; do
        if [[ ! -f "$DATADIR/uniform_${size}MB.txt" ]]; then
            [[ "$QUIET" == "false" ]] && echo "Generating uniform_${size}MB.txt..."
            # Create a simple pattern-based file instead of complex awk
            local target_bytes=$((size * 1024 * 1024))
            local current_bytes=0
            local line_count=0
            
            {
                while [[ $current_bytes -lt $target_bytes ]]; do
                    if [[ $((line_count % 10)) -eq 0 ]]; then
                        echo "ERROR Something went wrong id=$line_count"
                    else
                        echo "2024-01-01T00:00:00Z host app[1234]: Normal log message $line_count"
                    fi
                    current_bytes=$((current_bytes + 60))  # Approximate line length
                    line_count=$((line_count + 1))
                done
            } > "$DATADIR/uniform_${size}MB.txt"
            generated=true
        fi
    done

    # Generate UTF-8 test file
    if [[ ! -f "$DATADIR/utf8_${SMALL_MB}MB.txt" ]]; then
        [[ "$QUIET" == "false" ]] && echo "Generating utf8_${SMALL_MB}MB.txt..."
        # Copy the small uniform file as UTF-8 test
        cp "$DATADIR/uniform_${SMALL_MB}MB.txt" "$DATADIR/utf8_${SMALL_MB}MB.txt"
        generated=true
    fi

    # Generate binary test file
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
        awk "BEGIN {printf \"%.3f\", $minutes * 60 + $seconds + $ms / 1000}"
    elif [[ "$time_str" =~ ^([0-9]+)\.([0-9]+)$ ]]; then
        echo "$time_str"
    else
        echo "0"
    fi
}

# Calculate throughput in MB/s
calculate_throughput() {
    local size_mb="$1"
    local time_s="$2"
    if [[ $(awk "BEGIN {print ($time_s > 0)}") -eq 1 ]]; then
        awk "BEGIN {printf \"%.2f\", $size_mb / $time_s}"
    else
        echo "0"
    fi
}

# Calculate memory efficiency (MB memory per MB processed)
calculate_memory_efficiency() {
    local memory_mb="$1"
    local size_mb="$2"
    if [[ $(awk "BEGIN {print ($size_mb > 0)}") -eq 1 ]]; then
        awk "BEGIN {printf \"%.3f\", $memory_mb / $size_mb}"
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

    # Measure with high precision timing
    set +e
    if [[ "$PROFILE" == "true" && $run_idx -eq 1 ]]; then
        # Profile only the first run
        perf record -g -o "perf_${label}.data" $cmd >"$t_out" 2>"$t_err"
    else
        # Use high precision timing instead of /usr/bin/time for better accuracy
        local start_time=$(date +%s.%N)
        $cmd >"$t_out" 2>"$t_err"
        local end_time=$(date +%s.%N)
        local wall_time=$(awk "BEGIN {printf \"%.6f\", $end_time - $start_time}")
        
        # Parse memory usage from /usr/bin/time output
        LC_ALL=C /usr/bin/time -v $cmd >/dev/null 2>"$t_err" || true
    fi
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

    # Calculate derived metrics
    local throughput_mbps memory_efficiency
    throughput_mbps="$(calculate_throughput "$size_mb" "$wall_s")"
    memory_efficiency="$(calculate_memory_efficiency "$(awk "BEGIN {printf \"%.3f\", $rss_kb / 1024}")" "$size_mb")"

    # Store results
    echo "$wall_s,$user_s,$sys_s,$rss_kb,$exit_code,$throughput_mbps,$memory_efficiency"

    rm -f "$t_out" "$t_err"
}

# Measure comparative tool
measure_comparison() {
    local tool="$1" cmd="$2" dataset="$3" size_mb="$4" run_idx="$5"
    local t_out t_err
    t_out="$(mktemp)"
    t_err="$(mktemp)"

    # Build full command
    local full_cmd="$cmd $dataset"

    # Run warmup if this is the first run
    if [[ $run_idx -eq 1 ]]; then
        for ((w=1; w<=WARMUPS; w++)); do
            eval "$full_cmd" >/dev/null 2>&1 || true
        done
    fi

    # Measure with time
    set +e
    LC_ALL=C /usr/bin/time -v bash -c "$full_cmd" >"$t_out" 2>"$t_err"
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

    # Calculate derived metrics
    local throughput_mbps memory_efficiency
    throughput_mbps="$(calculate_throughput "$size_mb" "$wall_s")"
    memory_efficiency="$(calculate_memory_efficiency "$(awk "BEGIN {printf \"%.3f\", $rss_kb / 1024}")" "$size_mb")"

    # Store results
    echo "$wall_s,$user_s,$sys_s,$rss_kb,$exit_code,$throughput_mbps,$memory_efficiency"

    rm -f "$t_out" "$t_err"
}

# Calculate statistics
calculate_stats() {
    local values=("$@")
    local sum=0
    local min="${values[0]}"
    local max="${values[0]}"
    
    for val in "${values[@]}"; do
        sum=$(awk "BEGIN {printf \"%.6f\", $sum + $val}")
        if [[ $(awk "BEGIN {print ($val < $min)}") -eq 1 ]]; then
            min="$val"
        fi
        if [[ $(awk "BEGIN {print ($val > $max)}") -eq 1 ]]; then
            max="$val"
        fi
    done
    
    local avg=$(awk "BEGIN {printf \"%.6f\", $sum / ${#values[@]}}")
    echo "$avg,$min,$max"
}

# Output functions for different formats
output_human() {
    local results="$1"
    echo "fiskta Benchmark Results"
    echo "======================="
    echo "Configuration: Small=${SMALL_MB}MB, Medium=${MEDIUM_MB}MB, Large=${LARGE_MB}MB, Runs=${RUNS}, Warmups=${WARMUPS}"
    echo "Binary: $BIN"
    echo "Timestamp: $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    echo
    
    while IFS='|' read -r name dataset_file size_mb ops description wall_avg wall_min wall_max user_avg user_min user_max sys_avg sys_min sys_max rss_avg rss_min rss_max throughput_mbps memory_efficiency failures; do
        if [[ $failures -eq 0 ]]; then
            printf "%-25s (%dMB): %s\n" "$name" "$size_mb" "$description"
            printf "  ✓ Wall: %.3fs (%.3f-%.3f) | User: %.3fs (%.3f-%.3f) | Sys: %.3fs (%.3f-%.3f)\n" \
                "$wall_avg" "$wall_min" "$wall_max" \
                "$user_avg" "$user_min" "$user_max" \
                "$sys_avg" "$sys_min" "$sys_max"
            printf "  Memory: %.0fKB (%.0f-%.0f) | Throughput: %.1f MB/s | Efficiency: %.3f MB/MB\n" \
                "$rss_avg" "$rss_min" "$rss_max" "$throughput_mbps" "$memory_efficiency"
        else
            printf "%-25s (%dMB): %s\n" "$name" "$size_mb" "$description"
            printf "  ✗ FAILED (%d/%d runs failed)\n" "$failures" "$RUNS"
        fi
        echo
    done <<< "$results"
}

output_csv() {
    local results="$1"
    echo "test_name,file_size_mb,description,wall_time_avg,wall_time_min,wall_time_max,user_time_avg,user_time_min,user_time_max,sys_time_avg,sys_time_min,sys_time_max,peak_memory_kb,peak_memory_min_kb,peak_memory_max_kb,throughput_mbps,memory_efficiency,failures"
    while IFS='|' read -r name dataset_file size_mb ops description wall_avg wall_min wall_max user_avg user_min user_max sys_avg sys_min sys_max rss_avg rss_min rss_max throughput_mbps memory_efficiency failures; do
        echo "$name,$size_mb,\"$description\",$wall_avg,$wall_min,$wall_max,$user_avg,$user_min,$user_max,$sys_avg,$sys_min,$sys_max,$rss_avg,$rss_min,$rss_max,$throughput_mbps,$memory_efficiency,$failures"
    done <<< "$results"
}

output_json() {
    local results="$1"
    echo "{"
    echo "  \"benchmark\": {"
    echo "    \"version\": \"2.0\","
    echo "    \"timestamp\": \"$(date -u +"%Y-%m-%dT%H:%M:%SZ")\","
    echo "    \"binary\": \"$BIN\","
    echo "    \"configuration\": {"
    echo "      \"small_mb\": $SMALL_MB,"
    echo "      \"medium_mb\": $MEDIUM_MB,"
    echo "      \"large_mb\": $LARGE_MB,"
    echo "      \"runs\": $RUNS,"
    echo "      \"warmups\": $WARMUPS,"
    echo "      \"compare\": $COMPARE,"
    echo "      \"profile\": $PROFILE"
    echo "    },"
    echo "    \"results\": {"
    
    local first=true
    while IFS='|' read -r name dataset_file size_mb ops description wall_avg wall_min wall_max user_avg user_min user_max sys_avg sys_min sys_max rss_avg rss_min rss_max throughput_mbps memory_efficiency failures; do
        if [[ "$first" == "true" ]]; then
            first=false
        else
            echo ","
        fi
        echo "      \"$name\": {"
        echo "        \"description\": \"$description\","
        echo "        \"file_size_mb\": $size_mb,"
        echo "        \"wall_time\": {\"avg\": $wall_avg, \"min\": $wall_min, \"max\": $wall_max},"
        echo "        \"user_time\": {\"avg\": $user_avg, \"min\": $user_min, \"max\": $user_max},"
        echo "        \"sys_time\": {\"avg\": $sys_avg, \"min\": $sys_min, \"max\": $sys_max},"
        echo "        \"peak_memory_kb\": {\"avg\": $rss_avg, \"min\": $rss_min, \"max\": $rss_max},"
        echo "        \"throughput_mbps\": $throughput_mbps,"
        echo "        \"memory_efficiency\": $memory_efficiency,"
        echo "        \"failures\": $failures"
        echo -n "      }"
    done <<< "$results"
    echo
    echo "    }"
    echo "  }"
    echo "}"
}

output_markdown() {
    local results="$1"
    echo "# fiskta Benchmark Results"
    echo
    echo "**Configuration:** Small=${SMALL_MB}MB, Medium=${MEDIUM_MB}MB, Large=${LARGE_MB}MB, Runs=${RUNS}, Warmups=${WARMUPS}"  
    echo "**Binary:** \`$BIN\`"
    echo "**Timestamp:** $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    echo
    echo "## Results"
    echo
    echo "| Test | Size | Description | Wall Time (s) | User Time (s) | Memory (KB) | Throughput (MB/s) | Efficiency |"
    echo "|------|------|-------------|---------------|--------------|------------|-------------------|------------|"
    
    while IFS='|' read -r name dataset_file size_mb ops description wall_avg wall_min wall_max user_avg user_min user_max sys_avg sys_min sys_max rss_avg rss_min rss_max throughput_mbps memory_efficiency failures; do
        if [[ $failures -eq 0 ]]; then
            printf "| %s | %dMB | %s | %.3f (%.3f-%.3f) | %.3f (%.3f-%.3f) | %.0f (%.0f-%.0f) | %.1f | %.3f |\n" \
                "$name" "$size_mb" "$description" \
                "$wall_avg" "$wall_min" "$wall_max" \
                "$user_avg" "$user_min" "$user_max" \
                "$rss_avg" "$rss_min" "$rss_max" \
                "$throughput_mbps" "$memory_efficiency"
        else
            printf "| %s | %dMB | %s | FAILED (%d/%d) | - | - | - | - |\n" \
                "$name" "$size_mb" "$description" "$failures" "$RUNS"
        fi
    done <<< "$results"
}

# Run benchmark
run_benchmark() {
    [[ "$QUIET" == "false" ]] && echo "Running enhanced benchmark for: $BIN"
    [[ "$QUIET" == "false" ]] && echo "Configuration: Small=${SMALL_MB}MB, Medium=${MEDIUM_MB}MB, Large=${LARGE_MB}MB, Runs=${RUNS}, Warmups=${WARMUPS}"
    [[ "$QUIET" == "false" ]] && echo "Format: $FORMAT, Compare: $COMPARE, Profile: $PROFILE"
    [[ "$QUIET" == "false" ]] && echo

    # Generate datasets
    generate_datasets

    # Results storage
    local results=""

    # Run all test cases
    for entry in "${CASES[@]}"; do
        IFS="|" read -r name dataset_file size_mb ops description <<< "$entry"
        local dataset="$DATADIR/$dataset_file"

        [[ "$QUIET" == "false" ]] && echo "Testing: $name (${size_mb}MB) - $description"

        # Run multiple times and collect results
        local wall_times=()
        local user_times=()
        local sys_times=()
        local rss_values=()
        local exit_codes=()
        local throughputs=()
        local memory_efficiencies=()

        for ((r=1; r<=RUNS; r++)); do
            local result
            result="$(measure_one "$BIN" "$name" "$dataset" "$size_mb" "$ops" "$r")"
            IFS=',' read -r wall user sys rss exit_code throughput memory_efficiency <<< "$result"

            wall_times+=("$wall")
            user_times+=("$user")
            sys_times+=("$sys")
            rss_values+=("$rss")
            exit_codes+=("$exit_code")
            throughputs+=("$throughput")
            memory_efficiencies+=("$memory_efficiency")
        done

        # Calculate statistics
        local wall_stats user_stats sys_stats rss_stats
        wall_stats="$(calculate_stats "${wall_times[@]}")"
        user_stats="$(calculate_stats "${user_times[@]}")"
        sys_stats="$(calculate_stats "${sys_times[@]}")"
        rss_stats="$(calculate_stats "${rss_values[@]}")"

        IFS=',' read -r wall_avg wall_min wall_max <<< "$wall_stats"
        IFS=',' read -r user_avg user_min user_max <<< "$user_stats"
        IFS=',' read -r sys_avg sys_min sys_max <<< "$sys_stats"
        IFS=',' read -r rss_avg rss_min rss_max <<< "$rss_stats"

        # Calculate average throughput and memory efficiency
        local throughput_avg memory_efficiency_avg
        throughput_avg="$(calculate_stats "${throughputs[@]}")"
        memory_efficiency_avg="$(calculate_stats "${memory_efficiencies[@]}")"
        throughput_avg="$(echo "$throughput_avg" | cut -d',' -f1)"
        memory_efficiency_avg="$(echo "$memory_efficiency_avg" | cut -d',' -f1)"

        # Check for failures
        local failures=0
        for exit_code in "${exit_codes[@]}"; do
            if [[ $exit_code -ne 0 ]]; then
                ((failures++))
            fi
        done

        # Store results
        results+="$name|$dataset_file|$size_mb|$ops|$description|$wall_avg|$wall_min|$wall_max|$user_avg|$user_min|$user_max|$sys_avg|$sys_min|$sys_max|$rss_avg|$rss_min|$rss_max|$throughput_avg|$memory_efficiency_avg|$failures"$'\n'

        [[ "$QUIET" == "false" ]] && echo "  ✓ Completed"
    done

    # Run comparative analysis if requested
    if [[ "$COMPARE" == "true" ]]; then
        [[ "$QUIET" == "false" ]] && echo
        [[ "$QUIET" == "false" ]] && echo "Running comparative analysis..."
        
        # Test simple find operations against grep/awk/sed
        local compare_dataset="$DATADIR/uniform_${MEDIUM_MB}MB.txt"
        for tool in "${!COMPARISON_TOOLS[@]}"; do
            local cmd="${COMPARISON_TOOLS[$tool]}"
            [[ "$QUIET" == "false" ]] && echo "Testing: $tool"
            
            local tool_wall_times=()
            local tool_user_times=()
            local tool_sys_times=()
            local tool_rss_values=()
            local tool_exit_codes=()
            local tool_throughputs=()
            local tool_memory_efficiencies=()

            for ((r=1; r<=RUNS; r++)); do
                local result
                result="$(measure_comparison "$tool" "$cmd" "$compare_dataset" "$MEDIUM_MB" "$r")"
                IFS=',' read -r wall user sys rss exit_code throughput memory_efficiency <<< "$result"

                tool_wall_times+=("$wall")
                tool_user_times+=("$user")
                tool_sys_times+=("$sys")
                tool_rss_values+=("$rss")
                tool_exit_codes+=("$exit_code")
                tool_throughputs+=("$throughput")
                tool_memory_efficiencies+=("$memory_efficiency")
            done

            # Calculate statistics for comparison tool
            local tool_wall_stats tool_user_stats tool_sys_stats tool_rss_stats
            tool_wall_stats="$(calculate_stats "${tool_wall_times[@]}")"
            tool_user_stats="$(calculate_stats "${tool_user_times[@]}")"
            tool_sys_stats="$(calculate_stats "${tool_sys_times[@]}")"
            tool_rss_stats="$(calculate_stats "${tool_rss_values[@]}")"

            IFS=',' read -r tool_wall_avg tool_wall_min tool_wall_max <<< "$tool_wall_stats"
            IFS=',' read -r tool_user_avg tool_user_min tool_user_max <<< "$tool_user_stats"
            IFS=',' read -r tool_sys_avg tool_sys_min tool_sys_max <<< "$tool_sys_stats"
            IFS=',' read -r tool_rss_avg tool_rss_min tool_rss_max <<< "$tool_rss_stats"

            local tool_throughput_avg tool_memory_efficiency_avg
            tool_throughput_avg="$(calculate_stats "${tool_throughputs[@]}")"
            tool_memory_efficiency_avg="$(calculate_stats "${tool_memory_efficiencies[@]}")"
            tool_throughput_avg="$(echo "$tool_throughput_avg" | cut -d',' -f1)"
            tool_memory_efficiency_avg="$(echo "$tool_memory_efficiency_avg" | cut -d',' -f1)"

            # Check for failures
            local tool_failures=0
            for exit_code in "${tool_exit_codes[@]}"; do
                if [[ $exit_code -ne 0 ]]; then
                    ((tool_failures++))
                fi
            done

            # Store comparison results
            results+="compare_${tool}|uniform_${MEDIUM_MB}MB.txt|$MEDIUM_MB|$cmd|Comparison with $tool|$tool_wall_avg|$tool_wall_min|$tool_wall_max|$tool_user_avg|$tool_user_min|$tool_user_max|$tool_sys_avg|$tool_sys_min|$tool_sys_max|$tool_rss_avg|$tool_rss_min|$tool_rss_max|$tool_throughput_avg|$tool_memory_efficiency_avg|$tool_failures"$'\n'

            [[ "$QUIET" == "false" ]] && echo "  ✓ Completed"
        done
    fi

    # Output results in requested format
    case "$FORMAT" in
        "human")
            if [[ -n "$OUTPUT_FILE" ]]; then
                output_human "$results" > "$OUTPUT_FILE"
            else
                output_human "$results"
            fi
            ;;
        "csv")
            if [[ -n "$OUTPUT_FILE" ]]; then
                output_csv "$results" > "$OUTPUT_FILE"
            else
                output_csv "$results"
            fi
            ;;
        "json")
            if [[ -n "$OUTPUT_FILE" ]]; then
                output_json "$results" > "$OUTPUT_FILE"
            else
                output_json "$results"
            fi
            ;;
        "markdown")
            if [[ -n "$OUTPUT_FILE" ]]; then
                output_markdown "$results" > "$OUTPUT_FILE"
            else
                output_markdown "$results"
            fi
            ;;
        *)
            echo "ERROR: Unknown format: $FORMAT" >&2
            exit 1
            ;;
    esac

    # Generate profile reports if profiling was enabled
    if [[ "$PROFILE" == "true" ]]; then
        [[ "$QUIET" == "false" ]] && echo
        [[ "$QUIET" == "false" ]] && echo "Generating profile reports..."
        for perf_file in perf_*.data; do
            if [[ -f "$perf_file" ]]; then
                local test_name="${perf_file#perf_}"
                test_name="${test_name%.data}"
                perf report --stdio -i "$perf_file" > "profile_${test_name}.txt"
                [[ "$QUIET" == "false" ]] && echo "  Generated profile_${test_name}.txt"
            fi
        done
    fi

    [[ "$QUIET" == "false" ]] && echo
    [[ "$QUIET" == "false" ]] && echo "Benchmark complete!"
}

# Main execution
run_benchmark
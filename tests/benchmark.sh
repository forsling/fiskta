#!/bin/bash
set -euo pipefail

# fiskta Benchmark Script
# Usage: ./benchmark.sh [options]

# Default values
SMALL_MB=1
MEDIUM_MB=10
RUNS=3
WARMUPS=1
FORMAT="human"
BIN="./fiskta"

# Parse arguments
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
        -f|--format)
            FORMAT="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  -s, --small SIZE     Small file size in MB (default: 1)"
            echo "  -m, --medium SIZE    Medium file size in MB (default: 10)"
            echo "  -r, --runs N         Number of runs per test (default: 3)"
            echo "  -f, --format FORMAT  Output format: human,json (default: human)"
            echo "  -h, --help           Show this help"
            exit 0
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

# Create data directory
DATADIR="bench/data"
mkdir -p "$DATADIR"

echo "fiskta Minimal Benchmark"
echo "========================"
echo "Binary: $BIN"
echo "Configuration: Small=${SMALL_MB}MB, Medium=${MEDIUM_MB}MB, Runs=${RUNS}"
echo

# Generate test datasets
generate_test_file() {
    local size_mb="$1"
    local filename="$2"
    local target_bytes=$((size_mb * 1024 * 1024))
    local current_bytes=0
    local line_count=0
    
    echo "Generating $filename (${size_mb}MB)..."
    
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
    } > "$DATADIR/$filename"
}

# Generate datasets
generate_test_file "$SMALL_MB" "small.txt"
generate_test_file "$MEDIUM_MB" "medium.txt"

# Test cases
declare -a TESTS=(
    "find_simple_small|small.txt|find ERROR take 5b"
    "find_simple_medium|medium.txt|find ERROR take 5b"
    "take_throughput_small|small.txt|take to EOF"
    "take_throughput_medium|medium.txt|take to EOF"
)

# Measure performance
measure_test() {
    local test_name="$1"
    local dataset="$2"
    local ops="$3"
    local runs="$4"
    
    local dataset_path="$DATADIR/$dataset"
    local times=()
    
    echo "Testing: $test_name" >&2
    
    for ((i=1; i<=runs; i++)); do
        # Warmup on first run
        if [[ $i -eq 1 ]]; then
            "$BIN" $ops "$dataset_path" >/dev/null 2>&1 || true
        fi
        
        # Measure time with high precision
        local start_time=$(date +%s.%N)
        "$BIN" $ops "$dataset_path" >/dev/null 2>&1
        local end_time=$(date +%s.%N)
        
        local duration=$(awk "BEGIN {printf \"%.6f\", $end_time - $start_time}")
        times+=("$duration")
    done
    
    # Calculate average
    local sum=0
    for time in "${times[@]}"; do
        sum=$(awk "BEGIN {printf \"%.6f\", $sum + $time}")
    done
    local avg=$(awk "BEGIN {printf \"%.6f\", $sum / ${#times[@]}}")
    
    # Convert to milliseconds for display
    local avg_ms=$(awk "BEGIN {printf \"%.3f\", $avg * 1000}")
    echo "  Average time: ${avg_ms}ms (${avg}s)" >&2
    
    # Store result
    echo "$test_name|$avg"
}

# Run tests
echo "Running tests..."
echo

results=""
for test in "${TESTS[@]}"; do
    IFS='|' read -r name dataset ops <<< "$test"
    result=$(measure_test "$name" "$dataset" "$ops" "$RUNS")
    results+="$result"$'\n'
done

# Output results
case "$FORMAT" in
    "human")
        echo
        echo "Results Summary:"
        echo "================"
        while IFS='|' read -r name time; do
            # Skip empty lines
            if [[ -z "$name" || -z "$time" ]]; then
                continue
            fi
            time_ms=$(awk "BEGIN {printf \"%.3f\", $time * 1000}")
            printf "%-25s: %8.3fms (%6.6fs)\n" "$name" "$time_ms" "$time"
        done <<< "$results"
        ;;
    "json")
        echo "{"
        echo "  \"benchmark\": {"
        echo "    \"version\": \"1.0\","
        echo "    \"timestamp\": \"$(date -u +"%Y-%m-%dT%H:%M:%SZ")\","
        echo "    \"binary\": \"$BIN\","
        echo "    \"results\": {"
        
        first=true
        while IFS='|' read -r name time; do
            # Skip empty lines
            if [[ -z "$name" || -z "$time" ]]; then
                continue
            fi
            
            if [[ "$first" == "true" ]]; then
                first=false
            else
                echo ","
            fi
            time_ms=$(awk "BEGIN {printf \"%.3f\", $time * 1000}")
            echo -n "      \"$name\": {\"time_s\": $time, \"time_ms\": $time_ms}"
        done <<< "$results"
        
        echo
        echo "    }"
        echo "  }"
        echo "}"
        ;;
    *)
        echo "ERROR: Unknown format: $FORMAT" >&2
        exit 1
        ;;
esac

echo
echo "Benchmark complete!"

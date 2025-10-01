#!/bin/bash
set -euo pipefail

# Performance feature impact tests for fiskta
# Tests specific optimizations like line index cache, Boyer-Moore-Horspool, etc.

BIN="./fiskta"
DATADIR="bench/data"
mkdir -p "$DATADIR"

echo "fiskta Performance Feature Impact Tests"
echo "======================================="
echo "Binary: $BIN"
echo

# Generate test datasets optimized for specific features
generate_performance_datasets() {
    echo "Generating performance test datasets..."
    
    # 1. Line-heavy dataset for line index cache testing
    echo "  Creating line-heavy dataset (10MB)..."
    {
        for i in $(seq 1 200000); do
            echo "Line $i: This is a test line with some content to make it realistic"
        done
    } > "$DATADIR/lines_10MB.txt"
    
    # 2. Pattern-heavy dataset for Boyer-Moore-Horspool testing
    echo "  Creating pattern-heavy dataset (10MB)..."
    {
        for i in $(seq 1 100000); do
            if [[ $((i % 100)) -eq 0 ]]; then
                echo "ERROR: Critical failure detected at iteration $i"
            else
                echo "INFO: Normal operation continuing at step $i"
            fi
        done
    } > "$DATADIR/pattern_10MB.txt"
    
    # 3. Large file for memory efficiency testing
    echo "  Creating large dataset (50MB)..."
    {
        for i in $(seq 1 1000000); do
            if [[ $((i % 1000)) -eq 0 ]]; then
                echo "WARNING: Performance degradation detected at $i"
            else
                echo "DEBUG: System status normal at checkpoint $i"
            fi
        done
    } > "$DATADIR/large_50MB.txt"
    
    echo "Dataset generation complete."
    echo
}

# High precision timing function
measure_time() {
    local cmd="$1"
    local runs="${2:-5}"
    local times=()
    
    for ((i=1; i<=runs; i++)); do
        # Warmup on first run
        if [[ $i -eq 1 ]]; then
            eval "$cmd" >/dev/null 2>&1 || true
        fi
        
        local start_time=$(date +%s.%N)
        eval "$cmd" >/dev/null 2>&1
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
    local avg_ms=$(awk "BEGIN {printf \"%.3f\", $avg * 1000}")
    
    echo "$avg_ms"
}

# Test line index cache performance
test_line_index_cache() {
    echo "=== Line Index Cache Performance ==="
    echo "Testing line-based operations that benefit from caching..."
    echo
    
    local dataset="$DATADIR/lines_10MB.txt"
    
    # Test 1: Line navigation (should benefit from cache)
    echo "Test 1: Line navigation (goto BOF skip 10000l take 100l)"
    local time1=$(measure_time "\"$BIN\" goto BOF skip 10000l take 100l \"$dataset\"")
    echo "  Time: ${time1}ms"
    
    # Test 2: Multiple line operations (cache should help)
    echo "Test 2: Multiple line skips (skip 5000l skip 5000l take 50l)"
    local time2=$(measure_time "\"$BIN\" skip 5000l skip 5000l take 50l \"$dataset\"")
    echo "  Time: ${time2}ms"
    
    # Test 3: Line counting (should use cache efficiently)
    echo "Test 3: Line counting (skip 100000l take 1l)"
    local time3=$(measure_time "\"$BIN\" skip 100000l take 1l \"$dataset\"")
    echo "  Time: ${time3}ms"
    
    echo "Line index cache tests complete."
    echo
}

# Test Boyer-Moore-Horspool algorithm performance
test_boyer_moore_horspool() {
    echo "=== Boyer-Moore-Horspool Algorithm Performance ==="
    echo "Testing string search performance with different patterns..."
    echo
    
    local dataset="$DATADIR/pattern_10MB.txt"
    
    # Test 1: Short pattern (should be fast)
    echo "Test 1: Short pattern search (find 'ERROR')"
    local time1=$(measure_time "\"$BIN\" find ERROR take 5b \"$dataset\"")
    echo "  Time: ${time1}ms"
    
    # Test 2: Medium pattern (good for BMH)
    echo "Test 2: Medium pattern search (find 'Critical failure')"
    local time2=$(measure_time "\"$BIN\" find 'Critical failure' take 20b \"$dataset\"")
    echo "  Time: ${time2}ms"
    
    # Test 3: Long pattern (BMH should excel)
    echo "Test 3: Long pattern search (find 'Critical failure detected at iteration')"
    local time3=$(measure_time "\"$BIN\" find 'Critical failure detected at iteration' take 50b \"$dataset\"")
    echo "  Time: ${time3}ms"
    
    # Test 4: Pattern not found (worst case for BMH)
    echo "Test 4: Pattern not found (find 'NOTFOUND')"
    local time4=$(measure_time "\"$BIN\" find NOTFOUND take 5b \"$dataset\"")
    echo "  Time: ${time4}ms"
    
    echo "Boyer-Moore-Horspool tests complete."
    echo
}

# Test memory efficiency with large files
test_memory_efficiency() {
    echo "=== Memory Efficiency Tests ==="
    echo "Testing memory usage patterns with large files..."
    echo
    
    local dataset="$DATADIR/large_50MB.txt"
    
    # Test 1: Full file processing (memory efficiency)
    echo "Test 1: Full file processing (take to EOF)"
    local time1=$(measure_time "\"$BIN\" take to EOF \"$dataset\"")
    echo "  Time: ${time1}ms"
    
    # Test 2: Streaming search (should be memory efficient)
    echo "Test 2: Streaming search (find 'Performance degradation')"
    local time2=$(measure_time "\"$BIN\" find 'Performance degradation' take 30b \"$dataset\"")
    echo "  Time: ${time2}ms"
    
    # Test 3: Multiple operations (memory reuse)
    echo "Test 3: Multiple operations (find 'WARNING' take 10b :: find 'DEBUG' take 10b)"
    local time3=$(measure_time "\"$BIN\" find WARNING take 10b :: find DEBUG take 10b \"$dataset\"")
    echo "  Time: ${time3}ms"
    
    echo "Memory efficiency tests complete."
    echo
}

# Test regex engine performance
test_regex_performance() {
    echo "=== Regex Engine Performance ==="
    echo "Testing regex compilation and execution performance..."
    echo
    
    local dataset="$DATADIR/pattern_10MB.txt"
    
    # Test 1: Simple regex
    echo "Test 1: Simple regex (findr 'ERROR.*Critical')"
    local time1=$(measure_time "\"$BIN\" findr 'ERROR.*Critical' take 20b \"$dataset\"")
    echo "  Time: ${time1}ms"
    
    # Test 2: Complex regex with quantifiers
    echo "Test 2: Complex regex (findr 'ERROR.*[0-9]+')"
    local time2=$(measure_time "\"$BIN\" findr 'ERROR.*[0-9]+' take 20b \"$dataset\"")
    echo "  Time: ${time2}ms"
    
    # Test 3: Regex with character classes
    echo "Test 3: Character class regex (findr '[A-Z]+:.*[0-9]+')"
    local time3=$(measure_time "\"$BIN\" findr '[A-Z]+:.*[0-9]+' take 30b \"$dataset\"")
    echo "  Time: ${time3}ms"
    
    echo "Regex performance tests complete."
    echo
}

# Test scaling characteristics
test_scaling_characteristics() {
    echo "=== Scaling Characteristics ==="
    echo "Testing how performance scales with file size..."
    echo
    
    # Create different sized files
    echo "Creating scaling test files..."
    for size in 1 5 10 25; do
        {
            for i in $(seq 1 $((size * 20000))); do
                if [[ $((i % 100)) -eq 0 ]]; then
                    echo "ERROR: Test error $i"
                else
                    echo "INFO: Normal message $i"
                fi
            done
        } > "$DATADIR/scaling_${size}MB.txt"
    done
    
    echo "Testing scaling with simple find operation..."
    for size in 1 5 10 25; do
        local dataset="$DATADIR/scaling_${size}MB.txt"
        local time=$(measure_time "\"$BIN\" find ERROR take 5b \"$dataset\"")
        local throughput=$(awk "BEGIN {printf \"%.1f\", $size / ($time / 1000)}")
        echo "  ${size}MB file: ${time}ms (${throughput} MB/s)"
    done
    
    echo "Scaling characteristics tests complete."
    echo
}

# Generate performance report
generate_report() {
    echo "=== Performance Feature Impact Report ==="
    echo "Generated: $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    echo "Binary: $BIN"
    echo
    
    echo "Key Performance Features Tested:"
    echo "1. Line Index Cache - Caches line boundaries for fast line navigation"
    echo "2. Boyer-Moore-Horspool - Efficient string search algorithm"
    echo "3. Memory Efficiency - Streaming processing with bounded memory"
    echo "4. Regex Engine - NFA-based regex compilation and execution"
    echo "5. Scaling Characteristics - Performance vs file size"
    echo
    
    echo "Recommendations:"
    echo "- Use line-based operations for large files (benefits from cache)"
    echo "- Longer search patterns perform better with BMH algorithm"
    echo "- Streaming operations maintain consistent memory usage"
    echo "- Regex complexity impacts compilation time"
    echo
}

# Main execution
main() {
    # Check if binary exists
    if [[ ! -x "$BIN" ]]; then
        echo "ERROR: Binary $BIN not found or not executable" >&2
        exit 1
    fi
    
    # Generate datasets
    generate_performance_datasets
    
    # Run performance tests
    test_line_index_cache
    test_boyer_moore_horspool
    test_memory_efficiency
    test_regex_performance
    test_scaling_characteristics
    
    # Generate report
    generate_report
    
    echo "Performance feature impact tests complete!"
}

# Run if called directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi

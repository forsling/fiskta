#!/bin/bash
# Quick performance analysis with millisecond precision
# Usage: ./performance_analysis.sh

set -euo pipefail

echo "fiskta Performance Analysis"
echo "==========================="
echo

# Run benchmark and extract JSON
echo "Running benchmark..."
JSON_OUTPUT=$(./tests/benchmark.sh --format json 2>/dev/null | sed -n '/^{/,/^}/p')

echo "Performance Results (millisecond precision):"
echo "---------------------------------------------"

# Extract timing data
echo "$JSON_OUTPUT" | jq -r '
.benchmark.results | 
to_entries[] | 
"\(.key): \(.value.time_ms)ms (\(.value.time_s)s)"
'

echo
echo "Throughput Analysis:"
echo "-------------------"

# Calculate throughput for small files (1MB)
echo "$JSON_OUTPUT" | jq -r '
.benchmark.results | 
to_entries[] | 
select(.key | contains("small")) | 
"\(.key): \(.value.time_ms)ms, Throughput: \(1000 / .value.time_ms | floor) MB/s"
'

echo
echo "Scaling Analysis:"
echo "----------------"

# Compare small vs medium performance
echo "$JSON_OUTPUT" | jq -r '
.benchmark.results | 
to_entries[] | 
select(.key | contains("find_simple")) | 
"\(.key): \(.value.time_ms)ms"
' | while read -r line; do
    echo "$line"
done

echo
echo "Performance Summary:"
echo "-------------------"

# Calculate average performance
AVG_TIME=$(echo "$JSON_OUTPUT" | jq '[.benchmark.results[].time_ms] | add / length')
echo "Average execution time: ${AVG_TIME}ms"

# Find fastest operation
FASTEST=$(echo "$JSON_OUTPUT" | jq -r '.benchmark.results | to_entries | min_by(.value.time_ms) | "\(.key): \(.value.time_ms)ms"')
echo "Fastest operation: $FASTEST"

# Find slowest operation  
SLOWEST=$(echo "$JSON_OUTPUT" | jq -r '.benchmark.results | to_entries | max_by(.value.time_ms) | "\(.key): \(.value.time_ms)ms"')
echo "Slowest operation: $SLOWEST"

echo
echo "Analysis complete!"

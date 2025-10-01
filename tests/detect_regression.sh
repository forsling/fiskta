#!/bin/bash
# Regression detection script for fiskta benchmarks
# Usage: ./detect_regression.sh <current_results.json> <baseline_results.json> [threshold_percent]

set -euo pipefail

CURRENT_FILE="$1"
BASELINE_FILE="$2"
THRESHOLD="${3:-10}"  # Default 10% threshold

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <current_results.json> <baseline_results.json> [threshold_percent]"
    echo "Example: $0 current.json baseline.json 15"
    exit 1
fi

if [[ ! -f "$CURRENT_FILE" ]]; then
    echo "ERROR: Current results file $CURRENT_FILE not found"
    exit 1
fi

if [[ ! -f "$BASELINE_FILE" ]]; then
    echo "ERROR: Baseline results file $BASELINE_FILE not found"
    exit 1
fi

# Check if jq is available
if ! command -v jq >/dev/null 2>&1; then
    echo "ERROR: jq not found (required for JSON processing)"
    exit 1
fi

echo "Regression Detection Report"
echo "=========================="
echo "Current: $CURRENT_FILE"
echo "Baseline: $BASELINE_FILE"
echo "Threshold: ${THRESHOLD}%"
echo

# Extract test results from both files
CURRENT_RESULTS=$(jq -r '.benchmark.results | to_entries[] | select(.value.failures == 0) | "\(.key)|\(.value.wall_time.avg)|\(.value.throughput_mbps)|\(.value.peak_memory_kb.avg)"' "$CURRENT_FILE")
BASELINE_RESULTS=$(jq -r '.benchmark.results | to_entries[] | select(.value.failures == 0) | "\(.key)|\(.value.wall_time.avg)|\(.value.throughput_mbps)|\(.value.peak_memory_kb.avg)"' "$BASELINE_FILE")

# Create associative arrays for lookup
declare -A current_times current_throughputs current_memory
declare -A baseline_times baseline_throughputs baseline_memory

# Parse current results
while IFS='|' read -r test_name wall_time throughput memory; do
    current_times["$test_name"]="$wall_time"
    current_throughputs["$test_name"]="$throughput"
    current_memory["$test_name"]="$memory"
done <<< "$CURRENT_RESULTS"

# Parse baseline results
while IFS='|' read -r test_name wall_time throughput memory; do
    baseline_times["$test_name"]="$wall_time"
    baseline_throughputs["$test_name"]="$throughput"
    baseline_memory["$test_name"]="$memory"
done <<< "$BASELINE_RESULTS"

# Check for regressions
regressions_found=false
improvements_found=false

echo "Performance Changes:"
echo "-------------------"

for test_name in "${!current_times[@]}"; do
    if [[ -n "${baseline_times[$test_name]:-}" ]]; then
        current_time="${current_times[$test_name]}"
        baseline_time="${baseline_times[$test_name]}"
        current_throughput="${current_throughputs[$test_name]}"
        baseline_throughput="${baseline_throughputs[$test_name]}"
        current_mem="${current_memory[$test_name]}"
        baseline_mem="${baseline_memory[$test_name]}"
        
        # Calculate percentage changes
        time_change=$(echo "scale=2; (($current_time - $baseline_time) / $baseline_time) * 100" | bc -l)
        throughput_change=$(echo "scale=2; (($current_throughput - $baseline_throughput) / $baseline_throughput) * 100" | bc -l)
        memory_change=$(echo "scale=2; (($current_mem - $baseline_mem) / $baseline_mem) * 100" | bc -l)
        
        # Check for regressions (worse performance)
        time_regression=false
        throughput_regression=false
        memory_regression=false
        
        if [[ $(echo "$time_change > $THRESHOLD" | bc -l) -eq 1 ]]; then
            time_regression=true
        fi
        
        if [[ $(echo "$throughput_change < -$THRESHOLD" | bc -l) -eq 1 ]]; then
            throughput_regression=true
        fi
        
        if [[ $(echo "$memory_change > $THRESHOLD" | bc -l) -eq 1 ]]; then
            memory_regression=true
        fi
        
        # Check for improvements (better performance)
        time_improvement=false
        throughput_improvement=false
        memory_improvement=false
        
        if [[ $(echo "$time_change < -$THRESHOLD" | bc -l) -eq 1 ]]; then
            time_improvement=true
        fi
        
        if [[ $(echo "$throughput_change > $THRESHOLD" | bc -l) -eq 1 ]]; then
            throughput_improvement=true
        fi
        
        if [[ $(echo "$memory_change < -$THRESHOLD" | bc -l) -eq 1 ]]; then
            memory_improvement=true
        fi
        
        # Print results
        printf "%-30s: " "$test_name"
        
        if [[ "$time_regression" == "true" || "$throughput_regression" == "true" || "$memory_regression" == "true" ]]; then
            printf "🔴 REGRESSION "
            regressions_found=true
        elif [[ "$time_improvement" == "true" || "$throughput_improvement" == "true" || "$memory_improvement" == "true" ]]; then
            printf "🟢 IMPROVEMENT "
            improvements_found=true
        else
            printf "🟡 NO CHANGE "
        fi
        
        printf "(Time: %+5.1f%%, Throughput: %+5.1f%%, Memory: %+5.1f%%)\n" \
            "$time_change" "$throughput_change" "$memory_change"
    fi
done

echo

# Summary
if [[ "$regressions_found" == "true" ]]; then
    echo "❌ REGRESSIONS DETECTED!"
    echo "Performance has degraded beyond the ${THRESHOLD}% threshold."
    exit 1
elif [[ "$improvements_found" == "true" ]]; then
    echo "✅ IMPROVEMENTS DETECTED!"
    echo "Performance has improved beyond the ${THRESHOLD}% threshold."
    exit 0
else
    echo "✅ NO SIGNIFICANT CHANGES"
    echo "Performance is within the ${THRESHOLD}% threshold."
    exit 0
fi

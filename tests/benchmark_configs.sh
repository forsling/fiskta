# Benchmark Configuration Examples for fiskta
# Copy and modify these configurations for different testing scenarios

# Quick Development Testing (fast feedback)
QUICK_CONFIG="
--small 1
--medium 5
--large 20
--runs 3
--warmup 1
--format human
"

# Comprehensive Testing (thorough analysis)
COMPREHENSIVE_CONFIG="
--small 1
--medium 10
--large 100
--runs 10
--warmup 3
--compare
--format json
--output results.json
"

# Performance Regression Testing (CI/CD)
REGRESSION_CONFIG="
--small 1
--medium 10
--runs 5
--warmup 2
--format json
--output regression_results.json
"

# Memory Profiling (detailed memory analysis)
MEMORY_CONFIG="
--small 1
--medium 10
--large 50
--runs 3
--warmup 1
--profile
--format json
--output memory_profile.json
"

# Comparative Analysis (vs other tools)
COMPARISON_CONFIG="
--small 1
--medium 10
--large 100
--runs 5
--warmup 2
--compare
--format markdown
--output comparison_report.md
"

# Stress Testing (large files)
STRESS_CONFIG="
--small 10
--medium 100
--large 1000
--runs 3
--warmup 1
--format json
--output stress_test.json
"

# Usage Examples:
# ./benchmark.sh $QUICK_CONFIG
# ./benchmark.sh $COMPREHENSIVE_CONFIG
# ./benchmark.sh $REGRESSION_CONFIG
# ./benchmark.sh $MEMORY_CONFIG
# ./benchmark.sh $COMPARISON_CONFIG
# ./benchmark.sh $STRESS_CONFIG

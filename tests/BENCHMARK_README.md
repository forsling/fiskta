# fiskta Benchmarking System

A clean, reliable benchmarking suite for the fiskta text extraction tool with millisecond precision timing.

## Features

- **Millisecond Precision**: High-accuracy timing measurements
- **Multi-scale Testing**: Test performance across different file sizes (1MB, 10MB+)
- **Statistical Analysis**: Multiple runs with proper warmup
- **Multiple Output Formats**: Human-readable and JSON
- **Performance Analysis**: Throughput calculations and scaling analysis
- **Clean Implementation**: Simple, reliable, easy to understand

## Quick Start

### Basic Benchmark
```bash
# Run basic benchmark with human-readable output
./tests/benchmark.sh

# Run with different file sizes
./tests/benchmark.sh --small 1 --medium 10

# JSON output for analysis
./tests/benchmark.sh --format json
```

### Using Configuration Presets
```bash
# Load configuration presets
source tests/benchmark_configs.sh

# Quick development testing
./tests/benchmark.sh $QUICK_CONFIG

# Comprehensive testing
./tests/benchmark.sh $COMPREHENSIVE_CONFIG

# Performance regression testing
./tests/benchmark.sh $REGRESSION_CONFIG
```

### Advanced Analysis
```bash
# Generate JSON results
./tests/benchmark.sh --format json --output results.json

# Analyze results with Python
python3 tests/benchmark_analysis.py results.json --output-dir analysis/

# Detect performance regressions
./tests/detect_regression.sh current.json baseline.json 10
```

## Command Line Options

### Basic Options
- `-s, --small SIZE`: Small file size in MB (default: 1)
- `-m, --medium SIZE`: Medium file size in MB (default: 10)
- `-l, --large SIZE`: Large file size in MB (default: 100)
- `-r, --runs N`: Number of runs per test (default: 5)
- `-w, --warmup N`: Number of warmup runs (default: 2)
- `-q, --quiet`: Suppress verbose output

### Advanced Options
- `-c, --compare`: Enable comparative analysis with grep/awk/sed
- `-f, --format FORMAT`: Output format (human, csv, json, markdown)
- `-o, --output FILE`: Output file (default: stdout)
- `-p, --profile`: Enable profiling with perf
- `-h, --help`: Show help

## Test Cases

The benchmark includes comprehensive test cases covering:

### Core Operations
- **Simple Find**: Basic text search performance
- **Regex Find**: Regular expression search performance
- **Take Throughput**: Full file processing throughput
- **Line Navigation**: Line-based operations
- **UTF-8 Processing**: Unicode character handling
- **Binary Processing**: Binary data handling

### Advanced Features
- **Backward Search**: Reverse search with windowing
- **Take Until**: Boundary-based extraction
- **Clause Operations**: Complex multi-operation sequences
- **Memory Efficiency**: Memory usage patterns

### Comparative Analysis
- **vs grep**: Simple text search comparison
- **vs awk**: Pattern matching comparison
- **vs sed**: Stream processing comparison

## Output Formats

### Human-Readable
```
fiskta Benchmark Results
=======================
Configuration: Small=1MB, Medium=10MB, Large=100MB, Runs=5, Warmups=2

find_simple_small (1MB): Simple text search on small file
  ✓ Wall: 0.045s (0.043-0.047) | User: 0.042s (0.040-0.044) | Sys: 0.003s (0.002-0.004)
  Memory: 2100KB (2000-2200) | Throughput: 22.2 MB/s | Efficiency: 2.100 MB/MB
```

### CSV Format
```csv
test_name,file_size_mb,wall_time_avg,throughput_mbps,peak_memory_kb
find_simple_small,1,0.045,22.2,2100
find_simple_medium,10,0.450,22.2,2100
```

### JSON Format
```json
{
  "benchmark": {
    "version": "2.0",
    "timestamp": "2024-01-15T10:30:00Z",
    "results": {
      "find_simple_small": {
        "wall_time": {"avg": 0.045, "min": 0.043, "max": 0.047},
        "throughput_mbps": 22.2,
        "peak_memory_kb": {"avg": 2100}
      }
    }
  }
}
```

### Markdown Format
```markdown
# fiskta Benchmark Results

| Test | Size | Wall Time (s) | Throughput (MB/s) | Memory (KB) |
|------|------|---------------|-------------------|------------|
| find_simple_small | 1MB | 0.045 (0.043-0.047) | 22.2 | 2100 (2000-2200) |
```

## Performance Metrics

### Core Metrics
- **Wall Time**: Total elapsed time
- **User Time**: CPU time spent in user mode
- **System Time**: CPU time spent in system mode
- **Peak Memory**: Maximum resident set size

### Derived Metrics
- **Throughput**: MB/s processing rate
- **Memory Efficiency**: Peak memory per MB processed
- **Scaling Efficiency**: Performance scaling with file size

### Comparative Metrics
- **Speedup vs grep**: Performance improvement over grep
- **Speedup vs awk**: Performance improvement over awk
- **Memory vs alternatives**: Memory efficiency comparison

## Regression Detection

The regression detection script compares current results against a baseline:

```bash
# Detect regressions with 10% threshold
./tests/detect_regression.sh current.json baseline.json 10

# Output example:
# ❌ REGRESSIONS DETECTED!
# find_simple_medium: 🔴 REGRESSION (Time: +15.2%, Throughput: -12.1%, Memory: +8.3%)
```

## Visualization and Analysis

The Python analysis script provides:

### Performance Analysis
- **Scalability Analysis**: How performance scales with file size
- **Memory Efficiency**: Memory usage patterns
- **Comparative Performance**: Charts comparing fiskta vs other tools

### Generated Charts
- **Throughput vs File Size**: Performance scaling
- **Memory Usage vs File Size**: Memory scaling
- **Performance Comparison**: Bar charts vs other tools
- **Memory Efficiency Heatmap**: Efficiency across test cases

### HTML Reports
- **Interactive Reports**: Web-based performance reports
- **Color-coded Metrics**: Visual performance indicators
- **Trend Analysis**: Historical performance tracking

## CI/CD Integration

### GitHub Actions Example
```yaml
name: Performance Benchmark
on: [push, pull_request]

jobs:
  benchmark:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build fiskta
        run: make
      - name: Run benchmark
        run: |
          ./tests/benchmark.sh --format json --output results.json
      - name: Detect regressions
        run: |
          ./tests/detect_regression.sh results.json baseline.json 15
        continue-on-error: true
      - name: Upload results
        uses: actions/upload-artifact@v3
        with:
          name: benchmark-results
          path: results.json
```

### Automated Baseline Updates
```bash
# Update baseline after performance improvements
cp results.json baseline.json
git add baseline.json
git commit -m "Update performance baseline"
```

## Troubleshooting

### Common Issues

1. **Missing Dependencies**
   ```bash
   # Install required tools
   sudo apt-get install time bc jq
   
   # For Python analysis
   pip install matplotlib pandas seaborn
   ```

2. **Permission Errors**
   ```bash
   # Make scripts executable
   chmod +x tests/benchmark.sh
   chmod +x tests/benchmark_analysis.py
   chmod +x tests/detect_regression.sh
   ```

3. **Large File Generation**
   ```bash
   # For stress testing, ensure sufficient disk space
   df -h
   
   # Monitor during generation
   watch -n 1 'ls -lh tests/bench/data/'
   ```

### Performance Tips

1. **Consistent Environment**: Run benchmarks on dedicated hardware
2. **Warmup Runs**: Use sufficient warmup runs to stabilize performance
3. **Multiple Runs**: Use multiple runs for statistical significance
4. **Baseline Updates**: Regularly update baselines after optimizations

## Contributing

To add new test cases or improve the benchmarking system:

1. **Add Test Cases**: Edit the `CASES` array in `benchmark.sh`
2. **Add Metrics**: Extend the measurement functions
3. **Add Visualizations**: Enhance the Python analysis script
4. **Add Comparisons**: Include new tools in comparative analysis

## Examples

### Development Workflow
```bash
# Quick test during development
./tests/benchmark.sh $QUICK_CONFIG

# Comprehensive test before release
./tests/benchmark.sh $COMPREHENSIVE_CONFIG

# Generate analysis report
python3 tests/benchmark_analysis.py results.json --html-report report.html
```

### Performance Investigation
```bash
# Profile specific operations
./tests/benchmark.sh --profile --format json --output profile.json

# Compare against previous version
./tests/detect_regression.sh current.json previous.json 5

# Generate detailed analysis
python3 tests/benchmark_analysis.py profile.json --output-dir analysis/
```

This benchmarking system provides comprehensive performance analysis for fiskta, enabling confident performance optimization and regression detection.

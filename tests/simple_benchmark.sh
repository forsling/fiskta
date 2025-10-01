#!/bin/bash
set -euo pipefail

# Simple test version of benchmark script
BIN="./fiskta"
DATADIR="bench/data"
mkdir -p "$DATADIR"

echo "Creating test dataset..."
echo -e "line 1\nERROR line 2\nline 3\nERROR line 4\nline 5" > "$DATADIR/test.txt"

echo "Testing fiskta..."
echo "Test 1: Simple find"
time ./fiskta find ERROR take 5b "$DATADIR/test.txt"

echo "Test 2: Regex find"  
time ./fiskta findr 'ERROR.*line' take 10b "$DATADIR/test.txt"

echo "Test 3: Take throughput"
time ./fiskta take to EOF "$DATADIR/test.txt"

echo "Benchmark test complete!"

#!/bin/bash
# Edge case tests for fiskta
# These tests verify specific edge cases mentioned in feedback

set -e

# Get the fiskta binary path
FISKTA="${1:-./fiskta}"

if [ ! -f "$FISKTA" ]; then
    echo "Error: fiskta binary not found at $FISKTA"
    exit 1
fi

echo "Running edge case tests with $FISKTA"
echo "======================================"

# Test 1: Inline-offset label resolution in preflight/build
echo "Test 1: Inline-offset label resolution"
printf 'a\nb\nX\n' > /tmp/test1.txt
if "$FISKTA" 'label HERE :: goto HERE+1l take 1l' /tmp/test1.txt | grep -q 'X'; then
    echo "✓ PASS: Inline-offset label resolution works"
else
    echo "✗ FAIL: Inline-offset label resolution failed"
    exit 1
fi

# Test 2: Backward window find → rightmost
echo "Test 2: Backward window find → rightmost"
printf 'aaa ERROR aaa ERROR aaa' > /tmp/test2.txt
result="$("$FISKTA" 'skip 15b find to BOF "ERROR" take 5b' /tmp/test2.txt)"
if [ "$result" = "ERROR" ]; then
    echo "✓ PASS: Backward window find captures rightmost match"
else
    echo "✗ FAIL: Backward window find failed (got: '$result')"
    exit 1
fi

# Test 3: take until (empty span must not move cursor)
echo "Test 3: take until (empty span must not move cursor)"
printf 'HEAD\nM1\nM2\n' > /tmp/test3.txt
result="$("$FISKTA" 'take until "HEAD" :: take 4b' /tmp/test3.txt)"
if [ "$result" = "HEAD" ]; then
    echo "✓ PASS: take until with empty span works correctly"
else
    echo "✗ FAIL: take until with empty span failed (got: '$result')"
    exit 1
fi

# Test 4: UTF-8 chopped boundary handling
echo "Test 4: UTF-8 chopped boundary handling"
printf '\xF0\x9F\x98\x80X' | "$FISKTA" 'take 1c take 1c' - > /tmp/test4_result.txt
if [ -s /tmp/test4_result.txt ]; then
    echo "✓ PASS: UTF-8 chopped boundary handling works"
else
    echo "✗ FAIL: UTF-8 chopped boundary handling failed"
    exit 1
fi

# Test 5: Lines anchor (negative)
echo "Test 5: Lines anchor (negative)"
printf 'L1\nL2\nL3\nL4\n' > /tmp/test5.txt
result="$("$FISKTA" 'skip 7b take -2l' /tmp/test5.txt)"
if echo "$result" | grep -q "L1" && echo "$result" | grep -q "L2"; then
    echo "✓ PASS: Lines anchor (negative) works"
else
    echo "✗ FAIL: Lines anchor (negative) failed (got: '$result')"
    exit 1
fi

# Cleanup
rm -f /tmp/test1.txt /tmp/test2.txt /tmp/test3.txt /tmp/test4_result.txt /tmp/test5.txt

echo "======================================"
echo "All edge case tests passed!"

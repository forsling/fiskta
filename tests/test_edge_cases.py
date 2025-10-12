#!/usr/bin/env python3
"""
Edge case tests for fiskta
These tests verify specific edge cases mentioned in feedback
"""

import subprocess
import tempfile
import os
import sys

def run_fiskta(ops, input_data=None, input_file=None):
    """Run fiskta with given operations and input"""
    cmd = ["./fiskta", "--ops", ops]
    if input_file:
        cmd.extend(["--input", input_file])

    if input_data:
        process = subprocess.run(
            cmd,
            input=input_data,
            capture_output=True,
            text=True
        )
    else:
        process = subprocess.run(cmd, capture_output=True, text=True)

    return process.returncode, process.stdout, process.stderr

def test_inline_offset_label_resolution():
    """Test 1: Inline-offset label resolution in preflight/build"""
    with tempfile.NamedTemporaryFile(mode='w', delete=False, suffix='.txt') as f:
        f.write('a\nb\nX\n')
        temp_file = f.name

    try:
        returncode, stdout, stderr = run_fiskta('label HERE :: goto HERE+1l take 1l', input_file=temp_file)
        if returncode == 0 and 'X' in stdout:
            print("✓ PASS: Inline-offset label resolution works")
            return True
        else:
            print(f"✗ FAIL: Inline-offset label resolution failed (returncode: {returncode}, stdout: '{stdout}', stderr: '{stderr}')")
            return False
    finally:
        os.unlink(temp_file)

def test_backward_window_find():
    """Test 2: Backward window find → rightmost"""
    with tempfile.NamedTemporaryFile(mode='w', delete=False, suffix='.txt') as f:
        f.write('aaa ERROR aaa ERROR aaa')
        temp_file = f.name

    try:
        returncode, stdout, stderr = run_fiskta('skip 15b find to BOF "ERROR" take 5b', input_file=temp_file)
        if returncode == 0 and stdout.strip() == 'ERROR':
            print("✓ PASS: Backward window find captures rightmost match")
            return True
        else:
            print(f"✗ FAIL: Backward window find failed (returncode: {returncode}, stdout: '{stdout}', stderr: '{stderr}')")
            return False
    finally:
        os.unlink(temp_file)

def test_take_until_empty_span():
    """Test 3: take until (empty span must not move cursor)"""
    with tempfile.NamedTemporaryFile(mode='w', delete=False, suffix='.txt') as f:
        f.write('HEAD\nM1\nM2\n')
        temp_file = f.name

    try:
        returncode, stdout, stderr = run_fiskta('take until "HEAD" :: take 4b', input_file=temp_file)
        if returncode == 0 and stdout.strip() == 'HEAD':
            print("✓ PASS: take until with empty span works correctly")
            return True
        else:
            print(f"✗ FAIL: take until with empty span failed (returncode: {returncode}, stdout: '{stdout}', stderr: '{stderr}')")
            return False
    finally:
        os.unlink(temp_file)

def test_utf8_chopped_boundary():
    """Test 4: UTF-8 chopped boundary handling"""
    # Create UTF-8 emoji (😀) followed by 'X'
    utf8_data = bytes([0xF0, 0x9F, 0x98, 0x80]) + b'X'

    returncode, stdout, stderr = run_fiskta('take 1c take 1c', input_data=utf8_data)
    if returncode == 0 and len(stdout) > 0:
        print("✓ PASS: UTF-8 chopped boundary handling works")
        return True
    else:
        print(f"✗ FAIL: UTF-8 chopped boundary handling failed (returncode: {returncode}, stdout: '{stdout}', stderr: '{stderr}')")
        return False

def test_lines_anchor_negative():
    """Test 5: Lines anchor (negative)"""
    with tempfile.NamedTemporaryFile(mode='w', delete=False, suffix='.txt') as f:
        f.write('L1\nL2\nL3\nL4\n')
        temp_file = f.name

    try:
        returncode, stdout, stderr = run_fiskta('skip 7b take -2l', input_file=temp_file)
        if returncode == 0 and 'L1' in stdout and 'L2' in stdout:
            print("✓ PASS: Lines anchor (negative) works")
            return True
        else:
            print(f"✗ FAIL: Lines anchor (negative) failed (returncode: {returncode}, stdout: '{stdout}', stderr: '{stderr}')")
            return False
    finally:
        os.unlink(temp_file)

def main():
    """Run all edge case tests"""
    print("Running edge case tests")
    print("======================")

    tests = [
        test_inline_offset_label_resolution,
        test_backward_window_find,
        test_take_until_empty_span,
        test_utf8_chopped_boundary,
        test_lines_anchor_negative,
    ]

    passed = 0
    total = len(tests)

    for test in tests:
        if test():
            passed += 1

    print("======================")
    print(f"Tests passed: {passed}/{total}")

    if passed == total:
        print("All edge case tests passed!")
        return 0
    else:
        print("Some tests failed!")
        return 1

if __name__ == "__main__":
    sys.exit(main())

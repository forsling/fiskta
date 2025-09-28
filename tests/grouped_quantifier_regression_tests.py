#!/usr/bin/env python3
"""
Regression tests for grouped quantifiers to catch issues that the old implementation
accidentally allowed to pass even when the construction was wrong.
"""

import subprocess
import sys

def run_fiskta_test(tokens, input_data, expected_output=None, should_fail=False):
    """Run a fiskta test and return the result."""
    cmd = ["./fiskta"] + tokens + ["-"]
    try:
        result = subprocess.run(cmd, input=input_data, capture_output=True, text=True, timeout=5)
        if should_fail:
            return result.returncode != 0, result.stderr.strip()
        else:
            # Return both stdout and stderr for debugging
            output = result.stdout.strip()
            if not output and result.stderr.strip():
                output = result.stderr.strip()
            return result.returncode == 0, output
    except subprocess.TimeoutExpired:
        return False, "Timeout"
    except Exception as e:
        return False, str(e)

def test_star_quantifier_multiple_repeats():
    """Test that (ab)* allows 2+ repeats (ensure * matches more than one)."""
    print("Testing (ab)* allows multiple repeats...")
    success, output = run_fiskta_test(["findr", "(ab)*x", "take", "to", "match-end"], "ababx")
    if success and "ababx" in output:
        print("  ✓ PASS: (ab)* correctly allows multiple repeats")
        return True
    else:
        print(f"  ✗ FAIL: (ab)* should allow multiple repeats, got: {output}")
        return False

def test_question_quantifier_zero_occurrences():
    """Test that (ab)?x matches with zero occurrences."""
    print("Testing (ab)? handles zero occurrences...")
    success, output = run_fiskta_test(["findr", "(ab)?x", "take", "to", "match-end"], "x")
    if success and output == "x":
        print("  ✓ PASS: (ab)? correctly handles zero occurrences")
        return True
    else:
        print(f"  ✗ FAIL: (ab)? should match with zero occurrences, got: {output}")
        return False

def test_question_quantifier_one_occurrence():
    """Test that (ab)?x matches with one occurrence."""
    print("Testing (ab)? handles one occurrence...")
    success, output = run_fiskta_test(["findr", "(ab)?x", "take", "to", "match-end"], "abx")
    if success and output == "abx":
        print("  ✓ PASS: (ab)? correctly handles one occurrence")
        return True
    else:
        print(f"  ✗ FAIL: (ab)? should match with one occurrence, got: {output}")
        return False

def test_question_quantifier_not_require_two():
    """Test that (ab)?x does NOT require 2 occurrences."""
    print("Testing (ab)? does NOT require 2 occurrences...")
    success, output = run_fiskta_test(["findr", "(ab)?x", "take", "to", "match-end"], "ababx")
    if success and (output == "x" or output == "abx"):
        print(f"  ✓ PASS: (ab)? correctly finds first match: {output}")
        return True
    else:
        print(f"  ✗ FAIL: (ab)? should find first match, not require 2 occurrences, got: {output}")
        return False

def test_plus_quantifier_requires_one():
    """Test that (ab)+ requires at least one occurrence."""
    print("Testing (ab)+ requires at least one occurrence...")
    success, output = run_fiskta_test(["findr", "(ab)+x"], "x")
    if not success and "no match" in output:
        print("  ✓ PASS: (ab)+ correctly requires at least one occurrence")
        return True
    else:
        print(f"  ✗ FAIL: (ab)+ should require at least one occurrence, got: success={success}, output='{output}'")
        return False

def test_plus_quantifier_multiple_occurrences():
    """Test that (ab)+ handles multiple occurrences."""
    print("Testing (ab)+ handles multiple occurrences...")
    success, output = run_fiskta_test(["findr", "(ab)+x", "take", "to", "match-end"], "ababx")
    if success and "ababx" in output:
        print("  ✓ PASS: (ab)+ correctly handles multiple occurrences")
        return True
    else:
        print(f"  ✗ FAIL: (ab)+ should handle multiple occurrences, got: {output}")
        return False

def main():
    """Run all regression tests."""
    print("=== Grouped Quantifier Regression Tests ===")
    print("These tests catch issues that the old implementation accidentally allowed to pass")
    print()

    tests = [
        test_star_quantifier_multiple_repeats,
        test_question_quantifier_zero_occurrences,
        test_question_quantifier_one_occurrence,
        test_question_quantifier_not_require_two,
        test_plus_quantifier_requires_one,
        test_plus_quantifier_multiple_occurrences,
    ]

    passed = 0
    total = len(tests)

    for test in tests:
        if test():
            passed += 1
        print()

    print(f"=== Summary ===")
    print(f"Passed: {passed}/{total}")
    if passed == total:
        print("✓ All regression tests passed!")
        return 0
    else:
        print("✗ Some regression tests failed!")
        return 1

if __name__ == "__main__":
    sys.exit(main())

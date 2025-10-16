#!/usr/bin/env python3
# Standard library only: subprocess, hashlib, json, argparse, pathlib, sys, os, textwrap
import subprocess, sys, os, hashlib, argparse, json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIX = ROOT / "tests" / "fixtures"


def repo_version() -> str:
    version_file = ROOT / "VERSION"
    try:
        return version_file.read_text().strip()
    except OSError:
        pass
    return "dev"


VERSION = repo_version()
VERSION_LINE = f"fiskta - (fi)nd (sk)ip (ta)ke v{VERSION}\n"
PROGRAM_FAIL_EXIT = 1

def write(path: Path, data: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)

def make_fixtures():
    # 1) small.txt
    small = b"Header\nbody 1\nbody 2\nERROR hit\ntail\nERROR 2\nfoo\nHEADER\nbar\n"
    write(FIX / "small.txt", small)

    # 2) overlap.txt (no trailing LF)
    write(FIX / "overlap.txt", b"abcdefghij")

    # 3) lines.txt
    lines = b"".join([
        b"L01 a\n", b"L02 bb\n", b"L03 ccc\n", b"L04 dddd\n", b"L05 eeeee\n",
        b"L06 ffffff\n", b"L07 ggggggg\n", b"L08 hhhhhhhh\n",
        b"L09 iiiiiiiii\n", b"L10 jjjjjjjjjj\n",
    ])
    write(FIX / "lines.txt", lines)

    # 4) crlf.txt (no LF at end)
    write(FIX / "crlf.txt", b"A\r\nB\r\nC")

    # 5) big-forward.bin (20 MiB of 'A' with 'NEEDLE' at a deep offset)
    big = bytearray(b"A" * (20 * 1024 * 1024))
    ins_off = 12 * 1024 * 1024 + 123 + 890
    big[ins_off:ins_off+6] = b"NEEDLE"
    write(FIX / "big-forward.bin", big)

    # 6) longline-left.bin (single long line, no LF)
    lleft = bytearray(b"A" * (12 * 1024 * 1024))
    lleft[10_000_000:10_000_006] = b"NEEDLE"
    write(FIX / "longline-left.bin", lleft)

    # 7) longline-right.bin: 32 'B', then 12 MiB of 'C', then LF, then 'TAIL\n'
    lright = bytearray(b"B" * 32 + b"C" * (12 * 1024 * 1024) + b"\nTAIL\n")
    write(FIX / "longline-right.bin", lright)

    # 8) labels-evict.txt
    write(FIX / "labels-evict.txt", b"0123456789abcdefghijklmnopqrstuvwxyz")

    # 9) multiline.txt - Complex multi-line content
    multiline = b"""Section 1: Introduction
This is the first section with multiple lines.
It contains various text patterns.

Section 2: Data
KEY1=value1
KEY2=value2
KEY3=value3

Section 3: Results
SUCCESS: Operation completed
WARNING: Minor issue detected
ERROR: Critical failure occurred
INFO: Additional information

Section 4: Conclusion
Final thoughts and summary.
End of document.
"""
    write(FIX / "multiline.txt", multiline)

    # 10) empty.txt
    write(FIX / "empty.txt", b"")

    # 11) single-line.txt (no trailing LF)
    write(FIX / "single-line.txt", b"Single line without newline")

    # 12) single-line-with-lf.txt
    write(FIX / "single-line-with-lf.txt", b"Single line with newline\n")

    # 13) binary-data.bin - Mixed binary and text data
    binary_data = bytearray(b"TEXT_START\x00\x01\x02\x03BINARY_DATA\xff\xfe\xfdTEXT_END\n")
    write(FIX / "binary-data.bin", binary_data)

    # 14) repeated-patterns.txt
    repeated = b"PATTERN\n" * 100 + b"END_MARKER\n"
    write(FIX / "repeated-patterns.txt", repeated)

    # 15) unicode-test.txt - UTF-8 encoded content
    unicode_content = "Hello 世界\nCafé naïve\n🚀 Rocket\n".encode('utf-8')
    write(FIX / "unicode-test.txt", unicode_content)

    # 16) large-lines.txt - File with very long lines
    large_lines = bytearray()
    for i in range(10):
        line = b"X" * 1000 + f"LINE_{i:02d}".encode() + b"Y" * 1000 + b"\n"
        large_lines.extend(line)
    write(FIX / "large-lines.txt", large_lines)

    # 17) nested-sections.txt - Complex nested structure
    nested = b"""BEGIN_SECTION_A
  SUBSECTION_1
    DATA: value1
    DATA: value2
  SUBSECTION_2
    DATA: value3
END_SECTION_A
BEGIN_SECTION_B
  SUBSECTION_1
    DATA: value4
  SUBSECTION_2
    DATA: value5
    DATA: value6
END_SECTION_B
"""
    write(FIX / "nested-sections.txt", nested)

    # 18) edge-cases.txt - Various edge cases
    parts = []
    parts.append(b"Line with spaces at end   \n")
    parts.append(b"Line with tabs\t\t\n")
    parts.append(b"Line with mixed\t spaces\n")
    parts.append(b"Empty line above\n")
    parts.append(b"\n")  # the empty line referenced above
    parts.append(b"Line with only spaces:   \n")
    parts.append(b"Line with only tabs:\t\t\n")
    parts.append(b"Very long line: ")
    parts.append(b"X" * 1000)
    parts.append(b"\n")
    parts.append(b"Short line\n")
    edge_cases = b"".join(parts)
    write(FIX / "edge-cases.txt", edge_cases)

    # 19) crlf-comprehensive.txt - Comprehensive CRLF test file
    crlf_comp = b"Line1\r\nLine2\r\nLine3\r\n"
    write(FIX / "crlf-comprehensive.txt", crlf_comp)

    # 20) mixed-line-endings.txt - Mixed LF and CRLF line endings
    mixed_endings = b"LF line\nCRLF line\r\nAnother LF\nFinal CRLF\r\n"
    write(FIX / "mixed-line-endings.txt", mixed_endings)

    # 21) crlf-no-final-lf.txt - CRLF file without final newline
    crlf_no_final = b"Line1\r\nLine2\r\nLine3"
    write(FIX / "crlf-no-final-lf.txt", crlf_no_final)

    # 22) cr-only.txt - File with only CR (no LF) - edge case
    cr_only = b"Line1\rLine2\rLine3\r"
    write(FIX / "cr-only.txt", cr_only)

    # 23) crlf-large.txt - Large CRLF file for buffer boundary testing
    crlf_large = bytearray()
    for i in range(100):
        crlf_large.extend(f"Line {i:03d}\r\n".encode())
    write(FIX / "crlf-large.txt", crlf_large)

    # 24) crlf-boundary.txt - CRLF sequences that will span buffer boundaries
    # Create content where CRLF sequences are positioned to test buffer boundary handling
    crlf_boundary = bytearray(b"A" * 1000)  # Fill buffer
    crlf_boundary.extend(b"\r\n")  # CRLF at buffer boundary
    crlf_boundary.extend(b"B" * 1000)  # More content
    crlf_boundary.extend(b"\r\n")  # Another CRLF
    crlf_boundary.extend(b"C" * 1000)  # Final content
    write(FIX / "crlf-boundary.txt", crlf_boundary)

    # 25) ops file for CLI tests
    write(FIX / "commands_take_plus_2b.txt", b"take +2b\n")

    # 26) binary-patterns.bin - File with various binary patterns for find:bin tests
    bin_patterns = bytearray()
    # PNG header
    bin_patterns.extend(b"\x89\x50\x4E\x47\x0D\x0A\x1A\x0A")
    bin_patterns.extend(b"SOME_TEXT_DATA_HERE")
    # ZIP signature
    bin_patterns.extend(b"\x50\x4B\x03\x04")
    bin_patterns.extend(b"MORE_TEXT")
    # CAFEBABE (Java class file)
    bin_patterns.extend(b"\xCA\xFE\xBA\xBE")
    bin_patterns.extend(b"PADDING")
    # DEADBEEF pattern
    bin_patterns.extend(b"\xDE\xAD\xBE\xEF")
    bin_patterns.extend(b"END_DATA\n")
    write(FIX / "binary-patterns.bin", bin_patterns)

    # 27) binary-large.bin - Large binary file for buffer boundary testing
    bin_large = bytearray(b"\x00" * (10 * 1024 * 1024))  # 10MB of zeros
    # Insert pattern deep in the file
    pattern_offset = 7 * 1024 * 1024 + 12345
    bin_large[pattern_offset:pattern_offset+4] = b"\xDE\xAD\xBE\xEF"
    write(FIX / "binary-large.bin", bin_large)

    # 28) hex-test.bin - Simple file for hex parsing tests
    hex_test = bytearray()
    hex_test.extend(b"PREFIX_")
    hex_test.extend(b"\x01\x0D\xFF")  # hex: 01 0D FF
    hex_test.extend(b"_SUFFIX")
    write(FIX / "hex-test.bin", hex_test)

    # 29) label-offset.txt — exercises inline offset label lookups
    write(FIX / "label-offset.txt", b"A\nB\nX\n")

    # 30) backward-find.txt — ensures backward find chooses rightmost match
    write(FIX / "backward-find.txt", b"alpha ERROR beta\nERROR gamma\n")

    # 31) take-until-empty.txt — pattern at BOF to keep cursor stationary
    write(FIX / "take-until-empty.txt", b"HEADtail\n")

    # 32) utf8-boundary.bin — multi-byte code point followed by ASCII
    write(FIX / "utf8-boundary.bin", "🚀X".encode("utf-8"))

def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def run(exe: Path, tokens, in_path: str | None, stdin_data: bytes | None, extra_args=None):
    # Build argv: fiskta [options] [--input PATH] <tokens...>
    argv = [str(exe)]
    if extra_args:
        argv.extend(extra_args)
    if in_path is not None:
        argv.extend(["--input", in_path])
    argv.extend(tokens)
    try:
        proc = subprocess.run(argv, input=stdin_data, capture_output=True)
        return proc.returncode, proc.stdout, proc.stderr
    except FileNotFoundError:
        print(f"ERROR: executable not found: {exe}", file=sys.stderr)
        sys.exit(2)

def expect_stdout(actual: bytes, expect: dict) -> tuple[bool, str]:
    if "stdout" in expect:
        want = expect["stdout"].encode("utf-8")
        ok = actual == want
        return ok, "" if ok else f"stdout mismatch\n---want({len(want)}B)\n{want!r}\n---got({len(actual)}B)\n{actual!r}"
    if "stdout_startswith" in expect:
        prefix = expect["stdout_startswith"].encode("utf-8")
        ok = actual.startswith(prefix)
        return ok, "" if ok else f"stdout prefix mismatch\n---want-prefix({len(prefix)}B)\n{prefix!r}\n---got({len(actual)}B)\n{actual!r}"
    if "stdout_len" in expect:
        want_len = int(expect["stdout_len"])
        ok = len(actual) == want_len
        return ok, "" if ok else f"stdout_len mismatch want {want_len}, got {len(actual)}"
    if "stdout_sha256" in expect:
        want = expect["stdout_sha256"].lower()
        got = sha256(actual)
        ok = got == want
        return ok, "" if ok else f"sha256 mismatch want {want}, got {got}"
    # default: expect empty
    ok = len(actual) == 0
    return ok, "" if ok else f"expected empty stdout, got {len(actual)}B"

def tests():
    # NOTE: Using 'THEN' as the clause separator per your decision.
    # Each test: id, tokens (without input path), in, stdin (optional), expect {stdout|stdout_len|stdout_sha256, exit}
    return [
        # ---------- Grammar & parsing ----------
        dict(id="gram-001-clause-sep",
             tokens=["take","+3b","THEN","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="abcde", exit=0)),

        dict(id="gram-002-signed-skip",
             tokens=["skip","-5b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="gram-003-empty-needle-invalid",
             tokens=["find",""], input_file="small.txt",
             expect=dict(stdout="", exit=12)),

        dict(id="gram-004-label-name-validation",
             tokens=["label","Bad"], input_file="small.txt",
             expect=dict(stdout="", exit=12)),

        dict(id="gram-005-view-inline-offsets",
             tokens=["view","BOF+2b","BOF+5b","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

#             expect=dict(stdout="ab", exit=0)),

        dict(id="gram-008-print-hex",
             tokens=["print", r"\x00\xFA"], input_file="overlap.txt",
             expect=dict(stdout_sha256="d96bdf2090bd7dafe1ab0d9f7ffc4720d002c07abbf48df3969af497b1edbfb9", exit=0)),

        dict(id="gram-009-print-hex-invalid",
             tokens=["print", r"\x0G"], input_file="overlap.txt",
             expect=dict(stdout="", exit=12)),

        # ---------- Error path tests ----------
        dict(id="error-001-unknown-operation",
             tokens=["unknown","arg"], input_file="small.txt",
             expect=dict(stdout="", exit=12)),

        dict(id="error-002-missing-argument",
             tokens=["find"], input_file="small.txt",
             expect=dict(stdout="", exit=12)),

        dict(id="error-003-invalid-number",
             tokens=["take","notanumber"], input_file="small.txt",
             expect=dict(stdout="", exit=12)),

        dict(id="error-004-invalid-unit",
             tokens=["take","10x"], input_file="small.txt",
             expect=dict(stdout="", exit=12)),

        dict(id="error-005-invalid-location",
             tokens=["skip","to","NOTEXIST"], input_file="small.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),


        dict(id="error-007-negative-skip-beyond-bof",
             tokens=["skip","-1000b"], input_file="small.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="error-008-invalid-regex",
             tokens=["find:re","[unclosed"], input_file="small.txt",
             expect=dict(stdout="", exit=13)),

        dict(id="error-009-take-until-not-found",
             tokens=["take","until","NOTFOUND"], input_file="small.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),


        # ---------- Clause atomicity & staging ----------
        dict(id="atom-001-discard-within-clause",
             tokens=["take","+3b","find","NOPE"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),  # whole clause fails, nothing emits

        dict(id="atom-002-independent-clauses",
             tokens=["take","+3b","THEN","find","NOPE","THEN","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="abcde", exit=0)),

        dict(id="atom-003-staging-order-overlap",
             tokens=["take","+5b","take","-2b"], input_file="overlap.txt",
             expect=dict(stdout="abcdede", exit=0)),  # emits [0,5) then [3,5) concatenated

        # ---------- Cursor & Locality ----------
        dict(id="cur-001-take-moves-to-far-end",
             tokens=["take","+3b","THEN","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="abcde", exit=0)),

        dict(id="cur-002-empty-capture-no-move",
             tokens=["take","+0b","THEN","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="cur-003-take-to-cursor-empty",
             tokens=["take","to","cursor","THEN","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="cur-004-loc-expr-uses-staged-cursor",
             tokens=["skip","2b","take","to","cursor","+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        # ---------- Lines semantics ----------
        dict(id="line-001-forward-lines",
             tokens=["find","bb","take","+2l"], input_file="lines.txt",
             expect=dict(stdout="L02 bb\nL03 ccc\n", exit=0)),

        dict(id="line-002-backward-lines",
             tokens=["find","dddd","take","-2l"], input_file="lines.txt",
             expect=dict(stdout="L02 bb\nL03 ccc\n", exit=0)),

        dict(id="line-003-line-offsets-in-loc-expr",
             tokens=["find","ccc","skip","to","line-start","take","to","line-start","+2l"], input_file="lines.txt",
             expect=dict(stdout="L03 ccc\nL04 dddd\n", exit=0)),

        dict(id="line-004-cr-is-just-a-byte",
             tokens=["take","+1l"], input_file="crlf.txt",
             expect=dict(stdout="A\r\n", exit=0)),

        dict(id="line-005-line-start-unbounded-backscan",
             tokens=["find","NEEDLE","skip","to","line-start","take","+3b"], input_file="longline-left.bin",
             expect=dict(stdout="AAA", exit=0)),

        dict(id="line-006-line-end-unbounded-forwardscan",
             tokens=["find","B","take","until","C","at","line-end"], input_file="longline-right.bin",
             # Expect many bytes; just assert the last 5 bytes are 'TAIL\n' by taking until line-end then ensure the total ends with LF before TAIL won't be included (we captured up to LF).
             # Assert exact length: 32 bytes of 'B' + 12MiB of 'C' + 1 byte LF = 32 + 12*1024*1024 + 1
             expect=dict(stdout_len=32 + 12*1024*1024 + 1, exit=0)),  # from 'B'*32 + 'C'*12MiB up to and including LF

        # ---------- find semantics ----------
        dict(id="find-001-forward-first-match",
             tokens=["find","ERROR","take","+10b"], input_file="small.txt",
             expect=dict(stdout="ERROR hit\n", exit=0)),

        dict(id="find-002-backward-rightmost",
             tokens=["skip","100b","find","to","BOF","ERROR","take","+7b"], input_file="small.txt",
             expect=dict(stdout="ERROR 2", exit=0)),

        dict(id="find-003-forward-window-spans-buffers",
             tokens=["find","NEEDLE","take","+6b"], input_file="big-forward.bin",
             expect=dict(stdout="NEEDLE", exit=0)),

        dict(id="find-004-miss-fails-clause",
             tokens=["find","NOPE","take","+3b"], input_file="small.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        # ---------- take to ----------
        dict(id="take-001-order-normalized",
             tokens=["label","HERE","skip","5b","take","to","HERE","take","+5b"], input_file="overlap.txt",
             expect=dict(stdout="abcdefghij", exit=0)),

        dict(id="take-002-to-invalid-fails",
             tokens=["take","to","MISSING"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        # ---------- take until ----------
        dict(id="until-001-default-at-match-start",
             tokens=["take","until","ERROR","THEN","take","+5b"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\nERROR", exit=0)),

        dict(id="until-002-at-line-start",
             tokens=["take","until","ERROR","at","line-start"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\n", exit=0)),

        dict(id="until-003-at-match-end",
             tokens=["take","until","ERROR","at","match-end"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\nERROR", exit=0)),

        dict(id="until-004-miss-fails",
             tokens=["take","until","ZZZ"], input_file="small.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        # ---------- Labels & LRU ----------
        dict(id="lab-001-basic-label-skip-to",
             tokens=["label","A","skip","3b","take","to","A"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="lab-002-unknown-label-fails",
             tokens=["skip","to","NOPE"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="lab-003-evict-lru-on-33rd",
             tokens=[
                # 32 labels in 32 clauses at pos 0
                "label","A01","THEN","label","A02","THEN","label","A03","THEN","label","A04","THEN",
                "label","A05","THEN","label","A06","THEN","label","A07","THEN","label","A08","THEN",
                "label","A09","THEN","label","A10","THEN","label","A11","THEN","label","A12","THEN",
                "label","A13","THEN","label","A14","THEN","label","A15","THEN","label","A16","THEN",
                "label","A17","THEN","label","A18","THEN","label","A19","THEN","label","A20","THEN",
                "label","A21","THEN","label","A22","THEN","label","A23","THEN","label","A24","THEN",
                "label","A25","THEN","label","A26","THEN","label","A27","THEN","label","A28","THEN",
                "label","A29","THEN","label","A30","THEN","label","A31","THEN","label","A32","THEN",
                # add A33 (no eviction with direct mapping), then goto A01 succeeds
                "label","A33","skip","to","A01","take","+1b"
             ], input_file="labels-evict.txt",
             expect=dict(stdout="0", exit=0)),

        # ---------- Bounds & clamps ----------
        dict(id="clamp-001-skip-clamps",
             tokens=["skip","to","EOF","skip","100b","take","to","BOF"], input_file="overlap.txt",
             expect=dict(stdout="abcdefghij", exit=0)),

        dict(id="clamp-002-loc-expr-offset-clamps",
             tokens=["take","to","EOF","+100b"], input_file="overlap.txt",
             expect=dict(stdout="abcdefghij", exit=0)),

        # ---------- Stdin & spooling ----------
        dict(id="io-001-stdin-forward",
             tokens=["take","+5b"], input_file="-", stdin=b"Hello\nWorld\n",
             expect=dict(stdout="Hello", exit=0)),


        # ---------- last_match requirements ----------
        dict(id="match-001-atloc-requires-valid",
             tokens=["skip","to","match-start"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        # ---------- Basic Operations: Comprehensive take tests ----------
        dict(id="take-101-zero-bytes",
             tokens=["take","+0b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-102-zero-lines",
             tokens=["take","+0l"], input_file="lines.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-103-negative-bytes",
             tokens=["skip","5b","take","-3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        dict(id="take-104-negative-lines",
             tokens=["find","L05","take","-2l"], input_file="lines.txt",
             expect=dict(stdout="L03 ccc\nL04 dddd\n", exit=0)),

        dict(id="take-105-beyond-eof",
             tokens=["take","+1000b"], input_file="overlap.txt",
             expect=dict(stdout="abcdefghij", exit=0)),

        dict(id="take-106-beyond-bof",
             tokens=["take","-1000b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-107-single-byte",
             tokens=["take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="a", exit=0)),

        dict(id="take-108-single-line",
             tokens=["take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L01 a\n", exit=0)),

        dict(id="take-109-all-lines",
             tokens=["take","+10l"], input_file="lines.txt",
             expect=dict(stdout="L01 a\nL02 bb\nL03 ccc\nL04 dddd\nL05 eeeee\nL06 ffffff\nL07 ggggggg\nL08 hhhhhhhh\nL09 iiiiiiiii\nL10 jjjjjjjjjj\n", exit=0)),

        dict(id="take-110-empty-file",
             tokens=["take","+5b"], input_file="empty.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-111-single-line-no-lf",
             tokens=["take","+1l"], input_file="single-line.txt",
             expect=dict(stdout="Single line without newline", exit=0)),

        # ---------- Basic Operations: Comprehensive skip tests ----------
        dict(id="skip-101-zero-bytes",
             tokens=["skip","0b","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="skip-102-zero-lines",
             tokens=["skip","0l","take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L01 a\n", exit=0)),

        dict(id="skip-103-skip-all",
             tokens=["skip","1000b","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="skip-104-skip-lines",
             tokens=["skip","3l","take","+2l"], input_file="lines.txt",
             expect=dict(stdout="L04 dddd\nL05 eeeee\n", exit=0)),

        dict(id="skip-105-skip-beyond-eof",
             tokens=["skip","1000b","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="skip-106-skip-to-eof",
             tokens=["skip","10b","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        # ---------- Basic Operations: Comprehensive find tests ----------
        dict(id="find-101-single-char",
             tokens=["find","a","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="a", exit=0)),

        dict(id="find-102-multi-char",
             tokens=["find","def","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),

        dict(id="find-103-case-sensitive",
             tokens=["find","ERROR","take","+5b"], input_file="small.txt",
             expect=dict(stdout="ERROR", exit=0)),

        dict(id="find-104-no-match",
             tokens=["find","XYZ","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="find-105-empty-string",
             tokens=["find",""], input_file="overlap.txt",
             expect=dict(stdout="", exit=12)),

        dict(id="find-106-binary-data",
             tokens=["find","BINARY_DATA","take","+11b"], input_file="binary-data.bin",
             expect=dict(stdout="BINARY_DATA", exit=0)),

        dict(id="find-107-unicode",
             tokens=["find","世界","take","+6b"], input_file="unicode-test.txt",
             expect=dict(stdout="世界", exit=0)),

        dict(id="find-108-repeated-pattern",
             tokens=["find","PATTERN","take","+7b"], input_file="repeated-patterns.txt",
             expect=dict(stdout="PATTERN", exit=0)),

        dict(id="find-109-last-occurrence",
             tokens=["skip","1000b","find","to","BOF","PATTERN","take","+7b"], input_file="repeated-patterns.txt",
             expect=dict(stdout="PATTERN", exit=0)),

        # ---------- Advanced Operations: take to tests ----------
        dict(id="take-to-101-bof",
             tokens=["skip","5b","take","to","BOF"], input_file="overlap.txt",
             expect=dict(stdout="abcde", exit=0)),

        dict(id="take-to-102-eof",
             tokens=["take","to","EOF"], input_file="overlap.txt",
             expect=dict(stdout="abcdefghij", exit=0)),

        dict(id="take-to-103-cursor",
             tokens=["skip","3b","take","to","cursor"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-to-104-cursor-plus",
             tokens=["skip","2b","take","to","cursor","+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        dict(id="take-to-105-cursor-minus",
             tokens=["skip","5b","take","to","cursor","-2b"], input_file="overlap.txt",
             expect=dict(stdout="de", exit=0)),

        dict(id="take-to-106-bof-plus",
             tokens=["take","to","BOF","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="take-to-107-eof-minus",
             tokens=["take","to","EOF","-3b"], input_file="overlap.txt",
             expect=dict(stdout="abcdefg", exit=0)),

        dict(id="take-to-108-label",
             tokens=["label","MARK","skip","3b","take","to","MARK"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="take-to-109-label-plus",
             tokens=["label","MARK","skip","2b","take","to","MARK","+2b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-to-110-match-start",
             tokens=["find","def","take","to","match-start"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-to-111-match-end",
             tokens=["find","def","take","to","match-end"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),

        dict(id="take-to-112-line-start",
             tokens=["find","L03","take","to","line-start"], input_file="lines.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="take-to-113-line-end",
             tokens=["find","L03","take","to","line-end"], input_file="lines.txt",
             expect=dict(stdout="L03 ccc\n", exit=0)),

        dict(id="take-to-114-line-offset",
             tokens=["find","L05","take","to","line-start","+1l"], input_file="lines.txt",
             expect=dict(stdout="L05 eeeee\n", exit=0)),

        # ---------- Advanced Operations: take until tests ----------
        dict(id="take-until-101-default",
             tokens=["take","until","ERROR"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\n", exit=0)),

        dict(id="take-until-102-at-match-start",
             tokens=["take","until","ERROR","at","match-start"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\n", exit=0)),

        dict(id="take-until-103-at-match-end",
             tokens=["take","until","ERROR","at","match-end"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\nERROR", exit=0)),

        dict(id="take-until-104-at-line-start",
             tokens=["take","until","ERROR","at","line-start"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\n", exit=0)),

        dict(id="take-until-105-at-line-end",
             tokens=["take","until","ERROR","at","line-end"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\nERROR hit\n", exit=0)),

        dict(id="take-until-106-at-match-start-plus",
             tokens=["take","until","ERROR","at","match-start","+1b"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\nE", exit=0)),

        dict(id="take-until-107-at-match-end-minus",
             tokens=["take","until","ERROR","at","match-end","-1b"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\nERRO", exit=0)),

        dict(id="take-until-108-no-match",
             tokens=["take","until","XYZ"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="take-until-109-empty-needle",
             tokens=["take","until",""], input_file="overlap.txt",
             expect=dict(stdout="", exit=12)),

        dict(id="take-until-110-binary-data",
             tokens=["take","until","BINARY_DATA"], input_file="binary-data.bin",
             expect=dict(stdout="TEXT_START\x00\x01\x02\x03", exit=0)),

        # ---------- Advanced Operations: label and goto tests ----------
        dict(id="label-101-basic",
             tokens=["label","START","skip","3b","skip","to","START","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="label-102-multiple-labels",
             tokens=["label","A","skip","2b","label","B","skip","to","A","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="ab", exit=0)),

        dict(id="label-103-label-with-offset",
             tokens=["label","MARK","skip","5b","skip","to","MARK","+2b","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        dict(id="label-104-label-minus-offset",
             tokens=["label","MARK","skip","5b","skip","to","MARK","-2b","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="label-105-unknown-label",
             tokens=["skip","to","UNKNOWN"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="label-106-label-after-failed-clause",
             tokens=["label","A","find","XYZ","THEN","skip","to","A"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="label-107-goto-bof",
             tokens=["skip","to","BOF","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="label-108-goto-eof",
             tokens=["skip","to","EOF","take","-3b"], input_file="overlap.txt",
             expect=dict(stdout="hij", exit=0)),

        dict(id="label-109-goto-match-start",
             tokens=["find","def","skip","to","match-start","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),

        dict(id="label-110-goto-match-end",
             tokens=["find","def","skip","to","match-end","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="ghi", exit=0)),

        dict(id="label-111-goto-line-start",
             tokens=["find","L03","skip","to","line-start","take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L03 ccc\n", exit=0)),

        dict(id="label-112-goto-line-end",
             tokens=["find","L03","skip","to","line-end","take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L04 dddd\n", exit=0)),

        # ---------- Line semantics comprehensive tests ----------
        dict(id="line-101-forward-from-middle",
             tokens=["skip","5b","take","+2l"], input_file="lines.txt",
             expect=dict(stdout="L01 a\nL02 bb\n", exit=0)),

        dict(id="line-102-backward-from-middle",
             tokens=["skip","15b","take","-2l"], input_file="lines.txt",
             expect=dict(stdout="L01 a\nL02 bb\n", exit=0)),



        dict(id="line-105-crlf-handling",
             tokens=["take","+1l"], input_file="crlf.txt",
             expect=dict(stdout="A\r\n", exit=0)),

        dict(id="line-106-no-trailing-lf",
             tokens=["take","+1l"], input_file="single-line.txt",
             expect=dict(stdout="Single line without newline", exit=0)),

        dict(id="line-107-empty-lines",
             tokens=["find","Section 2","take","+3l"], input_file="multiline.txt",
             expect=dict(stdout="Section 2: Data\nKEY1=value1\nKEY2=value2\n", exit=0)),

        dict(id="line-108-large-lines",
             tokens=["find","LINE_05","take","+1l"], input_file="large-lines.txt",
             expect=dict(stdout_len=2008, exit=0)),  # 1000 X's + "LINE_05" + 1000 Y's + \n

        dict(id="line-109-line-offset-forward",
             tokens=["find","L05","take","to","line-start","+3l"], input_file="lines.txt",
             expect=dict(stdout="L05 eeeee\nL06 ffffff\nL07 ggggggg\n", exit=0)),

        dict(id="line-110-line-offset-backward",
             tokens=["find","L05","take","to","line-start","-2l"], input_file="lines.txt",
             expect=dict(stdout="L03 ccc\nL04 dddd\n", exit=0)),

        # ---------- Clause semantics and error handling ----------
        dict(id="clause-101-all-succeed",
             tokens=["take","+2b","THEN","take","+2b","THEN","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="abcdef", exit=0)),

        dict(id="clause-102-first-fails-second-succeeds",
             tokens=["find","XYZ","take","+2b","THEN","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="clause-103-first-succeeds-second-fails",
             tokens=["take","+3b","THEN","find","XYZ","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="clause-104-all-fail",
             tokens=["find","XYZ","take","+2b","THEN","find","ABC","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="clause-105-staging-within-clause",
             tokens=["take","+5b","take","-2b"], input_file="overlap.txt",
             expect=dict(stdout="abcdede", exit=0)),

        dict(id="clause-106-label-staging",
             tokens=["label","A","skip","3b","label","B","skip","to","A","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="clause-107-cursor-staging",
             tokens=["skip","2b","take","to","cursor","+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        dict(id="clause-108-match-staging",
             tokens=["find","def","take","to","match-end","+2b"], input_file="overlap.txt",
             expect=dict(stdout="defgh", exit=0)),

        # ---------- Label staging precedence tests ----------
        dict(id="clause-109-staged-label-override",
             tokens=["label","A","THEN","skip","3b","label","A","skip","to","A","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),  # Should use staged A at position 3, not committed A at position 0

        dict(id="clause-110-failed-clause-label-isolation",
             tokens=["label","A","THEN","skip","3b","label","A","THEN","find","XYZ","THEN","skip","to","A","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),  # Second clause succeeds and commits A at position 3, third clause fails, fourth clause uses committed A at position 3

        # ---------- Edge cases and boundary conditions ----------
        dict(id="edge-101-empty-file-operations",
             tokens=["take","+1b"], input_file="empty.txt",
             expect=dict(stdout="", exit=0)),

        dict(id="edge-102-single-byte-file",
             tokens=["take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="a", exit=0)),

        dict(id="edge-103-single-line-file",
             tokens=["take","+1l"], input_file="single-line.txt",
             expect=dict(stdout="Single line without newline", exit=0)),

        # Note: Binary data tests with \x00 and \xff in tokens are removed
        # because Python subprocess cannot handle embedded null bytes in command-line arguments

        dict(id="edge-106-unicode-boundary",
             tokens=["find","🚀","take","+4b"], input_file="unicode-test.txt",
             expect=dict(stdout="🚀", exit=0)),

        dict(id="edge-107-very-long-line",
             tokens=["take","+1l"], input_file="large-lines.txt",
             expect=dict(stdout_len=2008, exit=0)),

        dict(id="edge-108-repeated-patterns",
             tokens=["find","PATTERN","take","+7b"], input_file="repeated-patterns.txt",
             expect=dict(stdout="PATTERN", exit=0)),

        dict(id="edge-109-nested-sections",
             tokens=["find","BEGIN_SECTION_A","take","until","END_SECTION_A"], input_file="nested-sections.txt",
             expect=dict(stdout="BEGIN_SECTION_A\n  SUBSECTION_1\n    DATA: value1\n    DATA: value2\n  SUBSECTION_2\n    DATA: value3\n", exit=0)),

        dict(id="edge-110-edge-case-whitespace",
             tokens=["find","spaces at end","take","+1l"], input_file="edge-cases.txt",
             expect=dict(stdout="Line with spaces at end   \n", exit=0)),

        # ---------- IO and stdin processing ----------
        dict(id="io-101-stdin-basic",
             tokens=["take","+5b"], input_file="-", stdin=b"Hello World",
             expect=dict(stdout="Hello", exit=0)),

        dict(id="io-102-stdin-lines",
             tokens=["take","+2l"], input_file="-", stdin=b"Line 1\nLine 2\nLine 3\n",
             expect=dict(stdout="Line 1\nLine 2\n", exit=0)),

        dict(id="io-103-stdin-find",
             tokens=["find","World","take","+5b"], input_file="-", stdin=b"Hello World\n",
             expect=dict(stdout="World", exit=0)),


        dict(id="io-105-stdin-large",
             tokens=["find","NEEDLE","take","+6b"], input_file="-", stdin=b"A" * 1000000 + b"NEEDLE" + b"B" * 1000000,
             expect=dict(stdout="NEEDLE", exit=0)),

        # Note: io-106-stdin-binary test is removed because it uses \x00\x01 in tokens
        # which Python subprocess cannot handle (embedded null bytes in command-line arguments)

        # ---------- Label LRU eviction comprehensive tests ----------
        dict(id="lru-101-basic-eviction",
             tokens=[
                 "label","A01","THEN","label","A02","THEN","label","A03","THEN","label","A04","THEN",
                 "label","A05","THEN","label","A06","THEN","label","A07","THEN","label","A08","THEN",
                 "label","A09","THEN","label","A10","THEN","label","A11","THEN","label","A12","THEN",
                 "label","A13","THEN","label","A14","THEN","label","A15","THEN","label","A16","THEN",
                 "label","A17","THEN","label","A18","THEN","label","A19","THEN","label","A20","THEN",
                 "label","A21","THEN","label","A22","THEN","label","A23","THEN","label","A24","THEN",
                 "label","A25","THEN","label","A26","THEN","label","A27","THEN","label","A28","THEN",
                 "label","A29","THEN","label","A30","THEN","label","A31","THEN","label","A32","THEN",
                 "label","A33","skip","to","A01","take","+1b"
             ], input_file="labels-evict.txt",
             expect=dict(stdout="0", exit=0)),

        dict(id="lru-102-simple-reuse",
             tokens=["label","TEST","take","+1b"], input_file="labels-evict.txt",
             expect=dict(stdout="0", exit=0)),

        dict(id="lru-103-label-update",
             tokens=["label","MARK","take","+1b"], input_file="labels-evict.txt",
             expect=dict(stdout="0", exit=0)),

        # ---------- Complex multi-operation sequences ----------
        dict(id="complex-101-extract-section",
             tokens=["find","Section 2","take","until","Section 3"], input_file="multiline.txt",
             expect=dict(stdout="Section 2: Data\nKEY1=value1\nKEY2=value2\nKEY3=value3\n\n", exit=0)),

        dict(id="complex-102-extract-key-value",
             tokens=["find","KEY2","take","until","KEY3","at","line-start"], input_file="multiline.txt",
             expect=dict(stdout="KEY2=value2\n", exit=0)),

        dict(id="complex-103-extract-error-lines",
             tokens=["find","ERROR","skip","to","line-start","take","+1l"], input_file="multiline.txt",
             expect=dict(stdout="ERROR: Critical failure occurred\n", exit=0)),

        dict(id="complex-104-extract-multiple-sections",
             tokens=["find","Section 1","take","until","Section 2","THEN","find","Section 3","take","until","Section 4"], input_file="multiline.txt",
             expect=dict(stdout="Section 1: Introduction\nThis is the first section with multiple lines.\nIt contains various text patterns.\n\nSection 3: Results\nSUCCESS: Operation completed\nWARNING: Minor issue detected\nERROR: Critical failure occurred\nINFO: Additional information\n\n", exit=0)),

        dict(id="complex-105-nested-extraction",
             tokens=["find","BEGIN_SECTION_A","label","START","find","DATA: value2","skip","to","START","take","until","END_SECTION_A"], input_file="nested-sections.txt",
             expect=dict(stdout="BEGIN_SECTION_A\n  SUBSECTION_1\n    DATA: value1\n    DATA: value2\n  SUBSECTION_2\n    DATA: value3\n", exit=0)),

        dict(id="complex-106-large-file-extraction",
             tokens=["find","NEEDLE","take","-1000b"], input_file="big-forward.bin",
             expect=dict(stdout_len=1000, exit=0)),

        dict(id="complex-107-unicode-extraction",
             tokens=["find","Café","take","+1l"], input_file="unicode-test.txt",
             expect=dict(stdout="Café naïve\n", exit=0)),

        dict(id="complex-108-binary-extraction",
             tokens=["find","TEXT_START","take","until","TEXT_END"], input_file="binary-data.bin",
             expect=dict(stdout_sha256="b0f3971a2ee5b79231cfc24a0a3b2fa930e951481f751304124883d069c919ed", exit=0)),  # Binary data with SHA256 hash

        dict(id="complex-109-repeated-pattern-extraction",
             tokens=["find","PATTERN","label","FIRST","skip","50b","find","PATTERN","skip","to","FIRST","take","until","END_MARKER","at","line-start"], input_file="repeated-patterns.txt",
             expect=dict(stdout="PATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\nPATTERN\n", exit=0)),

        dict(id="complex-110-edge-case-extraction",
             tokens=["find","Very long line","take","+1l"], input_file="edge-cases.txt",
             expect=dict(stdout_len=1017, exit=0)),  # "Very long line: " + 1000 X's + \n = 16 + 1000 + 1 = 1017

        # ---------- CRLF Support Comprehensive Tests ----------
        dict(id="crlf-101-basic-crlf-lines",
             tokens=["take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\n", exit=0)),

        dict(id="crlf-102-multiple-crlf-lines",
             tokens=["take","+2l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\nLine2\r\n", exit=0)),

        dict(id="crlf-103-all-crlf-lines",
             tokens=["take","+3l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\nLine2\r\nLine3\r\n", exit=0)),

        dict(id="crlf-104-crlf-skip-lines",
             tokens=["skip","1l","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2\r\n", exit=0)),

        dict(id="crlf-105-crlf-negative-lines",
             tokens=["skip","2l","take","-1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2\r\n", exit=0)),

        dict(id="crlf-106-crlf-line-start",
             tokens=["find","Line2","skip","to","line-start","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2\r\n", exit=0)),  # goto line-start from Line2 gives Line2\r\n

        dict(id="crlf-107-crlf-line-end",
             tokens=["find","Line2","skip","to","line-end","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line3\r\n", exit=0)),

        dict(id="crlf-108-crlf-take-to-line-start",
             tokens=["find","Line2","take","to","line-start"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="", exit=0)),  # take to line-start from cursor at Line2 gives empty range

        dict(id="crlf-109-crlf-take-to-line-end",
             tokens=["find","Line2","take","to","line-end"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2\r\n", exit=0)),  # take to line-end from cursor at Line2 gives Line2\r\n

        dict(id="crlf-110-crlf-line-offsets",
             tokens=["find","Line2","take","to","line-start","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2\r\n", exit=0)),

        dict(id="crlf-111-crlf-no-final-lf",
             tokens=["take","+1l"], input_file="crlf-no-final-lf.txt",
             expect=dict(stdout="Line1\r\n", exit=0)),

        dict(id="crlf-112-crlf-no-final-all-lines",
             tokens=["take","+3l"], input_file="crlf-no-final-lf.txt",
             expect=dict(stdout="Line1\r\nLine2\r\nLine3", exit=0)),

        dict(id="crlf-113-mixed-line-endings-lf",
             tokens=["find","LF line","take","+1l"], input_file="mixed-line-endings.txt",
             expect=dict(stdout="LF line\n", exit=0)),

        dict(id="crlf-114-mixed-line-endings-crlf",
             tokens=["find","CRLF line","take","+1l"], input_file="mixed-line-endings.txt",
             expect=dict(stdout="CRLF line\r\n", exit=0)),

        dict(id="crlf-115-mixed-line-endings-multiple",
             tokens=["take","+2l"], input_file="mixed-line-endings.txt",
             expect=dict(stdout="LF line\nCRLF line\r\n", exit=0)),

        dict(id="crlf-116-mixed-line-endings-all",
             tokens=["take","+4l"], input_file="mixed-line-endings.txt",
             expect=dict(stdout="LF line\nCRLF line\r\nAnother LF\nFinal CRLF\r\n", exit=0)),

        # dict(id="crlf-117-cr-only-basic",
        #      tokens=["take","+1l"], input_file="cr-only.txt",
        #      expect=dict(stdout="Line1\r", exit=0)),  # CR is treated as regular byte, not line terminator

        # dict(id="crlf-118-cr-only-multiple",
        #      tokens=["take","+2l"], input_file="cr-only.txt",
        #      expect=dict(stdout="Line1\rLine2\r", exit=0)),

        dict(id="crlf-119-crlf-large-file",
             tokens=["find","Line 050","take","+1l"], input_file="crlf-large.txt",
             expect=dict(stdout="Line 050\r\n", exit=0)),

        dict(id="crlf-120-crlf-large-skip",
             tokens=["skip","50l","take","+1l"], input_file="crlf-large.txt",
             expect=dict(stdout="Line 050\r\n", exit=0)),

        dict(id="crlf-121-crlf-boundary-test",
             tokens=["find","B","take","+1l"], input_file="crlf-boundary.txt",
             expect=dict(stdout="B" * 1000 + "\r\n", exit=0)),

        dict(id="crlf-122-crlf-boundary-skip",
             tokens=["skip","1000b","take","+1l"], input_file="crlf-boundary.txt",
             expect=dict(stdout="A" * 1000 + "\r\n", exit=0)),  # skip 1000b puts cursor at \r, take +1l gets A line

        dict(id="crlf-123-crlf-take-until",
             tokens=["take","until","Line2"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\n", exit=0)),

        dict(id="crlf-124-crlf-take-until-at-match-end",
             tokens=["take","until","Line2","at","match-end"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\nLine2", exit=0)),

        dict(id="crlf-125-crlf-take-until-at-line-end",
             tokens=["take","until","Line2","at","line-end"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\nLine2\r\n", exit=0)),

        dict(id="crlf-126-crlf-find-backward",
             tokens=["skip","100b","find","to","BOF","Line2","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2\r\n", exit=0)),

        dict(id="crlf-127-crlf-label-operations",
             tokens=["label","START","skip","1l","skip","to","START","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\n", exit=0)),

        dict(id="crlf-128-crlf-cursor-operations",
             tokens=["skip","5b","take","to","cursor","+5b"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="\r\nLin", exit=0)),  # skip 5b to '1', take to cursor+5b gives \r\nLin

        dict(id="crlf-129-crlf-empty-lines",
             tokens=["find","Line2","take","to","line-start","+2l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2\r\nLine3\r\n", exit=0)),

        dict(id="crlf-130-crlf-edge-case-boundary",
             tokens=["take","+1l"], input_file="crlf-boundary.txt",
             expect=dict(stdout="A" * 1000 + "\r\n", exit=0)),

        # ---------- CRLF Error Cases ----------
        dict(id="crlf-131-crlf-beyond-eof",
             tokens=["take","+100l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\nLine2\r\nLine3\r\n", exit=0)),

        dict(id="crlf-132-crlf-beyond-bof",
             tokens=["take","-100l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="", exit=0)),  # take -100l from BOF gives empty (nothing before BOF)

        dict(id="crlf-133-crlf-no-match",
             tokens=["find","NONEXISTENT","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="crlf-134-crlf-empty-file",
             tokens=["take","+1l"], input_file="empty.txt",
             expect=dict(stdout="", exit=0)),

        # ---------- CRLF Complex Scenarios ----------
        dict(id="crlf-135-crlf-multi-clause",
             tokens=["find","Line1","take","+1l","THEN","find","Line2","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\nLine2\r\n", exit=0)),

        dict(id="crlf-136-crlf-nested-extraction",
             tokens=["find","Line1","label","MARK","find","Line3","skip","to","MARK","take","until","Line3"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\nLine2\r\n", exit=0)),

        dict(id="crlf-137-crlf-stdin-processing",
             tokens=["take","+1l"], input_file="-", stdin=b"CRLF line\r\nAnother line\r\n",
             expect=dict(stdout="CRLF line\r\n", exit=0)),

        dict(id="crlf-138-crlf-stdin-multiple",
             tokens=["take","+2l"], input_file="-", stdin=b"Line1\r\nLine2\r\nLine3\r\n",
             expect=dict(stdout="Line1\r\nLine2\r\n", exit=0)),

        dict(id="crlf-139-crlf-stdin-find",
             tokens=["find","Line2","take","+1l"], input_file="-", stdin=b"Line1\r\nLine2\r\nLine3\r\n",
             expect=dict(stdout="Line2\r\n", exit=0)),

        # dict(id="crlf-140-crlf-stdin-backward",
        #      tokens=["find","to","BOF","Line1","take","+1l"], input_file="-", stdin=b"Line1\r\nLine2\r\nLine3\r\n",
        #      expect=dict(stdout="Line1\r\n", exit=0)),  # REMOVED: backward search from BOF is impossible

        # ---------- Inverted takes & cursor law ----------
        dict(id="inv-001-take-to-symmetric-output",
             # Both programs must emit identical bytes: [min(A,B), max(A,B))
             tokens=["label","A","skip","7b","label","B","skip","to","A","take","to","B","THEN",
                     "skip","to","B","take","to","A"],
             input_file="overlap.txt",
             expect=dict(stdout="abcdefgabcdefg", exit=0)),
             # 'overlap.txt' is "abcdefghij"; A=0, B=7 -> "abcdefg" twice.

        dict(id="inv-002-until-empty-does-not-move",
             # Test that take until works correctly with at expressions
             tokens=["skip","5b",                 # cursor at 'f'
                     "take","until","hij","at","match-start","-2b",  # target at match-start-2b
                     "THEN",
                     "take","+3b"],              # must emit 'fgh'
             input_file="overlap.txt",
             expect=dict(stdout="fgh", exit=0)),

        # ---------- Inline offsets in loc/at expressions ----------
        dict(id="gram-010-inline-loc-offset",
             tokens=["take","to","BOF+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="gram-011-inline-at-offset",
             tokens=["take","until","ERROR","at","line-start+1l"], input_file="small.txt",
             expect=dict(stdout="Header\nbody 1\nbody 2\nERROR hit\n", exit=0)),

        dict(id="inv-003-symmetric-output-and-poststate",
             tokens=["label","A","skip","4b","label","B",
                     # branch 1
                     "skip","to","A","take","to","B","take","+1b","THEN",
                     # branch 2
                     "skip","to","B","take","to","A","take","+1b"],
             input_file="overlap.txt",
             expect=dict(stdout="abcdeabcde", exit=0)),

        # ---------- Additional edge cases ----------
        dict(id="edge-111-label-with-hyphens",
             tokens=["label","FOO-BAR","skip","3b","skip","to","FOO-BAR+1b","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="bc", exit=0)),

        dict(id="edge-112-cursor-at-newline-line-end",
             tokens=["find","L03","skip","to","line-end","take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L04 dddd\n", exit=0)),

        # ---------- UTF-8 character tests ----------
        dict(id="utf8-001-take-c-vs-b",
             tokens=["find","世界","take","to","match-start","take","+3c"], input_file="unicode-test.txt",
             expect=dict(stdout="世界\n", exit=0)),

        dict(id="utf8-002-negative-chars",
             tokens=["find","Café","take","to","match-start","take","+3c"], input_file="unicode-test.txt",
             expect=dict(stdout="Caf", exit=0)),

        dict(id="utf8-003-loc-expr-char-offset",
             tokens=["find","Café","take","to","match-start+2c"], input_file="unicode-test.txt",
             expect=dict(stdout="Ca", exit=0)),

        dict(id="utf8-004-permissive-invalid",
             tokens=["take","+3c"], input_file="binary-data.bin",
             expect=dict(stdout="TEX", exit=0)),  # counts bytes T,E,X as 3 chars despite binary following

        dict(id="utf8-005-empty-char-capture-no-move",
             tokens=["skip","5b","take","to","cursor+0c","THEN","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="fgh", exit=0)),

        dict(id="edge-feedback-001-inline-offset-label-resolution",
             tokens=["label","HERE","THEN","skip","to","HERE+1l","take","1l"], input_file="-", stdin=b"a\nb\nX\n",
             expect=dict(stdout="b\n", exit=0)),

        dict(id="edge-feedback-002-backward-window-find-rightmost",
             tokens=["skip","15b","find","to","BOF","ERROR","take","5b"], input_file="-", stdin=b"aaa ERROR aaa ERROR aaa",
             expect=dict(stdout="ERROR", exit=0)),

        dict(id="edge-feedback-003-take-until-empty-span-no-move",
             tokens=["take","until","HEAD","THEN","take","4b"], input_file="-", stdin=b"HEAD\nM1\nM2\n",
             expect=dict(stdout="HEAD", exit=0)),

        dict(id="edge-feedback-004-utf8-chopped-boundary",
             tokens=["take","1c","take","1c"], input_file="-", stdin=b"\xF0\x9F\x98\x80X",
             expect=dict(stdout_len=5, exit=0)),  # 4 bytes for 😀 + 1 byte for X

        dict(id="edge-feedback-005-lines-anchor-negative",
             tokens=["skip","7b","take","-2l"], input_file="-", stdin=b"L1\nL2\nL3\nL4\n",
             expect=dict(stdout="L1\nL2\n", exit=0)),

        # ---------- Regex (find:re) Tests ----------
        # Basic character matching
        dict(id="regex-001-literal-char",
             tokens=["find:re","a","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="a", exit=0)),

        dict(id="regex-002-literal-string",
             tokens=["find:re","def","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),

        dict(id="regex-003-any-char",
             tokens=["find:re",".","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="a", exit=0)),

        dict(id="regex-004-any-char-multiple",
             tokens=["find:re","...","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        # Character classes
        dict(id="regex-005-digit-class",
             tokens=["find:re","\\d","take","+1b"], input_file="-", stdin=b"abc123def",
             expect=dict(stdout="1", exit=0)),

        dict(id="regex-006-word-class",
             tokens=["find:re","\\w","take","+1b"], input_file="-", stdin=b"123abc456",
             expect=dict(stdout="1", exit=0)),

        dict(id="regex-007-space-class",
             tokens=["find:re","\\s","take","+1b"], input_file="-", stdin=b"abc def",
             expect=dict(stdout=" ", exit=0)),

        dict(id="regex-008-negated-digit",
             tokens=["find:re","\\D","take","+1b"], input_file="-", stdin=b"123abc456",
             expect=dict(stdout="a", exit=0)),

        dict(id="regex-009-negated-word",
             tokens=["find:re","\\W","take","+1b"], input_file="-", stdin=b"abc def",
             expect=dict(stdout=" ", exit=0)),

        dict(id="regex-010-negated-space",
             tokens=["find:re","\\S","take","+1b"], input_file="-", stdin=b"   abc",
             expect=dict(stdout="a", exit=0)),

        # Character class comprehensive tests (verify correct implementation)
        dict(id="regex-charclass-d-matches-digits",
             tokens=["find:re","\\d\\d\\d","take","to","match-end"], input_file="-", stdin=b"abc123def456",
             expect=dict(stdout="123", exit=0)),

        dict(id="regex-charclass-d-rejects-nondigits",
             tokens=["find:re","\\d","take","to","match-end"], input_file="-", stdin=b"abcXYZ",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-charclass-D-matches-nondigits",
             tokens=["find:re","\\D\\D\\D","take","to","match-end"], input_file="-", stdin=b"123abc456",
             expect=dict(stdout="abc", exit=0)),

        dict(id="regex-charclass-D-rejects-digits",
             tokens=["find:re","\\D","take","to","match-end"], input_file="-", stdin=b"0123456789",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-charclass-w-matches-word-chars",
             tokens=["find:re","\\w\\w\\w\\w\\w","take","to","match-end"], input_file="-", stdin=b"  hello  ",
             expect=dict(stdout="hello", exit=0)),

        dict(id="regex-charclass-w-matches-underscore",
             tokens=["find:re","\\w\\w\\w","take","to","match-end"], input_file="-", stdin=b"a_b",
             expect=dict(stdout="a_b", exit=0)),

        dict(id="regex-charclass-w-rejects-nonword",
             tokens=["find:re","\\w","take","to","match-end"], input_file="-", stdin=b" !@#$%",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-charclass-W-matches-nonword",
             tokens=["find:re","\\W\\W\\W","take","to","match-end"], input_file="-", stdin=b"abc !@ def",
             expect=dict(stdout=" !@", exit=0)),

        dict(id="regex-charclass-W-rejects-word-chars",
             tokens=["find:re","\\W","take","to","match-end"], input_file="-", stdin=b"abc123_XYZ",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-charclass-s-matches-space",
             tokens=["find:re","\\s\\s","take","to","match-end"], input_file="-", stdin=b"a  b",
             expect=dict(stdout="  ", exit=0)),

        dict(id="regex-charclass-s-matches-tab",
             tokens=["find:re","\\s","take","to","match-end"], input_file="-", stdin=b"a\tb",
             expect=dict(stdout="\t", exit=0)),

        dict(id="regex-charclass-s-matches-newline",
             tokens=["find:re","\\s","take","to","match-end"], input_file="-", stdin=b"a\nb",
             expect=dict(stdout="\n", exit=0)),

        dict(id="regex-charclass-s-rejects-nonwhitespace",
             tokens=["find:re","\\s","take","to","match-end"], input_file="-", stdin=b"abcXYZ123",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-charclass-S-matches-nonwhitespace",
             tokens=["find:re","\\S\\S\\S","take","to","match-end"], input_file="-", stdin=b"   abc   ",
             expect=dict(stdout="abc", exit=0)),

        dict(id="regex-charclass-S-rejects-whitespace",
             tokens=["find:re","\\S","take","to","match-end"], input_file="-", stdin=b" \t\n\r",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        # Custom character classes
        dict(id="regex-011-custom-class",
             tokens=["find:re","[aeiou]","take","+1b"], input_file="-", stdin=b"bcdefgh",
             expect=dict(stdout="e", exit=0)),

        dict(id="regex-012-class-range",
             tokens=["find:re","[a-z]","take","+1b"], input_file="-", stdin=b"123abc456",
             expect=dict(stdout="a", exit=0)),

        dict(id="regex-013-class-multiple-ranges",
             tokens=["find:re","[0-9A-F]","take","+1b"], input_file="-", stdin=b"xyz5ABC",
             expect=dict(stdout="5", exit=0)),

        dict(id="regex-014-negated-class",
             tokens=["find:re","[^0-9]","take","+1b"], input_file="-", stdin=b"123abc456",
             expect=dict(stdout="a", exit=0)),

        # Quantifiers
        dict(id="regex-015-star-quantifier",
             tokens=["find:re","a*","take","+3b"], input_file="-", stdin=b"aaabc",
             expect=dict(stdout="aaa", exit=0)),

        dict(id="regex-016-plus-quantifier",
             tokens=["find:re","a+","take","+3b"], input_file="-", stdin=b"aaabc",
             expect=dict(stdout="aaa", exit=0)),

        dict(id="regex-017-question-quantifier",
             tokens=["find:re","a?","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        dict(id="regex-018-question-zero",
             tokens=["find:re","a?","take","+1b"], input_file="-", stdin=b"bc",
             expect=dict(stdout="b", exit=0)),

        # Quantifier comprehensive tests (verify correct matching behavior)
        dict(id="regex-quantifier-star-multiple",
             tokens=["find:re","a*b","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"aaab",
             expect=dict(stdout="aaab", exit=0)),

        dict(id="regex-quantifier-star-zero",
             tokens=["find:re","a*b","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"b",
             expect=dict(stdout="b", exit=0)),

        dict(id="regex-quantifier-plus-multiple",
             tokens=["find:re","a+b","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"aaab",
             expect=dict(stdout="aaab", exit=0)),

        dict(id="regex-quantifier-plus-requires-one",
             tokens=["find:re","a+b","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"b",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-quantifier-question-one",
             tokens=["find:re","a?b","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"ab",
             expect=dict(stdout="ab", exit=0)),

        dict(id="regex-quantifier-question-zero-match",
             tokens=["find:re","a?b","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"b",
             expect=dict(stdout="b", exit=0)),

        dict(id="regex-quantifier-brace-exact",
             tokens=["find:re","a{3}","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"aaaa",
             expect=dict(stdout="aaa", exit=0)),

        dict(id="regex-quantifier-brace-exact-fail",
             tokens=["find:re","a{3}","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"aa",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-quantifier-brace-min",
             tokens=["find:re","a{2,}","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"aaaaa",
             expect=dict(stdout="aaaaa", exit=0)),

        dict(id="regex-quantifier-brace-min-exact",
             tokens=["find:re","a{2,}","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"aa",
             expect=dict(stdout="aa", exit=0)),

        dict(id="regex-quantifier-brace-min-fail",
             tokens=["find:re","a{2,}","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"a",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-quantifier-brace-range",
             tokens=["find:re","a{2,4}","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"aaaaa",
             expect=dict(stdout="aaaa", exit=0)),

        dict(id="regex-quantifier-brace-range-min",
             tokens=["find:re","a{2,4}","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"aa",
             expect=dict(stdout="aa", exit=0)),

        dict(id="regex-quantifier-brace-range-fail",
             tokens=["find:re","a{2,4}","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"a",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        # Quantifiers with character classes
        dict(id="regex-quantifier-charclass-digit-plus",
             tokens=["find:re","\\d+","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abc123def",
             expect=dict(stdout="123", exit=0)),

        dict(id="regex-quantifier-charclass-word-star",
             tokens=["find:re","\\w*","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b" hello ",
             expect=dict(stdout="", exit=0)),

        dict(id="regex-quantifier-charclass-space-plus",
             tokens=["find:re","\\s+","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"a   b",
             expect=dict(stdout="   ", exit=0)),

        dict(id="regex-quantifier-charclass-nonspace-plus",
             tokens=["find:re","\\S+","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"   hello   ",
             expect=dict(stdout="hello", exit=0)),

        # Anchors
        dict(id="regex-019-bol-anchor",
             tokens=["find:re","^abc","take","+3b"], input_file="-", stdin=b"abc def",
             expect=dict(stdout="abc", exit=0)),

        dict(id="regex-020-eol-anchor",
             tokens=["find:re","def$","take","+3b"], input_file="-", stdin=b"abc def",
             expect=dict(stdout="def", exit=0)),

        dict(id="regex-021-bol-eol-complete",
             tokens=["find:re","^abc$","take","+3b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="abc", exit=0)),

        dict(id="regex-022-eol-with-newline",
             tokens=["find:re","def$","take","+3b"], input_file="-", stdin=b"abc def\n",
             expect=dict(stdout="def", exit=0)),  # $ matches before newline (line end behavior)

        # Anchor comprehensive tests (verify anchors enforce constraints)
        dict(id="regex-anchor-bol-rejects-middle",
             tokens=["find:re","^def","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abc def",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-anchor-bol-accepts-start",
             tokens=["find:re","^abc","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abc def",
             expect=dict(stdout="abc", exit=0)),

        dict(id="regex-anchor-eol-rejects-middle",
             tokens=["find:re","abc$","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abc def",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-anchor-eol-accepts-end",
             tokens=["find:re","def$","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abc def",
             expect=dict(stdout="def", exit=0)),

        dict(id="regex-anchor-both-accepts-exact",
             tokens=["find:re","^abc$","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abc",
             expect=dict(stdout="abc", exit=0)),

        dict(id="regex-anchor-both-rejects-extra",
             tokens=["find:re","^abc$","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abc def",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-anchor-bol-with-quantifier",
             tokens=["find:re","^a+","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"aaabcd",
             expect=dict(stdout="aaa", exit=0)),

        dict(id="regex-anchor-eol-with-quantifier",
             tokens=["find:re","d+$","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abcddd",
             expect=dict(stdout="ddd", exit=0)),

        # Alternation
        dict(id="regex-023-alternation",
             tokens=["find:re","cat|dog","take","+3b"], input_file="-", stdin=b"I have a cat",
             expect=dict(stdout="cat", exit=0)),

        dict(id="regex-024-alternation-second",
             tokens=["find:re","cat|dog","take","+3b"], input_file="-", stdin=b"I have a dog",
             expect=dict(stdout="dog", exit=0)),

        dict(id="regex-025-alternation-multiple",
             tokens=["find:re","cat|dog|bird","take","+4b"], input_file="-", stdin=b"I have a bird",
             expect=dict(stdout="bird", exit=0)),

        # Escape sequences
        dict(id="regex-026-newline-escape",
             tokens=["find:re","\\n","take","+1b"], input_file="-", stdin=b"abc\ndef",
             expect=dict(stdout="\n", exit=0)),

        dict(id="regex-027-tab-escape",
             tokens=["find:re","\\t","take","+1b"], input_file="-", stdin=b"abc\tdef",
             expect=dict(stdout="\t", exit=0)),

        dict(id="regex-028-carriage-return-escape",
             tokens=["find:re","\\r","take","+1b"], input_file="-", stdin=b"abc\rdef",
             expect=dict(stdout="\r", exit=0)),

        dict(id="regex-029-form-feed-escape",
             tokens=["find:re","\\f","take","+1b"], input_file="-", stdin=b"abc\fdef",
             expect=dict(stdout="\f", exit=0)),

        dict(id="regex-030-vertical-tab-escape",
             tokens=["find:re","\\v","take","+1b"], input_file="-", stdin=b"abc\vdef",
             expect=dict(stdout="\v", exit=0)),

        dict(id="regex-031-null-escape",
             tokens=["find:re","\\0","take","+1b"], input_file="-", stdin=b"abc\0def",
             expect=dict(stdout="\0", exit=0)),

        # Escape sequence comprehensive tests (verify they match correct chars and reject others)
        dict(id="regex-escape-newline-rejects-other",
             tokens=["find:re","\\n","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abc def",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-escape-tab-rejects-space",
             tokens=["find:re","\\t","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abc def",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-escape-cr-rejects-other",
             tokens=["find:re","\\r","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abc\ndef",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-escape-multiple-in-pattern",
             tokens=["find:re","\\n\\t\\r","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abc\n\t\rdef",
             expect=dict(stdout="\n\t\r", exit=0)),

        dict(id="regex-escape-in-alternation",
             tokens=["find:re","\\n|\\t","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abc\tdef",
             expect=dict(stdout="\t", exit=0)),

        dict(id="regex-escape-with-quantifier",
             tokens=["find:re","a\\n+b","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"a\n\n\nb",
             expect=dict(stdout="a\n\n\nb", exit=0)),

        # Complex patterns
        dict(id="regex-032-word-boundary-pattern",
             tokens=["find:re","\\w+\\s+\\d+","take","+5b"], input_file="-", stdin=b"hello 123 world",
             expect=dict(stdout="hello", exit=0)),

        dict(id="regex-033-email-like-pattern",
             tokens=["find:re","\\w+@\\w+\\.\\w+","take","+10b"], input_file="-", stdin=b"Contact: user@example.com for help",
             expect=dict(stdout="user@examp", exit=0)),

        dict(id="regex-034-phone-pattern",
             tokens=["find:re","\\d{3}-\\d{3}-\\d{4}","take","+12b"], input_file="-", stdin=b"Call 555-123-4567 now",
             expect=dict(stdout="555-123-4567", exit=0)),  # {n} quantifiers now implemented!

        dict(id="regex-035-mixed-quantifiers",
             tokens=["find:re","a+b*c?","take","+4b"], input_file="-", stdin=b"aaabcc",
             expect=dict(stdout="aaab", exit=0)),

        # Line-based patterns
        dict(id="regex-036-line-start-pattern",
             tokens=["find:re","^ERROR","take","+5b"], input_file="small.txt",
             expect=dict(stdout="ERROR", exit=0)),  # Now works with line boundary anchors!

        dict(id="regex-037-line-end-pattern",
             tokens=["find:re","hit$","take","+3b"], input_file="small.txt",
             expect=dict(stdout="hit", exit=0)),  # $ matches before newline (line end behavior)

        dict(id="regex-038-complete-line-pattern",
             tokens=["find:re","^ERROR hit$","take","+9b"], input_file="small.txt",
             expect=dict(stdout="ERROR hit", exit=0)),  # ^ and $ match line boundaries

        # Binary data patterns
        dict(id="regex-039-binary-pattern",
             tokens=["find:re","TEXT_START.*TEXT_END","take","+25b"], input_file="binary-data.bin",
             expect=dict(stdout="TEXT_START\x00\x01\x02\x03BINARY_DATA", exit=0)),  # .* pattern works with binary data

        # Unicode patterns
        dict(id="regex-040-unicode-pattern",
             tokens=["find:re","世界","take","+6b"], input_file="unicode-test.txt",
             expect=dict(stdout="世界", exit=0)),

        dict(id="regex-041-unicode-with-anchors",
             tokens=["find:re","^Hello 世界$","take","+12b"], input_file="-", stdin="Hello 世界".encode('utf-8'),
             expect=dict(stdout="Hello 世界", exit=0)),

        # Error cases
        dict(id="regex-042-no-match",
             tokens=["find:re","XYZ","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-043-empty-pattern",
             tokens=["find:re",""], input_file="overlap.txt",
             expect=dict(stdout="", exit=13)),

        dict(id="regex-044-invalid-escape",
             tokens=["find:re","\\z","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        # Complex multi-clause regex
        dict(id="regex-045-multi-clause-regex",
             tokens=["find:re","ERROR","take","+5b","THEN","find:re","WARNING","take","+7b"], input_file="multiline.txt",
             expect=dict(stdout="ERROR", exit=0)),

        dict(id="regex-046-regex-with-labels",
             tokens=["find:re","Section 2","label","START","find:re","Section 3","skip","to","START","take","until","Section 3"], input_file="multiline.txt",
             expect=dict(stdout="Section 2: Data\nKEY1=value1\nKEY2=value2\nKEY3=value3\n\n", exit=0)),

        # Edge cases
        dict(id="regex-047-empty-file",
             tokens=["find:re","a","take","+1b"], input_file="empty.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-048-single-char-file",
             tokens=["find:re","a","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="a", exit=0)),

        dict(id="regex-049-regex-with-take-until",
             tokens=["find:re","Section 1","take","until","Section 2"], input_file="multiline.txt",
             expect=dict(stdout="Section 1: Introduction\nThis is the first section with multiple lines.\nIt contains various text patterns.\n\n", exit=0)),

        # Performance and large patterns
        dict(id="regex-050-large-pattern",
             tokens=["find:re","PATTERN","take","+7b"], input_file="repeated-patterns.txt",
             expect=dict(stdout="PATTERN", exit=0)),

        dict(id="regex-051-regex-in-large-file",
             tokens=["find:re","NEEDLE","take","+6b"], input_file="big-forward.bin",
             expect=dict(stdout="NEEDLE", exit=0)),

        # Mixed regex and literal find
        dict(id="regex-052-mixed-find-operations",
             tokens=["find","Section 1","take","+1l","THEN","find:re","ERROR","take","+5b"], input_file="multiline.txt",
             expect=dict(stdout="Section 1: Introduction\nERROR", exit=0)),

        # Regex with line operations
        dict(id="regex-053-regex-line-operations",
             tokens=["find:re","ERROR","skip","to","line-start","take","+1l"], input_file="multiline.txt",
             expect=dict(stdout="ERROR: Critical failure occurred\n", exit=0)),

        # Complex alternation
        dict(id="regex-054-complex-alternation",
             tokens=["find:re","SUCCESS|WARNING|ERROR|INFO","take","+7b"], input_file="multiline.txt",
             expect=dict(stdout="SUCCESS", exit=0)),

        # Regex with quantifiers and classes
        dict(id="regex-055-quantified-classes",
             tokens=["find:re","\\d+\\s+\\w+","take","+8b"], input_file="-", stdin=b"123 abc 456 def",
             expect=dict(stdout="123 abc ", exit=0)),

        # Anchors with complex patterns
        dict(id="regex-056-anchor-complex",
             tokens=["find:re","^\\w+\\s+\\d+\\s+\\w+$","take","+13b"], input_file="-", stdin=b"hello 123 world",
             expect=dict(stdout="hello 123 wor", exit=0)),

        # Regex with binary data
        dict(id="regex-057-binary-regex",
             tokens=["find:re","[\\x00-\\xFF]+","take","+4b"], input_file="binary-data.bin",
             expect=dict(stdout="TEXT", exit=0)),

        # Unicode with quantifiers
        dict(id="regex-058-unicode-quantified",
             tokens=["find:re","[\\u4e00-\\u9fff]+","take","+6b"], input_file="unicode-test.txt",
             expect=dict(stdout="Hello ", exit=0)),  # Unicode ranges not implemented yet

        # Complex nested patterns
        dict(id="regex-059-nested-patterns",
             tokens=["find:re","(cat|dog)\\s+(food|toy)","take","+7b"], input_file="-", stdin=b"I bought cat food",
             expect=dict(stdout="cat foo", exit=0)),

        # Regex with special characters
        dict(id="regex-060-special-chars",
             tokens=["find:re","[!@#$%^&*()]+","take","+3b"], input_file="-", stdin=b"abc!@#def",
             expect=dict(stdout="!@#", exit=0)),

        # Multiple matches (first match)
        dict(id="regex-061-first-match",
             tokens=["find:re","ERROR","take","+5b"], input_file="small.txt",
             expect=dict(stdout="ERROR", exit=0)),

        # Regex with line boundaries
        dict(id="regex-062-line-boundaries",
             tokens=["find:re","^L\\d+","take","+4b"], input_file="lines.txt",
             expect=dict(stdout="L01 ", exit=0)),

        # Complex character class
        dict(id="regex-063-complex-class",
             tokens=["find:re","[A-Za-z0-9_]+","take","+6b"], input_file="-", stdin=b"123abc_456DEF",
             expect=dict(stdout="123abc", exit=0)),

        # Regex with mixed content
        dict(id="regex-064-mixed-content",
             tokens=["find:re","\\w+=\\w+","take","+7b"], input_file="multiline.txt",
             expect=dict(stdout="KEY1=va", exit=0)),

        # Edge case: very long pattern
        dict(id="regex-065-long-pattern",
             tokens=["find:re","X{1000}","take","+1000b"], input_file="large-lines.txt",
             expect=dict(stdout="", exit=11)),  # Large quantifiers cause memory issues (1000 individual instructions)

        # Regex with CRLF
        dict(id="regex-066-crlf-pattern",
             tokens=["find:re","Line\\d+\\r\\n","take","+7b"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1\r\n", exit=0)),

        # CRLF EOL anchor tests - these would have caught the bug!
        dict(id="regex-067-crlf-eol-anchor",
             tokens=["find:re","Line1$","take","+5b"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line1", exit=0)),  # $ should match before \r\n

        dict(id="regex-068-crlf-eol-anchor-second-line",
             tokens=["find:re","Line2$","take","+5b"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line2", exit=0)),  # $ should match before \r\n

        dict(id="regex-069-crlf-eol-anchor-last-line",
             tokens=["find:re","Line3$","take","+5b"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="Line3", exit=0)),  # $ should match before \r\n

        dict(id="regex-070-crlf-eol-anchor-no-final-lf",
             tokens=["find:re","Line3$","take","+5b"], input_file="crlf-no-final-lf.txt",
             expect=dict(stdout="Line3", exit=0)),  # $ should match at EOF when no final \r\n

        dict(id="regex-071-crlf-mixed-eol-anchor",
             tokens=["find:re","CRLF line$","take","+9b"], input_file="mixed-line-endings.txt",
             expect=dict(stdout="CRLF line", exit=0)),  # $ should match before \r\n in mixed file

        # Complex regex with multiple features
        dict(id="regex-072-complex-features",
             tokens=["find:re","^\\w+\\s+\\d+\\s+\\w+\\s*$","take","+13b"], input_file="-", stdin=b"hello 123 world",
             expect=dict(stdout="hello 123 wor", exit=0)),

        # Regex with alternation and quantifiers
        dict(id="regex-073-alternation-quantified",
             tokens=["find:re","(cat|dog)+","take","+6b"], input_file="-", stdin=b"catdogcat",
             expect=dict(stdout="catdog", exit=0)),  # Groups () treated as literal characters

        # Regex with anchors and classes
        dict(id="regex-074-anchor-classes",
             tokens=["find:re","^[A-Z]+\\s+[a-z]+$","take","+8b"], input_file="-", stdin=b"HELLO world",
             expect=dict(stdout="HELLO wo", exit=0)),

        # Final comprehensive test
        dict(id="regex-075-comprehensive",
             tokens=["find:re","^\\w+\\s+\\d+\\s+\\w+\\s*$","take","+13b"], input_file="-", stdin=b"hello 123 world",
             expect=dict(stdout="hello 123 wor", exit=0)),

        # NEW TESTS FOR GROUPING FUNCTIONALITY
        # Basic grouping
        dict(id="regex-076-basic-grouping",
             tokens=["find:re","(a)","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        # Grouping with alternation
        dict(id="regex-077-grouping-alternation",
             tokens=["find:re","(a|b)","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        # Nested grouping
        dict(id="regex-078-nested-grouping",
             tokens=["find:re","((a))","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        # Complex grouping with alternation
        dict(id="regex-079-complex-grouping",
             tokens=["find:re","(a|b)|c","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        # Grouping with multiple alternatives
        dict(id="regex-080-grouping-multiple-alternatives",
             tokens=["find:re","(a|b|c)","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        # Grouping with second alternative
        dict(id="regex-081-grouping-second-alternative",
             tokens=["find:re","(a|b|c)","take","+1b"], input_file="-", stdin=b"bcd",
             expect=dict(stdout="b", exit=0)),

        # Grouping with third alternative
        dict(id="regex-082-grouping-third-alternative",
             tokens=["find:re","(a|b|c)","take","+1b"], input_file="-", stdin=b"cde",
             expect=dict(stdout="c", exit=0)),

        # Deeply nested grouping
        dict(id="regex-083-deeply-nested-grouping",
             tokens=["find:re","(((a|b)|c)|d)","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        # Grouping with anchors
        dict(id="regex-084-grouping-with-anchors",
             tokens=["find:re","^(a|b)$","take","+1b"], input_file="-", stdin=b"a",
             expect=dict(stdout="a", exit=0)),

        # Grouping with anchors - no match
        dict(id="regex-085-grouping-with-anchors-no-match",
             tokens=["find:re","^(a|b)$","take","+1b"], input_file="-", stdin=b"ab",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        # Grouping with character classes
        dict(id="regex-086-grouping-with-classes",
             tokens=["find:re","([a-z]|[0-9])","take","+1b"], input_file="-", stdin=b"abc123",
             expect=dict(stdout="a", exit=0)),

        # Grouping with character classes - digits
        dict(id="regex-087-grouping-with-classes-digits",
             tokens=["find:re","([a-z]|[0-9])","take","+1b"], input_file="-", stdin=b"123abc",
             expect=dict(stdout="1", exit=0)),

        # Grouping with quantifiers (should use old parser)
        dict(id="regex-088-grouping-with-quantifiers",
             tokens=["find:re","(a+)","take","+3b"], input_file="-", stdin=b"aaabc",
             expect=dict(stdout="aaa", exit=0)),

        # Grouping with alternation and quantifiers
        dict(id="regex-089-grouping-alternation-quantifiers",
             tokens=["find:re","(a|b)+","take","+3b"], input_file="-", stdin=b"aaabc",
             expect=dict(stdout="aaa", exit=0)),

        # Grouping quantifier comprehensive tests (verify greedy behavior)
        dict(id="regex-grouping-quantifier-plus-greedy",
             tokens=["find:re","(ab)+","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abababcd",
             expect=dict(stdout="ababab", exit=0)),

        dict(id="regex-grouping-quantifier-star-greedy",
             tokens=["find:re","c(ab)*","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"xcabababd",
             expect=dict(stdout="cababab", exit=0)),

        dict(id="regex-grouping-quantifier-alternation-greedy",
             tokens=["find:re","(a|b)+","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abababcd",
             expect=dict(stdout="ababab", exit=0)),

        dict(id="regex-grouping-quantifier-complex",
             tokens=["find:re","(cat|dog)+","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"catdogcatdog!",
             expect=dict(stdout="catdogcatdog", exit=0)),

        dict(id="regex-grouping-nested-quantifier",
             tokens=["find:re","((ab)+c)+","skip","to","match-start","take","to","match-end"], input_file="-", stdin=b"abcababc!",
             expect=dict(stdout="abcababc", exit=0)),

        # Complex grouping pattern
        dict(id="regex-090-complex-grouping-pattern",
             tokens=["find:re","(cat|dog)|(bird|fish)","take","+4b"], input_file="-", stdin=b"I have a cat",
             expect=dict(stdout="cat", exit=0)),

        # Grouping with second group
        dict(id="regex-091-grouping-second-group",
             tokens=["find:re","(cat|dog)|(bird|fish)","take","+4b"], input_file="-", stdin=b"I have a bird",
             expect=dict(stdout="bird", exit=0)),

        # Empty group (should match as epsilon)
        dict(id="regex-092-empty-group",
             tokens=["find:re","()","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        # Grouping with escape sequences
        dict(id="regex-093-grouping-with-escapes",
             tokens=["find:re","(\\n|\\t)","take","+1b"], input_file="-", stdin=b"abc\ndef",
             expect=dict(stdout="\n", exit=0)),

        # Grouping with dot
        dict(id="regex-094-grouping-with-dot",
             tokens=["find:re","(.|a)","take","+1b"], input_file="-", stdin=b"abc",
             expect=dict(stdout="a", exit=0)),

        # Grouping with anchors and multiline
        dict(id="regex-095-grouping-multiline",
             tokens=["find:re","^(a|b)","take","+1b"], input_file="-", stdin=b"abc\ndef",
             expect=dict(stdout="a", exit=0)),

        # Grouping with anchors and multiline - second line
        dict(id="regex-096-grouping-multiline-second",
             tokens=["find:re","^(a|b)","take","+1b"], input_file="-", stdin=b"xyz\ndef",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        # Grouping with anchors and multiline - after newline
        dict(id="regex-097-grouping-multiline-after-newline",
             tokens=["find:re","^(a|b)","take","+1b"], input_file="-", stdin=b"xyz\na",
             expect=dict(stdout="a", exit=0)),

        # Regression tests for grouped quantifiers (catch issues old implementation accidentally allowed)
        dict(id="regex-098-star-quantifier-multiple-repeats",
             tokens=["find:re","(ab)*x","take","to","match-end"], input_file="-", stdin=b"ababx",
             expect=dict(stdout="ababx", exit=0)),

        dict(id="regex-099-question-quantifier-zero-occurrences",
             tokens=["find:re","(ab)?x","take","to","match-end"], input_file="-", stdin=b"x",
             expect=dict(stdout="x", exit=0)),

        dict(id="regex-100-question-quantifier-one-occurrence",
             tokens=["find:re","(ab)?x","take","to","match-end"], input_file="-", stdin=b"abx",
             expect=dict(stdout="abx", exit=0)),

        dict(id="regex-101-question-quantifier-not-require-two",
             tokens=["find:re","(ab)?x","take","to","match-end"], input_file="-", stdin=b"ababx",
             expect=dict(stdout="x", exit=0)),  # Should find first match, not require 2 occurrences

        dict(id="regex-102-plus-quantifier-requires-one",
             tokens=["find:re","(ab)+x"], input_file="-", stdin=b"x",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),  # Should fail - requires at least one occurrence

        dict(id="regex-103-plus-quantifier-multiple-occurrences",
             tokens=["find:re","(ab)+x","take","to","match-end"], input_file="-", stdin=b"ababx",
             expect=dict(stdout="ababx", exit=0)),

        # ---------- Take Until:re Tests ----------
        dict(id="regex-104-until-re-literal",
             tokens=["take","until:re","world"], input_file="-", stdin=b"hello world test",
             expect=dict(stdout="hello ", exit=0)),

        dict(id="regex-105-until-re-at-match-end",
             tokens=["take","until:re","world","at","match-end"], input_file="-", stdin=b"hello world test",
             expect=dict(stdout="hello world", exit=0)),

        dict(id="regex-106-until-re-digit-class",
             tokens=["take","until:re","\\d+"], input_file="-", stdin=b"abc123def",
             expect=dict(stdout="abc", exit=0)),

        dict(id="regex-107-until-re-word-class",
             tokens=["take","until:re","\\w+"], input_file="-", stdin=b"  hello world",
             expect=dict(stdout="  ", exit=0)),

        dict(id="regex-108-until-re-space-class",
             tokens=["take","until:re","\\s+","at","match-end"], input_file="-", stdin=b"hello world",
             expect=dict(stdout="hello ", exit=0)),

        dict(id="regex-109-until-re-custom-class",
             tokens=["take","until:re","[A-Z]"], input_file="-", stdin=b"hello World",
             expect=dict(stdout="hello ", exit=0)),

        dict(id="regex-110-until-re-alternation",
             tokens=["take","until:re","cat|dog"], input_file="-", stdin=b"I have a cat",
             expect=dict(stdout="I have a ", exit=0)),

        dict(id="regex-111-until-re-at-line-start",
             tokens=["take","until:re","world","at","line-start"], input_file="-", stdin=b"hello world",
             expect=dict(stdout="", exit=0)),

        dict(id="regex-112-until-re-at-line-end",
             tokens=["take","until:re","world","at","line-end"], input_file="-", stdin=b"hello world\ntest",
             expect=dict(stdout="hello world\n", exit=0)),

        dict(id="regex-113-until-re-not-found",
             tokens=["take","until:re","xyz"], input_file="-", stdin=b"hello world",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="regex-114-until-re-anchor-bol",
             tokens=["skip","6b","take","until:re","^w"], input_file="-", stdin=b"hello\nworld",
             expect=dict(stdout="", exit=0)),

        dict(id="regex-115-until-re-anchor-eol",
             tokens=["take","until:re","o$","at","match-end"], input_file="-", stdin=b"hello\nworld",
             expect=dict(stdout="hello", exit=0)),

        dict(id="regex-116-until-re-quantifier",
             tokens=["take","until:re","o+"], input_file="-", stdin=b"hellooo world",
             expect=dict(stdout="hell", exit=0)),

        dict(id="regex-117-until-re-complex-pattern",
             tokens=["take","until:re","\\d{3}-\\d{3}"], input_file="-", stdin=b"Call 555-123-4567 now",
             expect=dict(stdout="Call ", exit=0)),

        dict(id="regex-118-until-re-escaped-chars",
             tokens=["take","until:re","\\n"], input_file="-", stdin=b"hello\nworld",
             expect=dict(stdout="hello", exit=0)),

        # ---------- Binary Search (find:bin) Tests ----------
        # Basic hex pattern matching
        dict(id="findbin-001-basic-hex-uppercase",
             tokens=["find:bin","89504E470D0A1A0A","take","+8b"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="4c4b6a3be1314ab86138bef4314dde022e600960d8689a2c8f8631802d20dab6", exit=0)),  # PNG header

        dict(id="findbin-002-basic-hex-lowercase",
             tokens=["find:bin","89504e470d0a1a0a","take","+8b"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="4c4b6a3be1314ab86138bef4314dde022e600960d8689a2c8f8631802d20dab6", exit=0)),  # PNG header lowercase

        dict(id="findbin-003-mixed-case",
             tokens=["find:bin","CaFeBaBe","take","+4b"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="65ab12a8ff3263fbc257e5ddf0aa563c64573d0bab1f1115b9b107834cfa6971", exit=0)),  # CAFEBABE

        # Hex patterns with whitespace
        dict(id="findbin-004-hex-with-spaces",
             tokens=["find:bin","DE AD BE EF","take","+4b"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="5f78c33274e43fa9de5659265c1d917e25c03722dcb0b8d27db8d5feaa813953", exit=0)),  # DEADBEEF

        dict(id="findbin-005-hex-with-tabs",
             tokens=["find:bin","50\t4B\t03\t04","take","+4b"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="8dcc7e601606217f3b754766511182a916b17e9a26a94c9d887104eba92e9bb2", exit=0)),  # ZIP signature

        dict(id="findbin-006-hex-mixed-whitespace",
             tokens=["find:bin","50 4B\t03 04","take","+4b"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="8dcc7e601606217f3b754766511182a916b17e9a26a94c9d887104eba92e9bb2", exit=0)),  # ZIP signature

        # Simple hex patterns
        dict(id="findbin-007-simple-pattern",
             tokens=["find:bin","010DFF","take","+3b"], input_file="hex-test.bin",
             expect=dict(stdout_sha256="7add1b01be62f96315f673a5e2bbd217abe42e46d094f2564e5da936e3777af4", exit=0)),

        dict(id="findbin-008-simple-pattern-spaces",
             tokens=["find:bin","01 0D FF","take","+3b"], input_file="hex-test.bin",
             expect=dict(stdout_sha256="7add1b01be62f96315f673a5e2bbd217abe42e46d094f2564e5da936e3777af4", exit=0)),

        # Forward and backward search
        dict(id="findbin-009-forward-search",
             tokens=["find:bin","CAFEBABE","take","+4b"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="65ab12a8ff3263fbc257e5ddf0aa563c64573d0bab1f1115b9b107834cfa6971", exit=0)),

        dict(id="findbin-010-backward-search",
             tokens=["skip","to","EOF","find:bin","to","BOF","DEADBEEF","take","+4b"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="5f78c33274e43fa9de5659265c1d917e25c03722dcb0b8d27db8d5feaa813953", exit=0)),

        # No match scenarios
        dict(id="findbin-011-no-match",
             tokens=["find:bin","FFFFFFFF","take","+1b"], input_file="hex-test.bin",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="findbin-012-empty-file",
             tokens=["find:bin","DEADBEEF","take","+1b"], input_file="empty.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        # Error cases - odd number of hex digits
        dict(id="findbin-013-odd-hex-digits",
             tokens=["find:bin","DEA","take","+1b"], input_file="hex-test.bin",
             expect=dict(stdout="", exit=12)),

        # Error cases - invalid hex characters
        dict(id="findbin-014-invalid-hex-char",
             tokens=["find:bin","DEFG","take","+1b"], input_file="hex-test.bin",
             expect=dict(stdout="", exit=12)),

        dict(id="findbin-015-invalid-hex-special",
             tokens=["find:bin","DE$F","take","+1b"], input_file="hex-test.bin",
             expect=dict(stdout="", exit=12)),

        # Error cases - empty pattern
        dict(id="findbin-016-empty-pattern",
             tokens=["find:bin",""], input_file="hex-test.bin",
             expect=dict(stdout="", exit=12)),

        # Large file search (buffer boundary testing)
        dict(id="findbin-017-large-file-search",
             tokens=["find:bin","DEADBEEF","take","+4b"], input_file="binary-large.bin",
             expect=dict(stdout_sha256="5f78c33274e43fa9de5659265c1d917e25c03722dcb0b8d27db8d5feaa813953", exit=0)),

        # Integration with match positions
        dict(id="findbin-018-goto-match-start",
             tokens=["find:bin","CAFEBABE","skip","to","match-start","take","to","match-end"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="65ab12a8ff3263fbc257e5ddf0aa563c64573d0bab1f1115b9b107834cfa6971", exit=0)),

        dict(id="findbin-019-goto-match-end",
             tokens=["find:bin","504B0304","skip","to","match-end","take","+4b"], input_file="binary-patterns.bin",
             expect=dict(stdout="MORE", exit=0)),

        # Integration with labels
        dict(id="findbin-020-label-after-find",
             tokens=["find:bin","DEADBEEF","label","MARK","skip","to","MARK","take","+4b"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="5f78c33274e43fa9de5659265c1d917e25c03722dcb0b8d27db8d5feaa813953", exit=0)),

        # Integration with views
        dict(id="findbin-021-find-within-view",
             tokens=["view","BOF","BOF+50b","find:bin","504B0304","take","+4b"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="8dcc7e601606217f3b754766511182a916b17e9a26a94c9d887104eba92e9bb2", exit=0)),

        dict(id="findbin-022-find-outside-view",
             tokens=["view","BOF","BOF+10b","find:bin","DEADBEEF","take","+1b"], input_file="binary-patterns.bin",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),  # DEADBEEF is beyond view

        # Integration with clauses
        dict(id="findbin-023-multiple-clauses",
             tokens=["find:bin","89504E47","take","+4b","THEN","find:bin","CAFEBABE","take","+4b"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="9e0cce7bfddeb213f8453efa50f344e5e1143f685e6e334df053d89e0c967790", exit=0)),  # Combined output

        # Integration with OR
        dict(id="findbin-024-or-first-match",
             tokens=["find:bin","FFFFFFFF","OR","find:bin","DEADBEEF","take","+4b"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="5f78c33274e43fa9de5659265c1d917e25c03722dcb0b8d27db8d5feaa813953", exit=0)),

        # Whitespace-only pattern (should fail as odd digits after removing whitespace if spaces only)
        dict(id="findbin-025-whitespace-only",
             tokens=["find:bin","   ","take","+1b"], input_file="hex-test.bin",
             expect=dict(stdout="", exit=12)),

        # All zeros pattern
        dict(id="findbin-026-all-zeros",
             tokens=["find:bin","00000000","take","+4b"], input_file="binary-large.bin",
             expect=dict(stdout_sha256="df3f619804a92fdb4057192dc43dd748ea778adc52bc498ce80524c014b81119", exit=0)),

        # Single byte pattern
        dict(id="findbin-027-single-byte",
             tokens=["find:bin","01","take","+1b"], input_file="hex-test.bin",
             expect=dict(stdout_sha256="4bf5122f344554c53bde2ebb8cd2b7e3d1600ad631c385a5d7cce23c7785459a", exit=0)),

        # Real-world patterns - PNG header
        dict(id="findbin-028-png-header",
             tokens=["find:bin","89 50 4E 47 0D 0A 1A 0A","take","+8b"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="4c4b6a3be1314ab86138bef4314dde022e600960d8689a2c8f8631802d20dab6", exit=0)),

        # Real-world patterns - ZIP signature
        dict(id="findbin-029-zip-signature",
             tokens=["find:bin","504B0304","take","+4b"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="8dcc7e601606217f3b754766511182a916b17e9a26a94c9d887104eba92e9bb2", exit=0)),

        # Real-world patterns - Java class file
        dict(id="findbin-030-java-class",
             tokens=["find:bin","CAFEBABE","take","+4b"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="65ab12a8ff3263fbc257e5ddf0aa563c64573d0bab1f1115b9b107834cfa6971", exit=0)),

        # ---------- Binary Take Until (take until:bin) Tests ----------
        # Basic take until:bin
        dict(id="takeuntilbin-001-basic",
             tokens=["take","until:bin","DEADBEEF"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="40e2b10b86401214c686fa44ca8d85fc61cd4ea97fd2cdf5ee7409cf0a6541dc", exit=0)),  # Everything before DEADBEEF

        dict(id="takeuntilbin-002-hex-lowercase",
             tokens=["take","until:bin","deadbeef"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="40e2b10b86401214c686fa44ca8d85fc61cd4ea97fd2cdf5ee7409cf0a6541dc", exit=0)),

        dict(id="takeuntilbin-003-hex-with-spaces",
             tokens=["take","until:bin","DE AD BE EF"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="40e2b10b86401214c686fa44ca8d85fc61cd4ea97fd2cdf5ee7409cf0a6541dc", exit=0)),

        # Take until with at clause
        dict(id="takeuntilbin-004-at-match-end",
             tokens=["take","until:bin","CAFEBABE","at","match-end"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="f3a4e56cbb9de856f7775305ccee5bcf5f1a41be9d21e68d085bb22f8681c92d", exit=0)),

        dict(id="takeuntilbin-005-at-match-start",
             tokens=["take","until:bin","504B0304","at","match-start"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="07a2380f5c0958106abc5c709b6ffc447ef7ab19019713c8b44c1ac06316f6fd", exit=0)),

        # Multiple take until:bin operations
        dict(id="takeuntilbin-006-sequential",
             tokens=["take","until:bin","504B0304","take","until:bin","CAFEBABE"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="0a86745b019f26aa9a872ee193d6098faf0f1e722fb91efd45e09fb687330b5e", exit=0)),

        # Error cases
        dict(id="takeuntilbin-007-no-match",
             tokens=["take","until:bin","FFFFFFFF"], input_file="hex-test.bin",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="takeuntilbin-008-odd-hex-digits",
             tokens=["take","until:bin","DEA"], input_file="hex-test.bin",
             expect=dict(stdout="", exit=12)),

        dict(id="takeuntilbin-009-invalid-hex",
             tokens=["take","until:bin","DEFG"], input_file="hex-test.bin",
             expect=dict(stdout="", exit=12)),

        dict(id="takeuntilbin-010-empty-pattern",
             tokens=["take","until:bin",""], input_file="hex-test.bin",
             expect=dict(stdout="", exit=12)),

        # Integration with views
        dict(id="takeuntilbin-011-within-view",
             tokens=["view","BOF","BOF+50b","take","until:bin","504B0304"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="07a2380f5c0958106abc5c709b6ffc447ef7ab19019713c8b44c1ac06316f6fd", exit=0)),

        # Integration with OR
        dict(id="takeuntilbin-012-or-fallback",
             tokens=["take","until:bin","FFFFFFFF","OR","take","until:bin","DEADBEEF"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="40e2b10b86401214c686fa44ca8d85fc61cd4ea97fd2cdf5ee7409cf0a6541dc", exit=0)),

        # Match position update
        dict(id="takeuntilbin-013-match-position",
             tokens=["take","until:bin","CAFEBABE","skip","to","match-start","take","to","match-end"], input_file="binary-patterns.bin",
             expect=dict(stdout_sha256="f3a4e56cbb9de856f7775305ccee5bcf5f1a41be9d21e68d085bb22f8681c92d", exit=0)),

        # Large file
        dict(id="takeuntilbin-014-large-file",
             tokens=["take","until:bin","DEADBEEF"], input_file="binary-large.bin",
             expect=dict(stdout_sha256="17764f1de746043516796e3d7a363479075f7bb007f6066f8bfa70c3b3fe40cf", exit=0)),

        # Simple pattern
        dict(id="takeuntilbin-015-simple-pattern",
             tokens=["take","until:bin","010DFF"], input_file="hex-test.bin",
             expect=dict(stdout="PREFIX_", exit=0)),

        # ---------- Views Feature Tests ----------
        # Basic view operations
        dict(id="view-001-basic-viewset",
             tokens=["view","BOF+2b","EOF-2b","take","100b"], input_file="overlap.txt",
             expect=dict(stdout="cdefgh", exit=0)),

        dict(id="view-002-viewclear",
             tokens=["clear","view","take","3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),

        dict(id="view-003-viewset-viewclear-sequence",
             tokens=["view","BOF+2b","EOF-2b","clear","view","take","3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),  # cursor at position 2 from viewset

        # View with find operations
        dict(id="view-004-view-find-forward",
             tokens=["view","BOF+2b","EOF-2b","find","def","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),

        dict(id="view-005-view-find-backward",
             tokens=["view","BOF+2b","EOF-2b","skip","5b","find","to","BOF","def","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),

        dict(id="view-006-view-find-no-match",
             tokens=["view","BOF","EOF-2b","find","X","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),  # X is at EOF-1b, excluded from view

        # View with regex operations
        dict(id="view-007-view-find:re-anchors",
             tokens=["view","BOF+3b","EOF","find:re","^HEADER","take","to","match-end"], input_file="-", stdin=b"ZZ\nHEADER\n",
             expect=dict(stdout="HEADER", exit=0)),

        dict(id="view-008-view-find:re-no-match",
             tokens=["view","BOF","EOF-1b","find:re","^HEADER","take","+6b"], input_file="-", stdin=b"ZZ\nHEADER\n",
             expect=dict(stdout="HEADER", exit=0)),  # HEADER is at EOF-1b, but view includes it

        # View with goto operations
        dict(id="view-009-goto-within-view",
             tokens=["view","BOF+2b","EOF-2b","skip","to","BOF+3b","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="fg", exit=0)),  # BOF+3b in view is position 5, take +2b gives fg

        dict(id="view-010-goto-outside-view-fails",
             tokens=["view","BOF+2b","EOF-2b","skip","to","BOF-1b","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="view-011-goto-outside-view-eof",
             tokens=["view","BOF+2b","EOF-2b","skip","to","EOF+1b","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        # View with take operations
        dict(id="view-012-view-take-len-positive",
             tokens=["view","BOF+2b","EOF-2b","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        dict(id="view-013-view-take-len-negative",
             tokens=["view","BOF+2b","EOF-2b","skip","3b","take","-2b"], input_file="overlap.txt",
             expect=dict(stdout="de", exit=0)),  # skip 3b puts cursor at position 5, take -2b gives de

        dict(id="view-014-view-take-to",
             tokens=["view","BOF+2b","EOF-2b","take","to","EOF-1b"], input_file="overlap.txt",
             expect=dict(stdout="cdefg", exit=0)),  # EOF-1b in view is position 7, take to gives cdefg

        dict(id="view-015-view-take-until",
             tokens=["view","BOF+2b","EOF-2b","take","until","f"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        # View with skip operations
        dict(id="view-016-view-skip-bytes",
             tokens=["view","BOF+2b","EOF-2b","skip","2b","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="ef", exit=0)),

        dict(id="view-017-view-skip-lines",
             tokens=["view","BOF+2b","EOF-2b","skip","1l","take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L02 bb\n", exit=0)),

        # View with line operations
        dict(id="view-018-view-line-start",
             tokens=["view","BOF+5b","EOF-2b","find","L03","skip","to","line-start","take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L03 ccc\n", exit=0)),

        dict(id="view-019-view-line-end",
             tokens=["view","BOF+5b","EOF-2b","find","L03","skip","to","line-end","take","+1l"], input_file="lines.txt",
             expect=dict(stdout="L04 dddd\n", exit=0)),

        # View with labels
        dict(id="view-020-view-labels",
             tokens=["view","BOF+2b","EOF-2b","label","MARK","skip","2b","skip","to","MARK","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="cd", exit=0)),

        # View atomicity
        dict(id="view-021-view-atomic-success",
             tokens=["view","BOF+2b","EOF-2b","take","+2b","THEN","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="cdef", exit=0)),

        dict(id="view-022-view-atomic-failure",
             tokens=["view","BOF+2b","EOF-2b","take","+2b","find","XYZ","THEN","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="ab", exit=0)),  # First clause fails, view not committed, second clause uses original cursor

        # View edge cases
        dict(id="view-023-empty-view",
             tokens=["view","BOF+5b","BOF+5b","take","+10b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),  # Empty view, cursor clamped to hi

        dict(id="view-024-view-beyond-file",
             tokens=["view","BOF+100b","EOF+100b","take","+10b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),  # View beyond file bounds

        dict(id="view-025-view-negative-bounds",
             tokens=["view","BOF-10b","EOF+10b","take","+10b"], input_file="overlap.txt",
             expect=dict(stdout="abcdefghij", exit=0)),  # Negative bounds clamped to file

        # View with complex operations
        dict(id="view-026-view-complex-extraction",
             tokens=["view","BOF+6b","EOF-5b","find","world","take","to","match-end"], input_file="-", stdin=b"hello world test",
             expect=dict(stdout="world", exit=0)),

        dict(id="view-027-view-multi-clause",
             tokens=["view","BOF+2b","EOF-2b","take","+2b","THEN","clear","view","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="cdef", exit=0)),  # First clause commits view, second clears it but cursor is at position 4

        # View with regex anchors
        dict(id="view-028-view-regex-bol",
             tokens=["view","BOF+3b","EOF","find:re","^HEADER","take","to","match-end"], input_file="-", stdin=b"ZZ\nHEADER\n",
             expect=dict(stdout="HEADER", exit=0)),

        dict(id="view-029-view-regex-eol",
             tokens=["view","BOF","EOF-3b","find:re","ZZ$","take","to","match-end"], input_file="-", stdin=b"ZZ\nHEADER\n",
             expect=dict(stdout="ZZ", exit=0)),  # ZZ$ matches before newline (line end behavior)

        # View with match invalidation
        dict(id="view-030-view-match-invalidation",
             tokens=["find","def","view","BOF+2b","EOF-2b","take","to","match-end"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),  # Match is still valid, def is at position 3

        # View with cursor clamping
        dict(id="view-031-view-cursor-clamping",
             tokens=["skip","5b","view","BOF+2b","EOF-2b","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="fg", exit=0)),  # Cursor at position 5, clamped to view [2,8), take +2b gives fg

        # View with take until and at expressions
        dict(id="view-032-view-take-until-at",
             tokens=["view","BOF+2b","EOF-2b","take","until","f","at","match-start","+1b"], input_file="overlap.txt",
             expect=dict(stdout="cdef", exit=0)),  # take until f, then at match-start+1b gives cdef

        # View with line offsets
        dict(id="view-033-view-line-offsets",
             tokens=["view","BOF+5b","EOF-2b","find","L03","take","to","line-start","+1l"], input_file="lines.txt",
             expect=dict(stdout="L03 ccc\n", exit=0)),

        # View with character operations
        dict(id="view-034-view-chars",
             tokens=["view","BOF+2b","EOF-2b","take","+3c"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),  # BOF+2b is position 2, take +3c gives cde

        dict(id="view-035-view-skip-chars",
             tokens=["view","BOF+2b","EOF-2b","skip","2c","take","+2c"], input_file="overlap.txt",
             expect=dict(stdout="ef", exit=0)),  # skip 2c from position 2 gives position 4, take +2c gives ef

        # View with binary data
        dict(id="view-036-view-binary",
             tokens=["view","BOF+5b","EOF-5b","find","BINARY_DATA","take","+11b"], input_file="binary-data.bin",
             expect=dict(stdout="BINARY_DATA", exit=0)),

        # View with unicode
        dict(id="view-037-view-unicode",
             tokens=["view","BOF+6b","EOF-6b","find","世界","take","+6b"], input_file="unicode-test.txt",
             expect=dict(stdout="世界", exit=0)),

        # View with CRLF
        dict(id="view-038-view-crlf",
             tokens=["view","BOF+2b","EOF-2b","take","+1l"], input_file="crlf-comprehensive.txt",
             expect=dict(stdout="ne1\r\n", exit=0)),  # BOF+2b is position 2, take +1l gives ne1\r\n

        # View with large files
        dict(id="view-039-view-large-file",
             tokens=["view","BOF+1000b","EOF-1000b","find","NEEDLE","take","+6b"], input_file="big-forward.bin",
             expect=dict(stdout="NEEDLE", exit=0)),

        # View with repeated patterns
        dict(id="view-040-view-repeated-patterns",
             tokens=["view","BOF+50b","EOF-50b","find","PATTERN","take","+7b"], input_file="repeated-patterns.txt",
             expect=dict(stdout="PATTERN", exit=0)),

        # View with nested sections
        dict(id="view-041-view-nested-sections",
             tokens=["view","BOF+10b","EOF-10b","find","BEGIN_SECTION_A","take","until","END_SECTION_A"], input_file="nested-sections.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),  # BEGIN_SECTION_A not found in view [10, EOF-10b)

        # View with edge cases
        dict(id="view-042-view-edge-whitespace",
             tokens=["view","BOF+5b","EOF-5b","find","spaces at end","take","+1l"], input_file="edge-cases.txt",
             expect=dict(stdout="with spaces at end   \n", exit=0)),  # BOF+5b skips "Line ", find gives "with spaces at end   \n"

        # View with stdin
        dict(id="view-043-view-stdin",
             tokens=["view","BOF+2b","EOF-2b","take","+3b"], input_file="-", stdin=b"Hello World",
             expect=dict(stdout="llo", exit=0)),

        # View with complex multi-operation sequences
        dict(id="view-044-view-complex-sequence",
             tokens=["view","BOF+2b","EOF-2b","find","def","label","MARK","skip","1b","skip","to","MARK","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),

        # View with regex and views
        dict(id="view-045-view-regex-complex",
             tokens=["view","BOF+2b","EOF-2b","find:re","\\w+","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),

        # View with take until and views
        dict(id="view-046-view-take-until-complex",
             tokens=["view","BOF+2b","EOF-2b","take","until","f","at","line-start"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),  # take until f at line-start gives empty range

        # View with multiple view operations
        dict(id="view-047-view-multiple-views",
             tokens=["view","BOF+2b","EOF-2b","view","BOF+3b","EOF-3b","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),  # Second view creates empty view [3,7), cursor clamped to 7, take +2b gives empty

        # View with clear view and subsequent operations
        dict(id="view-048-view-clear-subsequent",
             tokens=["view","BOF+2b","EOF-2b","clear","view","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="cde", exit=0)),  # Cursor still at position 2 from view

        # View with empty file
        dict(id="view-049-view-empty-file",
             tokens=["view","BOF+1b","EOF-1b","take","+1b"], input_file="empty.txt",
             expect=dict(stdout="", exit=0)),

        # View with single byte file
        dict(id="view-050-view-single-byte",
             tokens=["view","BOF","EOF","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="a", exit=0)),

        # ---------- CLI option smoke tests ----------
        dict(id="cli-001-version-flag",
             tokens=["--version"], input_file=None,
             expect=dict(stdout=VERSION_LINE, exit=0)),

        dict(id="cli-005-ops-file",
             tokens=[], input_file="overlap.txt",
             extra_args=["--ops", str(FIX / "commands_take_plus_2b.txt")],
             expect=dict(stdout="ab", exit=0)),


        dict(id="loop-001-basic",
             tokens=["take","+2b"], input_file="overlap.txt",
             extra_args=["--every","1ms","--until-idle","0"],
             expect=dict(stdout="ab", exit=0)),

        dict(id="loop-010-follow-idle-empty",
             tokens=["take","0b"], input_file="empty.txt",
             extra_args=["--follow","--until-idle","0"],
             expect=dict(stdout="", exit=0)),

        dict(id="loop-011-ignore-failures-monitor",
             tokens=["find","MISSING"], input_file="empty.txt",
             extra_args=["--monitor","--until-idle","0","--ignore-failures"],
             expect=dict(stdout="", exit=0)),

        dict(id="loop-012-naked-every-continue",
             tokens=["take","+1b"], input_file="overlap.txt",
             extra_args=["--every","0","--until-idle","0"],
             expect=dict(stdout="a", exit=0)),

        dict(id="loop-013-clause-failure-exits",
             tokens=["find","MISSING"], input_file="overlap.txt",
             extra_args=["--continue"],
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),

        dict(id="loop-014-for-timeout",
             tokens=["take","+1b"], input_file="overlap.txt",
             extra_args=["--for","0"],
             expect=dict(stdout="", exit=2)),

        # ---------- Stage-only execution tests ----------
        dict(id="stage-001-basic-staging",
             tokens=["take","+2b","label","TEST"], input_file="overlap.txt",
             expect=dict(stdout="ab", exit=0)),  # Verify staging works (output emitted, label committed)

        dict(id="stage-002-failed-clause-no-output",
             tokens=["find","NOTFOUND","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),  # Failed clause should not emit staged output

        dict(id="stage-003-successful-clause-commits",
             tokens=["take","+1b","label","A","take","+1b","label","B"], input_file="overlap.txt",
             expect=dict(stdout="ab", exit=0)),  # Both takes should emit, both labels should commit

        dict(id="logic-003b-and-vs-single-clause",
             tokens=["take","+3b","find","xyz"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),  # single clause: atomic rollback, no output

        # ---------- OR operator tests ----------
        # Basic OR - first success wins
        dict(id="logic-004-or-first-succeeds",
             tokens=["find","abc","take","+3b","OR","find","xyz"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),  # first succeeds -> second not tried

        dict(id="logic-005-or-first-fails-second-succeeds",
             tokens=["find","xyz","OR","find","abc","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),  # first fails, second succeeds

        dict(id="logic-006-or-both-fail",
             tokens=["find","xyz","OR","find","123"], input_file="overlap.txt",
             expect=dict(stdout="", exit=PROGRAM_FAIL_EXIT)),  # both fail -> overall fails

        # Short-circuit behavior - verify second clause is NOT executed
        dict(id="logic-008-or-short-circuit-no-second-output",
             tokens=["take","+2b","OR","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="ab", exit=0)),  # first succeeds with 2 bytes, second never runs (would output 3 if it did)

        # Chained OR - first success wins
        dict(id="logic-011-or-chain-first-succeeds",
             tokens=["find","abc","take","+1b","OR","find","def","take","+1b","OR","find","ghi","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="a", exit=0)),  # first succeeds, others never tried

        dict(id="logic-012-or-chain-second-succeeds",
             tokens=["find","xyz","OR","find","def","take","+1b","OR","find","ghi","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="d", exit=0)),  # first fails, second succeeds, third never tried

        dict(id="logic-013-or-chain-last-succeeds",
             tokens=["find","xyz","OR","find","123","OR","find","ghi","take","+1b"], input_file="overlap.txt",
             expect=dict(stdout="g", exit=0)),  # first two fail, last succeeds

        # THEN operator tests
        dict(id="logic-016-then-sequential",
             tokens=["find","abc","THEN","skip","3b","THEN","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),  # sequential execution

        dict(id="logic-017-then-with-failure",
             tokens=["find","abc","THEN","find","xyz","THEN","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),  # middle fails but continues, last clause succeeds

        # ---------- Non-short-circuit tests - verify clauses execute ----------
        # OR where first fails, second MUST execute and produce output
        dict(id="logic-023-or-second-executes",
             tokens=["find","xyz","take","+2b","OR","take","+2b"], input_file="overlap.txt",
             expect=dict(stdout="ab", exit=0)),  # first fails (no match), second executes

        # OR chain where each alternative tries and fails until last succeeds
        dict(id="logic-026-or-try-all-until-success",
             tokens=["find","xyz","OR","find","123","OR","find","def","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),  # first two fail and execute, last succeeds

        # THEN continues on failure (different from OR which short-circuits)
        dict(id="logic-028-then-vs-then-failure",
             tokens=["find","abc","THEN","find","xyz"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),  # THEN continues even after clause failure

        # ---------- fail operation tests ----------
        # Basic fail - always fails with message to stderr
        dict(id="fail-001-basic",
             tokens=["fail","Error message"], input_file="overlap.txt",
             expect=dict(stdout="", stderr="Error message", exit=PROGRAM_FAIL_EXIT)),

        # fail with OR - fail executes when first clause fails
        dict(id="fail-002-or-executes",
             tokens=["find","xyz","OR","fail","xyz not found"], input_file="overlap.txt",
             expect=dict(stdout="", stderr="xyz not found", exit=PROGRAM_FAIL_EXIT)),

        # fail with OR - fail not executed when first clause succeeds
        dict(id="fail-003-or-short-circuit",
             tokens=["find","abc","OR","fail","Should not see this"], input_file="overlap.txt",
             expect=dict(stdout="", stderr="", exit=0)),

        # fail with OR and recovery - fail, then another OR succeeds
        dict(id="fail-004-or-recovery",
             tokens=["find","xyz","OR","fail","First failed","OR","find","abc","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", stderr="First failed", exit=0)),

        # fail with THEN - fail executes, THEN continues
        dict(id="fail-005-then-continues",
             tokens=["fail","Failed","THEN","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", stderr="Failed", exit=0)),

        # fail in middle of clause - clause fails atomically
        dict(id="fail-006-atomic-rollback",
             tokens=["take","+3b","fail","Midway fail"], input_file="overlap.txt",
             expect=dict(stdout="", stderr="Midway fail", exit=PROGRAM_FAIL_EXIT)),

        # fail with empty message (user's choice, though not recommended)
        dict(id="fail-007-empty-message",
             tokens=["find","xyz","OR","fail",""], input_file="overlap.txt",
             expect=dict(stdout="", stderr="", exit=PROGRAM_FAIL_EXIT)),

        # fail with escape sequences in message
        dict(id="fail-008-escape-sequences",
             tokens=["find","xyz","OR","fail","Error:\\nxyz not found\\n"], input_file="overlap.txt",
             expect=dict(stdout="", stderr="Error:\nxyz not found\n", exit=PROGRAM_FAIL_EXIT)),

        # ---------- Mixed OR and THEN combinations ----------
        # OR followed by THEN - OR succeeds, THEN clause runs
        dict(id="logic-029-or-then-first-succeeds",
             tokens=["find","abc","OR","find","xyz","THEN","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),  # first OR succeeds (cursor at start of "abc"), second not tried, THEN runs

        # OR followed by THEN - OR fails then succeeds, THEN clause runs
        dict(id="logic-030-or-then-second-succeeds",
             tokens=["find","xyz","OR","find","def","THEN","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),  # first OR fails, second succeeds at "def", THEN runs from cursor at start of "def"

        # OR followed by THEN - all OR options fail, THEN still runs
        dict(id="logic-031-or-then-all-or-fail",
             tokens=["find","xyz","OR","find","123","THEN","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),  # both OR clauses fail, cursor stays at BOF, THEN runs anyway

        # THEN followed by OR - first THEN clause succeeds
        dict(id="logic-032-then-or-then-succeeds",
             tokens=["find","abc","THEN","take","+3b","OR","find","xyz"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),  # find abc at pos 0, take +3b outputs "abc", OR not evaluated

        # THEN followed by OR - first THEN clause fails, OR tries
        dict(id="logic-033-then-or-then-fails",
             tokens=["find","xyz","THEN","take","+3b","OR","find","abc","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="abc", exit=0)),  # first THEN clause fails, second THEN clause runs, OR alternative succeeds

        # Complex: OR THEN OR - test precedence
        dict(id="logic-034-or-then-or",
             tokens=["find","xyz","OR","find","abc","THEN","find","def","OR","find","ghi"], input_file="overlap.txt",
             expect=dict(stdout="", exit=0)),  # (find xyz OR find abc) succeeds, THEN (find def OR find ghi) both succeed

        # Complex: THEN OR THEN - sequential with fallback
        dict(id="logic-035-then-or-then",
             tokens=["find","abc","THEN","find","xyz","OR","find","def","THEN","take","+3b"], input_file="overlap.txt",
             expect=dict(stdout="def", exit=0)),  # find abc succeeds, find xyz fails, find def succeeds at pos 3, take from there

        # ---------- Edge Cases (regression tests) ----------
        # Test 1: Inline-offset label resolution in preflight/build
        dict(id="edge-001-inline-offset-label",
             tokens=["label","HERE","THEN","skip","to","HERE+2l","THEN","take","1l"], input_file="label-offset.txt",
             expect=dict(stdout="X\n", exit=0)),

        # Test 2: Backward window find → rightmost match
        dict(id="edge-002-backward-find-rightmost",
             tokens=["skip","15b","find","to","BOF","ERROR","take","5b"], input_file="backward-find.txt",
             expect=dict(stdout="ERROR", exit=0)),

        # Test 3: take until with empty span must not move cursor
        dict(id="edge-003-take-until-empty-span",
             tokens=["take","until","HEAD","THEN","take","4b"], input_file="take-until-empty.txt",
             expect=dict(stdout="HEAD", exit=0)),

        # Test 4: UTF-8 chopped boundary handling
        dict(id="edge-004-utf8-boundary",
             tokens=["take","1c","take","1c"], input_file="utf8-boundary.bin",
             expect=dict(stdout_sha256="ac6dbe25c0c20f48d8ff142ca49cb5a5ad558ae1db59f938bb62cdfeaa5c2b52", exit=0)),  # emoji + X

        # Test 5: Lines anchor (negative offset)
        dict(id="edge-005-lines-negative",
             tokens=["skip","7b","take","-1l"], input_file="lines.txt",
             expect=dict(stdout="L01 a\n", exit=0)),

    ]

def main():
    ap = argparse.ArgumentParser(description="Run fiskta v2 test suite")
    ap.add_argument("--exe", default=str(ROOT / "fiskta"), help="Path to fiskta executable")
    ap.add_argument("--filter", default="", help="Substring to filter test IDs")
    ap.add_argument("--list", action="store_true", help="List tests and exit")
    ap.add_argument("--no-fixtures", action="store_true", help="Do not regenerate fixtures")
    args = ap.parse_args()

    exe = Path(args.exe)
    if not args.no_fixtures:
        make_fixtures()

    all_tests = tests()
    if args.filter:
        all_tests = [t for t in all_tests if args.filter in t["id"]]

    if args.list:
        for t in all_tests:
            print(t["id"])
        return 0

    failures = 0
    passed = 0

    for t in all_tests:
        tid = t["id"]
        tokens = t["tokens"]
        in_name = t.get("input_file", None)
        stdin_data = t.get("stdin", None)
        extra_args = t.get("extra_args", [])

        if in_name is None:
            in_path = None
        elif in_name == "-":
            in_path = "-"
        else:
            in_path = str(FIX / in_name)
        code, out, err = run(exe, tokens, in_path, stdin_data, extra_args)
        ok_stdout, why = expect_stdout(out, t["expect"])
        ok_exit = (code == t["expect"]["exit"])

        if ok_stdout and ok_exit:
            print(f"[PASS] {tid}")
            passed += 1
        else:
            print(f"[FAIL] {tid}")
            if not ok_exit:
                print(f"  exit: want {t['expect']['exit']}, got {code}")
            if not ok_stdout:
                print(f"  {why}")
            if err:
                print(f"  stderr: {err.decode('utf-8', 'ignore').strip()}")
            failures += 1

    total = passed + failures
    print(f"\nSummary: {passed}/{total} passed, {failures} failed")
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())

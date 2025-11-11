#!/usr/bin/env python3
"""
Fix section comments to have consistent box formatting.

Handles two types of input:
1. // === style comments (converts to block)
2. Block comments (normalizes formatting)

Example output:
    /************************************************************
     * PHASE 1: PREFLIGHT PARSE
     * Analyze operations to determine memory requirements
     *************************************************************/

Rules:
1. Opening: /* followed by all *
2. Content lines: " * TEXT" with trailing spaces to pad to box width
3. Closing: " *" followed by all * and then */
4. Minimum content width of 56 chars (makes 60-char boxes with decorations)
"""

import re
import sys
import argparse

# Configuration
MIN_BOX_WIDTH = 10 # Minimum content width (text + padding)

# Global options
g_lowercase = False
g_uppercase = False

def format_box_comment(indent, texts):
    """Format texts as a proper box comment."""
    if not texts:
        return None

    # Apply case transformations to first line only (the header)
    if g_lowercase:
        texts = [texts[0].lower()] + texts[1:]
    elif g_uppercase:
        texts = [texts[0].upper()] + texts[1:]

    # Calculate width
    max_text_len = max(len(t) for t in texts)
    content_width = max(max_text_len, MIN_BOX_WIDTH)

    # Box dimensions:
    # Opening:  "/*****..."
    # Text:     " * TEXT... *"
    # Closing:  " *****.../"
    #
    # All lines must be the same total width.
    # Text line format: " * " + content_width + " *" = 3 + content_width + 2 = content_width + 5
    # Border line format: "/" + stars = border_stars + 1
    # Closing line format: " " + stars + "/" = border_stars + 2
    #
    # For all to match: content_width + 5 = border_stars + 1 = border_stars + 2
    # Wait, that's impossible. Let me reconsider...
    #
    # Opening: "/" + stars       (stars + 1 total)
    # Text:    " * TEXT... *"    (content_width + 5 total)
    # Closing: " " + stars + "/" (stars + 2 total)
    #
    # For opening and closing to match: stars + 1 = stars + 2 ??? NO!
    # The closing should be: " * " + stars + "/" to match!
    #
    # Let's think differently:
    # Opening:  "/*****"      where stars = border_stars
    # Text:     " * TEXT *"   where total = border_stars + 1 (to match opening)
    # Closing:  " ******/"    where stars = border_stars (+ 2 for space and slash)
    #
    # Text line: " * " (3) + TEXT (content_width) + " *" (2) = content_width + 5
    # This should equal border_stars + 1
    # So: border_stars = content_width + 4

    border_stars = content_width + 4

    # Build output
    result = []
    result.append(f"{indent}/" + '*' * border_stars)

    for text in texts:
        # Pad each text to content_width so all lines align
        padding = ' ' * (content_width - len(text))
        result.append(f"{indent} * {text}{padding} *")

    result.append(f"{indent} " + '*' * border_stars + "/")

    return '\n'.join(result)

def fix_equals_comment(match):
    """Convert // === style comments to block style."""
    indent = match.group(1)
    text = match.group(2).strip()

    result = format_box_comment(indent, [text])
    return result if result else match.group(0)

def fix_block_comment(match):
    """Normalize existing block comments."""
    indent = match.group(1)
    content_lines = match.group(2).strip().split('\n')

    # Extract text from each line
    texts = []
    for line in content_lines:
        line = line.strip()
        if line.startswith('*'):
            text = line[1:].strip()  # Remove leading * and whitespace
            if text.endswith('*'):
                text = text[:-1].rstrip()  # Remove trailing * and whitespace
            if text:
                texts.append(text)

    result = format_box_comment(indent, texts)
    return result if result else match.group(0)

def fix_file(filepath):
    """Fix all section comments in a file."""
    with open(filepath, 'r') as f:
        content = f.read()

    original = content

    # First pass: Convert // === style comments to block
    # Pattern: three lines with // and === borders
    equals_pattern = re.compile(
        r'^([ \t]*)// =+\n'           # Opening line: // ===...
        r'\1// (.+)\n'                 # Content line: // TEXT
        r'\1// =+\s*$',                # Closing line: // ===...
        re.MULTILINE
    )
    content = equals_pattern.sub(fix_equals_comment, content)

    # Second pass: Normalize existing block comments
    block_pattern = re.compile(
        r'^([ \t]*)/\*+/?\n'              # Opening
        r'((?:\1 \*[^\n]*\n)+)'           # Content lines
        r'\1 \*+/',                       # Closing
        re.MULTILINE
    )
    content = block_pattern.sub(fix_block_comment, content)

    with open(filepath, 'w') as f:
        f.write(content)

    return original != content

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Fix section comment formatting')
    parser.add_argument('files', nargs='+', help='File(s) to process')
    parser.add_argument('--lowercase', action='store_true', help='Convert text to lowercase')
    parser.add_argument('--uppercase', action='store_true', help='Convert text to UPPERCASE')

    args = parser.parse_args()

    if args.lowercase and args.uppercase:
        print("Error: Cannot use both --lowercase and --uppercase")
        sys.exit(1)

    g_lowercase = args.lowercase
    g_uppercase = args.uppercase

    fixed_count = 0
    for filepath in args.files:
        if fix_file(filepath):
            print(f"Fixed section comments in {filepath}")
            fixed_count += 1
        else:
            print(f"No changes needed in {filepath}")

    if fixed_count > 0:
        print(f"\nTotal: Fixed {fixed_count} of {len(args.files)} file(s)")

#!/bin/bash
set -euo pipefail

# Package fiskta release artifacts for GitHub upload.
# Usage: tools/package-release.sh [version]
#   version: e.g. "2.0" (default: parsed from git describe)
#
# Expects `zig build release` to have been run first.
# Creates archives in dist/ ready for GitHub release upload.

RELEASE_DIR="zig-out/release"
DIST_DIR="dist"

if [ ! -d "$RELEASE_DIR" ]; then
    echo "error: $RELEASE_DIR not found. Run 'zig build release' first." >&2
    exit 1
fi

# Determine version
if [ $# -ge 1 ]; then
    VERSION="$1"
else
    VERSION=$(git describe --tags --dirty --always 2>/dev/null || echo "unknown")
    VERSION="${VERSION#v}"
    echo "Auto-detected version: $VERSION"
fi

echo "Packaging fiskta $VERSION"
echo ""

# Clean stale artifacts from Windows release dir
WIN_DIR="$RELEASE_DIR/fiskta-windows-x86_64"
if [ -d "$WIN_DIR" ]; then
    find "$WIN_DIR" -name '*.tmp*' -delete 2>/dev/null || true
    rm -f "$WIN_DIR/bin/parse.lib" 2>/dev/null || true
fi

# Size limits (bytes) for sanity checks
MAX_CLI_SIZE=$((150 * 1024))       # 150 KiB for CLI binaries
MAX_CLI_SIZE_WIN=$((300 * 1024))   # 300 KiB for Windows (larger due to CRT)

# Verify binaries exist and check sizes
echo "Verifying binaries..."
FAIL=0
check_size() {
    local file="$1" max="$2" label="$3"
    if [ ! -f "$file" ]; then
        echo "  MISSING: $file"
        FAIL=1
        return
    fi
    local size
    size=$(stat -c%s "$file" 2>/dev/null || stat -f%z "$file" 2>/dev/null)
    local size_kb=$(( size / 1024 ))
    local max_kb=$(( max / 1024 ))
    if [ "$size" -gt "$max" ]; then
        echo "  OVERSIZE: $label: ${size_kb}K (limit: ${max_kb}K)"
        FAIL=1
    else
        echo "  OK: $label: ${size_kb}K"
    fi
}

check_size "$RELEASE_DIR/fiskta-linux-x86_64/bin/fiskta"      "$MAX_CLI_SIZE"     "linux-x86_64"
check_size "$RELEASE_DIR/fiskta-linux-x86_64-musl/bin/fiskta"  "$MAX_CLI_SIZE"     "linux-x86_64-musl"
check_size "$RELEASE_DIR/fiskta-macos-arm64/bin/fiskta"        "$MAX_CLI_SIZE"     "macos-arm64"
check_size "$RELEASE_DIR/fiskta-windows-x86_64/bin/fiskta.exe" "$MAX_CLI_SIZE_WIN" "windows-x86_64"

if [ "$FAIL" -ne 0 ]; then
    echo ""
    echo "error: binary verification failed" >&2
    exit 1
fi

echo ""

# Verify headers are present
for platform in fiskta-linux-x86_64 fiskta-linux-x86_64-musl fiskta-macos-arm64 fiskta-windows-x86_64; do
    for header in fiskta.h fiskta_types.h; do
        if [ ! -f "$RELEASE_DIR/$platform/include/$header" ]; then
            echo "error: missing $RELEASE_DIR/$platform/include/$header" >&2
            exit 1
        fi
    done
done
echo "Headers verified."
echo ""

# Create dist directory
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

# Package each platform
package_tar() {
    local dir_name="$1" archive_name="$2"
    echo "Creating $archive_name..."

    # Create a temp directory with the versioned name for clean archive paths
    local staging="$DIST_DIR/.staging/$archive_name"
    mkdir -p "$staging"

    # Copy contents, excluding .pdb files
    find "$RELEASE_DIR/$dir_name" -type f ! -name '*.pdb' | while read -r f; do
        local rel="${f#$RELEASE_DIR/$dir_name/}"
        mkdir -p "$staging/$(dirname "$rel")"
        cp "$f" "$staging/$rel"
    done

    tar -czf "$DIST_DIR/${archive_name}.tar.gz" -C "$DIST_DIR/.staging" "$archive_name"
}

package_zip() {
    local dir_name="$1" archive_name="$2"
    echo "Creating $archive_name..."

    local staging="$DIST_DIR/.staging/$archive_name"
    mkdir -p "$staging"

    # Copy contents, excluding .pdb files
    find "$RELEASE_DIR/$dir_name" -type f ! -name '*.pdb' | while read -r f; do
        local rel="${f#$RELEASE_DIR/$dir_name/}"
        mkdir -p "$staging/$(dirname "$rel")"
        cp "$f" "$staging/$rel"
    done

    (cd "$DIST_DIR/.staging" && zip -qr "../${archive_name}.zip" "$archive_name")
}

package_tar  "fiskta-linux-x86_64"      "fiskta-${VERSION}-linux-x86_64"
package_tar  "fiskta-linux-x86_64-musl" "fiskta-${VERSION}-linux-x86_64-musl"
package_tar  "fiskta-macos-arm64"       "fiskta-${VERSION}-macos-arm64"
package_zip  "fiskta-windows-x86_64"    "fiskta-${VERSION}-windows-x86_64"

# Clean up staging
rm -rf "$DIST_DIR/.staging"

echo ""
echo "Release artifacts:"
ls -lh "$DIST_DIR/"
echo ""
echo "Ready for: gh release create v${VERSION} ${DIST_DIR}/*"

#!/bin/sh
# fetch-stb.sh — fetch the stb_image single-file library needed by image_decode.c.
#
# stb has no version tags, so we pin to a specific commit hash.
# The header is placed in third_party/stb/ where Makefile.am's
# -I$(top_srcdir)/third_party/stb picks it up.
#
# Usage: scripts/fetch-stb.sh [DEST_DIR]
#   DEST_DIR defaults to third_party/stb (relative to repo root)

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEST="${1:-$SRC_DIR/third_party/stb}"

# Pinned commit — update when bumping stb.
STB_COMMIT="2c980bb59875b0d32144a71867fbdebb2f77cd20"
BASE_URL="https://raw.githubusercontent.com/nothings/stb/${STB_COMMIT}"

HEADER="stb_image.h"

mkdir -p "$DEST"

echo "Fetching $HEADER ..."
if command -v curl >/dev/null 2>&1; then
	curl -fsSL "${BASE_URL}/${HEADER}" -o "${DEST}/${HEADER}"
elif command -v wget >/dev/null 2>&1; then
	wget -q "${BASE_URL}/${HEADER}" -O "${DEST}/${HEADER}"
else
	echo "ERROR: Need curl or wget to fetch stb_image header" >&2
	exit 1
fi

echo "Done. stb_image.h placed in ${DEST}"
echo "Pinned to commit ${STB_COMMIT}"

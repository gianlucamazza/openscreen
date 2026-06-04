#!/usr/bin/env bash
# gen-wayland.sh — regenerate the vendored Wayland protocol glue used by the
# screen-mirror sender (screen_capturer.cc / screen_mirror_sender.cc).
#
# These *-client.h / *.c files are NOT hand-written: wayland-scanner emits them
# from protocol .xml. We vendor the output (rather than generating at build time)
# so the build does not hard-depend on wayland-scanner; this script is the
# documented, reproducible way to refresh them when a protocol or the scanner
# version changes. After running it, `git diff` here should be empty unless a
# source .xml or the scanner actually changed.
#
#   bash cast/standalone_sender/gen-wayland.sh
#
# Sources: three stable protocols ship with wayland-protocols (staging); the
# Hyprland one is vendored as XML next to this script (no system package).
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"

command -v wayland-scanner >/dev/null || {
	echo "wayland-scanner not found on PATH (install 'wayland')" >&2
	exit 1
}
echo "using $(wayland-scanner --version 2>&1)"

# wayland-protocols data dir (e.g. /usr/share/wayland-protocols); normalize any
# doubled slash from pkg-config, fall back to the conventional path.
PROTO_DIR="$(pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null | sed 's#//*#/#g')"
PROTO_DIR="${PROTO_DIR:-/usr/share/wayland-protocols}"

# Each entry: "<output stem>|<source .xml>". Quoted to keep the formatter from
# mangling the hyphenated stems.
PROTOCOLS=(
	"ext-foreign-toplevel-list-v1|$PROTO_DIR/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml"
	"ext-image-capture-source-v1|$PROTO_DIR/staging/ext-image-capture-source/ext-image-capture-source-v1.xml"
	"ext-image-copy-capture-v1|$PROTO_DIR/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml"
	"hyprland-toplevel-mapping-v1|$HERE/hyprland-toplevel-mapping-v1.xml"
)

for entry in "${PROTOCOLS[@]}"; do
	base="${entry%%|*}"
	xml="${entry#*|}"
	[[ -f "$xml" ]] || {
		echo "missing protocol xml for $base: $xml" >&2
		echo "(check your wayland-protocols version, or the vendored xml)" >&2
		exit 1
	}
	wayland-scanner client-header "$xml" "$HERE/$base-client.h"
	wayland-scanner private-code "$xml" "$HERE/$base.c"
	echo "generated $base (-client.h, .c) from $xml"
done

echo
echo "done. review with: git diff -- cast/standalone_sender/"

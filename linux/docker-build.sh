#!/bin/sh
set -eu

MODE=${1:-appimage}
VERSION=${2:-1.3.46-linux.1}
OUTPUT_DIR=${OUTPUT_DIR:-/out}

mkdir -p "$OUTPUT_DIR"

case "$MODE" in
	binary)
		make -C /src/linux BUILD_DIR="$OUTPUT_DIR" all
		printf 'Binary: %s/jpegview-linux\n' "$OUTPUT_DIR"
		;;
	appimage)
		BUILD_DIR="$OUTPUT_DIR" \
		APPIMAGETOOL_ARGS=--appimage-extract-and-run \
		sh /src/linux/package-appimage.sh "$VERSION"
		;;
	deb)
		if [ -z "${3:-}" ]; then
			echo "The deb mode requires the Ubuntu release (24 or 26)." >&2
			exit 2
		fi
		OUTPUT_DIR="$OUTPUT_DIR" sh /src/linux/package-deb.sh "$VERSION" "$3"
		;;
	*)
		echo "Usage: $0 [binary|appimage|deb] [version] [Ubuntu release for deb]" >&2
		exit 2
		;;
esac

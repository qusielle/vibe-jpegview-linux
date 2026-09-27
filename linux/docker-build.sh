#!/bin/sh
set -eu

MODE=${1:-appimage}
SOURCE_DIR=${JPEGVIEW_SOURCE_DIR:-/src}
VERSION=${2:-${JPEGVIEW_VERSION:-}}
if [ -z "$VERSION" ]; then
	if [ -f "$SOURCE_DIR/linux/version.sh" ]; then
		VERSION=$(cd "$SOURCE_DIR" && \
			GIT_OPTIONAL_LOCKS=0 \
			GIT_CONFIG_COUNT=1 \
			GIT_CONFIG_KEY_0=safe.directory \
			GIT_CONFIG_VALUE_0="$SOURCE_DIR" \
			sh ./linux/version.sh)
	else
		VERSION=0.0.0+unknown
	fi
fi
OUTPUT_DIR=${OUTPUT_DIR:-/out}

mkdir -p "$OUTPUT_DIR"

case "$MODE" in
	binary)
		make -C "$SOURCE_DIR/linux" BUILD_DIR="$OUTPUT_DIR" VERSION="$VERSION" all
		printf 'Binary: %s/jpegview-linux\n' "$OUTPUT_DIR"
		;;
	appimage)
		BUILD_DIR="$OUTPUT_DIR" \
		APPIMAGETOOL_ARGS=--appimage-extract-and-run \
		sh "$SOURCE_DIR/linux/package-appimage.sh" "$VERSION"
		;;
	deb)
		if [ -z "${3:-}" ]; then
			echo "The deb mode requires the Ubuntu release (24 or 26)." >&2
			exit 2
		fi
		OUTPUT_DIR="$OUTPUT_DIR" sh "$SOURCE_DIR/linux/package-deb.sh" "$VERSION" "$3"
		;;
	*)
		echo "Usage: $0 [binary|appimage|deb] [version] [Ubuntu release for deb]" >&2
		exit 2
		;;
esac

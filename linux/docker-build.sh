#!/bin/sh
set -eu

MODE=${1:-appimage}
SOURCE_DIR=${JPEGVIEW_SOURCE_DIR:-/src}
VERSION=${2:-${JPEGVIEW_VERSION:-}}
if [ -z "$VERSION" ]; then
	if [ -f "$SOURCE_DIR/linux/version.sh" ]; then
		if [ -e "$SOURCE_DIR/.git" ] && command -v git >/dev/null 2>&1; then
			git config --global --add safe.directory "$SOURCE_DIR"
		fi
		VERSION=$(cd "$SOURCE_DIR" && GIT_OPTIONAL_LOCKS=0 sh ./linux/version.sh)
	else
		VERSION=0.0.0+unknown
	fi
fi
OUTPUT_DIR=${OUTPUT_DIR:-/out}

mkdir -p "$OUTPUT_DIR"

case "$MODE" in
	binary)
		make -C "$SOURCE_DIR/linux" BUILD_DIR="$OUTPUT_DIR" VERSION="$VERSION" all
		if [ -n "${SEVENZIP_SOURCE_ROOT:-}" ]; then
			sevenzip_plugin="$OUTPUT_DIR/lib/jpegview-linux/7z.so"
			if [ ! -f "$sevenzip_plugin" ]; then
				echo "7-Zip support was requested but the Format7zF plugin is missing: $sevenzip_plugin" >&2
				exit 1
			fi
			sevenzip_source_archive=${SEVENZIP_SOURCE_ARCHIVE:-}
			if [ -z "$sevenzip_source_archive" ]; then
				sevenzip_source_archive="$(dirname -- "$SEVENZIP_SOURCE_ROOT")/7zip-24.09-source.tar.gz"
			fi
			sevenzip_docs="$OUTPUT_DIR/share/doc/jpegview-linux"
			mkdir -p "$sevenzip_docs"
			if [ ! -f "$sevenzip_source_archive" ]; then
				sevenzip_source_archive="$sevenzip_docs/7zip-24.09-source.tar.gz"
				tar -czf "$sevenzip_source_archive" -C "$(dirname -- "$SEVENZIP_SOURCE_ROOT")" \
					"$(basename -- "$SEVENZIP_SOURCE_ROOT")"
			fi
			cp "$SOURCE_DIR/linux/7zip-24.09-notice.txt" \
				"$sevenzip_docs/7zip-24.09-notice.txt"
			cp "$SEVENZIP_SOURCE_ROOT/DOC/License.txt" \
				"$sevenzip_docs/7zip-24.09-License.txt"
			cp "$SEVENZIP_SOURCE_ROOT/DOC/copying.txt" \
				"$sevenzip_docs/7zip-24.09-LGPL-2.1.txt"
			cp "$sevenzip_source_archive" "$sevenzip_docs/7zip-24.09-source.tar.gz"
		else
			echo "warning: building binary without SEVENZIP_SOURCE_ROOT; encrypted 7z support is unavailable" >&2
		fi
		if [ -n "${RAR_BACKEND_ROOT:-}" ]; then
			rar_plugin="$OUTPUT_DIR/lib/jpegview-linux/librar_backend.so"
			if [ ! -f "$rar_plugin" ]; then
				echo "RAR support was requested but the private reader plugin is missing: $rar_plugin" >&2
				exit 1
			fi
			rar_docs="$OUTPUT_DIR/share/doc/jpegview-linux"
			sh "$SOURCE_DIR/linux/package-rar-backend-source.sh" "$RAR_BACKEND_ROOT" \
				"${RAR_SOURCE_ARCHIVE:-}" "${RAR_LICENSE_FILE:-}" "$rar_docs"
		else
			echo "warning: building binary without RAR_BACKEND_ROOT; encrypted RAR support is unavailable" >&2
		fi
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

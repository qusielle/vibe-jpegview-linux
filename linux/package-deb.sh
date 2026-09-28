#!/bin/sh
set -eu

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
VERSION=${1:-}
if [ -z "$VERSION" ]; then
	VERSION=$(cd -- "$REPO_DIR" && sh "$SCRIPT_DIR/version.sh")
fi
UBUNTU_VERSION=${2:-}
OUTPUT_DIR=${OUTPUT_DIR:-/out}
SEVENZIP_SOURCE_ROOT=${SEVENZIP_SOURCE_ROOT:-}
SEVENZIP_SOURCE_ARCHIVE=${SEVENZIP_SOURCE_ARCHIVE:-}

case "$VERSION" in
	''|*[!A-Za-z0-9.+:~-]*)
		echo "Invalid Debian package version: $VERSION" >&2
		exit 2
		;;
esac
case "$VERSION" in
	[0-9]*) ;;
	*)
		echo "Debian package versions must start with a digit: $VERSION" >&2
		exit 2
		;;
esac

case "$UBUNTU_VERSION" in
	24|26) ;;
	*)
		echo "Only Ubuntu 24 and 26 .deb builds are supported." >&2
		exit 2
		;;
esac

mkdir -p "$OUTPUT_DIR"
WORK_DIR=$(mktemp -d "$OUTPUT_DIR/.jpegview-deb.XXXXXX")
trap 'rm -rf "$WORK_DIR"' EXIT

BUILD_DIR="$WORK_DIR/build"
PACKAGE_ROOT="$WORK_DIR/package"
BINARY="$BUILD_DIR/jpegview-linux"
OUTPUT=${OUTPUT:-$OUTPUT_DIR/jpegview-linux_${VERSION}_ubuntu${UBUNTU_VERSION}_amd64.deb}

make -C "$SCRIPT_DIR" BUILD_DIR="$BUILD_DIR" VERSION="$VERSION" \
	SEVENZIP_SOURCE_ROOT="$SEVENZIP_SOURCE_ROOT" all

SEVENZIP_PLUGIN=
if [ -n "$SEVENZIP_SOURCE_ROOT" ]; then
	SEVENZIP_PLUGIN="$BUILD_DIR/lib/jpegview-linux/7z.so"
	if [ ! -f "$SEVENZIP_PLUGIN" ]; then
		echo "7-Zip support was requested but the Format7zF plugin is missing: $SEVENZIP_PLUGIN" >&2
		exit 1
	fi
	if [ -z "$SEVENZIP_SOURCE_ARCHIVE" ]; then
		SEVENZIP_SOURCE_ARCHIVE="$(dirname -- "$SEVENZIP_SOURCE_ROOT")/7zip-24.09-source.tar.gz"
	fi
	if [ ! -f "$SEVENZIP_SOURCE_ARCHIVE" ]; then
		SEVENZIP_SOURCE_ARCHIVE="$WORK_DIR/7zip-24.09-source.tar.gz"
		tar -czf "$SEVENZIP_SOURCE_ARCHIVE" -C "$(dirname -- "$SEVENZIP_SOURCE_ROOT")" \
			"$(basename -- "$SEVENZIP_SOURCE_ROOT")"
	fi
else
	echo "warning: building .deb without SEVENZIP_SOURCE_ROOT; encrypted 7z support is unavailable" >&2
fi

if command -v patchelf >/dev/null 2>&1; then
	patchelf --remove-rpath "$BINARY"
fi

mkdir -p \
	"$PACKAGE_ROOT/DEBIAN" \
	"$WORK_DIR/meta/debian"

cat > "$WORK_DIR/meta/debian/control" <<'EOF'
Source: jpegview-linux
Section: graphics
Priority: optional
Maintainer: JPEGView Linux maintainers <qusielle@users.noreply.github.com>
Standards-Version: 4.6.2

Package: jpegview-linux
Architecture: amd64
Description: Fast, minimal native Linux image viewer
 JPEGView image viewing and processing on Linux.
EOF

if [ -n "$SEVENZIP_PLUGIN" ]; then
	SHLIBS_OUTPUT=$(cd "$WORK_DIR/meta" && dpkg-shlibdeps -O -e "$BINARY" -e "$SEVENZIP_PLUGIN")
else
	SHLIBS_OUTPUT=$(cd "$WORK_DIR/meta" && dpkg-shlibdeps -O -e "$BINARY")
fi
case "$SHLIBS_OUTPUT" in
	shlibs:Depends=*) DEPENDS=${SHLIBS_OUTPUT#shlibs:Depends=} ;;
	*)
		echo "Could not derive Debian runtime dependencies from $BINARY" >&2
		exit 1
		;;
esac

# These are loaded at runtime through dlopen and are not present in ELF NEEDED.
DEPENDS="$DEPENDS, libpangoft2-1.0-0, libfontconfig1"
# Ubuntu 24.04 and newer split HEVC support into optional libheif plugins.
DEPENDS="$DEPENDS, libheif-plugin-libde265, libheif-plugin-x265"

install -D -m 0755 "$BINARY" "$PACKAGE_ROOT/usr/bin/jpegview-linux"
install -D -m 0644 "$SCRIPT_DIR/jpegview.desktop" \
	"$PACKAGE_ROOT/usr/share/applications/jpegview-linux.desktop"
install -D -m 0644 "$REPO_DIR/LICENSE.txt" \
	"$PACKAGE_ROOT/usr/share/doc/jpegview-linux/copyright"
install -D -m 0644 "$REPO_DIR/COPYING.txt" \
	"$PACKAGE_ROOT/usr/share/doc/jpegview-linux/GPL-2.0.txt"

if [ -n "$SEVENZIP_PLUGIN" ]; then
	install -D -m 0644 "$SEVENZIP_PLUGIN" \
		"$PACKAGE_ROOT/usr/lib/jpegview-linux/7z.so"
	install -D -m 0644 "$SCRIPT_DIR/7zip-24.09-notice.txt" \
		"$PACKAGE_ROOT/usr/share/doc/jpegview-linux/7zip-24.09-notice.txt"
	install -D -m 0644 "$SEVENZIP_SOURCE_ROOT/DOC/License.txt" \
		"$PACKAGE_ROOT/usr/share/doc/jpegview-linux/7zip-24.09-License.txt"
	install -D -m 0644 "$SEVENZIP_SOURCE_ROOT/DOC/copying.txt" \
		"$PACKAGE_ROOT/usr/share/doc/jpegview-linux/7zip-24.09-LGPL-2.1.txt"
	install -D -m 0644 "$SEVENZIP_SOURCE_ARCHIVE" \
		"$PACKAGE_ROOT/usr/share/doc/jpegview-linux/7zip-24.09-source.tar.gz"
fi

ICON="$WORK_DIR/jpegview-linux.png"
"$BINARY" --export-app-icon "$ICON"
install -D -m 0644 "$ICON" \
	"$PACKAGE_ROOT/usr/share/icons/hicolor/64x64/apps/jpegview-linux.png"

cat > "$PACKAGE_ROOT/DEBIAN/control" <<EOF
Package: jpegview-linux
Version: $VERSION
Section: graphics
Priority: optional
Architecture: amd64
Maintainer: JPEGView Linux maintainers <qusielle@users.noreply.github.com>
Homepage: https://github.com/qusielle/vibe-jpegview-linux
Depends: $DEPENDS
Recommends: xclip | wl-clipboard, libjpeg-turbo-progs
Description: Fast, minimal native Linux image viewer
 JPEGView image viewing and processing on Linux.
EOF

mkdir -p "$(dirname -- "$OUTPUT")"
dpkg-deb --build --root-owner-group "$PACKAGE_ROOT" "$OUTPUT"
printf 'Created %s\n' "$OUTPUT"

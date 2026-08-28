#!/bin/sh
set -eu

CDPATH=
export CDPATH
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
VERSION=${1:-1.3.46-linux.1}
BUILD_DIR=${BUILD_DIR:-$SCRIPT_DIR/build}
APPDIR=${APPDIR:-$BUILD_DIR/JPEGView-Linux.AppDir}
OUTPUT=${OUTPUT:-$BUILD_DIR/JPEGView-Linux-${VERSION}-x86_64.AppImage}

make -C "$SCRIPT_DIR" BUILD_DIR="$BUILD_DIR" all
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp "$BUILD_DIR/jpegview-linux" "$APPDIR/usr/bin/jpegview-linux"
cp "$SCRIPT_DIR/AppRun" "$APPDIR/AppRun"
cp "$SCRIPT_DIR/jpegview.desktop" "$APPDIR/usr/share/applications/jpegview-linux.desktop"
cp "$SCRIPT_DIR/jpegview.desktop" "$APPDIR/jpegview-linux.desktop"
cp "$SCRIPT_DIR/jpegview-linux.svg" "$APPDIR/jpegview-linux.svg"
cp "$SCRIPT_DIR/jpegview-linux.svg" "$APPDIR/usr/share/icons/hicolor/256x256/apps/jpegview-linux.svg"
chmod +x "$APPDIR/AppRun"

copy_runtime_dependencies() {
	queue="$1"
	seen="|"
	while [ -n "$queue" ]; do
		current=${queue%% *}
		if [ "$queue" = "$current" ]; then queue=; else queue=${queue#* }; fi
		case "$seen" in *"|$current|"*) continue ;; esac
		seen="$seen$current|"
		[ -f "$current" ] || continue
		while IFS= read -r dependency; do
			[ -n "$dependency" ] || continue
			base=$(basename "$dependency")
			case "$base" in
				libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|ld-linux*.so.*|linux-vdso.so.*)
					continue
					;;
			esac
			if [ ! -e "$APPDIR/usr/lib/$base" ]; then
				cp -L "$dependency" "$APPDIR/usr/lib/$base"
			fi
			queue="$queue $dependency"
		done <<EOF
$(ldd "$current" 2>/dev/null | awk '$3 ~ /^\// {print $3} /^[[:space:]]*\// {print $1}')
EOF
		done
}

SDL2_LIBRARY=${SDL2_LIBRARY:-}
if [ -z "$SDL2_LIBRARY" ]; then
	SDL2_LIBRARY=$(ldd "$BUILD_DIR/jpegview-linux" | awk '/libSDL2-2\.0\.so/ {print $3; exit}')
fi
if [ -z "$SDL2_LIBRARY" ] || [ ! -f "$SDL2_LIBRARY" ]; then
	echo "Could not locate libSDL2. Set SDL2_LIBRARY=/path/to/libSDL2-2.0.so.0" >&2
	exit 1
fi
copy_runtime_dependencies "$BUILD_DIR/jpegview-linux"

# WebP is intentionally loaded at runtime so the normal build does not need
# WebP development headers. Bundle it when it is present on the build host.
WEBP_LIBRARY=${WEBP_LIBRARY:-}
if [ -z "$WEBP_LIBRARY" ]; then
	WEBP_LIBRARY=$(ldconfig -p 2>/dev/null | awk '/libwebp\.so/ {print $NF; exit}')
fi
if [ -n "$WEBP_LIBRARY" ] && [ -f "$WEBP_LIBRARY" ]; then
	cp -L "$WEBP_LIBRARY" "$APPDIR/usr/lib/$(basename "$WEBP_LIBRARY")"
	copy_runtime_dependencies "$WEBP_LIBRARY"
fi

if command -v patchelf >/dev/null 2>&1; then
	patchelf --set-rpath "\$ORIGIN/../lib" "$APPDIR/usr/bin/jpegview-linux"
else
	echo "warning: patchelf not found; AppImage may need LD_LIBRARY_PATH for bundled SDL2" >&2
fi

if [ -n "${APPIMAGETOOL:-}" ]; then
	APPIMAGE_TOOL=$APPIMAGETOOL
elif command -v appimagetool >/dev/null 2>&1; then
	APPIMAGE_TOOL=$(command -v appimagetool)
else
	APPIMAGE_TOOL=
fi

if [ -n "$APPIMAGE_TOOL" ]; then
	# Set APPIMAGETOOL_ARGS=--appimage-extract-and-run when FUSE is unavailable.
	if [ -n "${APPIMAGETOOL_ARGS:-}" ]; then
		ARCH=x86_64 "$APPIMAGE_TOOL" "$APPIMAGETOOL_ARGS" "$APPDIR" "$OUTPUT"
	else
		ARCH=x86_64 "$APPIMAGE_TOOL" "$APPDIR" "$OUTPUT"
	fi
	printf 'Created %s\n' "$OUTPUT"
	if command -v sha256sum >/dev/null 2>&1; then sha256sum "$OUTPUT"; fi
	exit 0
fi

echo "Prepared AppDir: $APPDIR"
echo "Install appimagetool, then run:"
echo "  ARCH=x86_64 appimagetool '$APPDIR' '$OUTPUT'"

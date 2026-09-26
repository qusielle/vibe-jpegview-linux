#!/bin/sh
set -eu

CDPATH=
export CDPATH
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
VERSION=${1:-}
if [ -z "$VERSION" ]; then
	VERSION=$(cd -- "$SCRIPT_DIR/.." && sh "$SCRIPT_DIR/version.sh")
fi
BUILD_DIR=${BUILD_DIR:-$SCRIPT_DIR/build}
APPDIR=${APPDIR:-$BUILD_DIR/JPEGView-Linux.AppDir}
OUTPUT=${OUTPUT:-$BUILD_DIR/JPEGView-Linux-${VERSION}-x86_64.AppImage}

make -C "$SCRIPT_DIR" BUILD_DIR="$BUILD_DIR" VERSION="$VERSION" all
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" \
	"$APPDIR/usr/share/icons/hicolor/64x64/apps" "$APPDIR/usr/share/jpegview"
cp "$BUILD_DIR/jpegview-linux" "$APPDIR/usr/bin/jpegview-linux"
cp "$SCRIPT_DIR/AppRun" "$APPDIR/AppRun"
cp "$SCRIPT_DIR/jpegview.desktop" "$APPDIR/usr/share/applications/jpegview-linux.desktop"
cp "$SCRIPT_DIR/jpegview.desktop" "$APPDIR/jpegview-linux.desktop"
"$BUILD_DIR/jpegview-linux" --export-app-icon "$APPDIR/jpegview-linux.png"
cp "$APPDIR/jpegview-linux.png" "$APPDIR/usr/share/icons/hicolor/64x64/apps/jpegview-linux.png"
cp "$SCRIPT_DIR/../src/JPEGView/res/JPEGView.ico" "$APPDIR/usr/share/jpegview/JPEGView.ico"
for desktop_file in "$APPDIR/jpegview-linux.desktop" \
	"$APPDIR/usr/share/applications/jpegview-linux.desktop"; do
	printf 'X-AppImage-Version=%s\n' "$VERSION" >> "$desktop_file"
done
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

# The system-font renderer is loaded on first use so processes that never draw
# UI text do not pay Fontconfig/Pango startup cost. Its dependency is therefore
# intentionally absent from the executable's ldd output and must be seeded into
# the AppImage dependency walk explicitly.
PANGOFT2_LIBRARY=${PANGOFT2_LIBRARY:-}
if [ -z "$PANGOFT2_LIBRARY" ]; then
	PANGOFT2_LIBRARY=$(ldconfig -p 2>/dev/null | awk '/libpangoft2-1\.0\.so\.0/ {print $NF; exit}')
fi
if [ -z "$PANGOFT2_LIBRARY" ] || [ ! -f "$PANGOFT2_LIBRARY" ]; then
	echo "Could not locate libpangoft2-1.0.so.0. Set PANGOFT2_LIBRARY=/path/to/libpangoft2-1.0.so.0" >&2
	exit 1
fi
cp -L "$PANGOFT2_LIBRARY" "$APPDIR/usr/lib/$(basename "$PANGOFT2_LIBRARY")"
copy_runtime_dependencies "$PANGOFT2_LIBRARY"

# Ubuntu releases with modular libheif packages keep the HEVC/AV1 codec
# plugins outside libheif.so. Copy them beside the bundled libraries and let
# AppRun point libheif at this private directory.
for plugin_directory in \
	/usr/lib/*/libheif/plugins \
	/usr/local/lib/libheif/plugins \
	/usr/lib/libheif/plugins; do
	[ -d "$plugin_directory" ] || continue
	mkdir -p "$APPDIR/usr/lib/libheif/plugins"
	for plugin in "$plugin_directory"/*.so; do
		[ -f "$plugin" ] || continue
		cp -L "$plugin" "$APPDIR/usr/lib/libheif/plugins/$(basename "$plugin")"
		copy_runtime_dependencies "$plugin"
	done
done

# Keep the Linux equivalents of Windows clipboard and lossless-JPEG helpers
# inside the AppImage when they are available in the build environment. They
# are selected through PATH by the frontend, so this also works on hosts that
# do not have the tools installed themselves.
for helper in xclip wl-copy wl-paste jpegtran; do
	helper_path=$(command -v "$helper" 2>/dev/null || true)
	if [ -n "$helper_path" ] && [ -f "$helper_path" ]; then
		cp -L "$helper_path" "$APPDIR/usr/bin/$helper"
		copy_runtime_dependencies "$helper_path"
	fi
done

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

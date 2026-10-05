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
OUTPUT=${OUTPUT:-$BUILD_DIR/JPEGView-${VERSION}-x86_64.AppImage}
SEVENZIP_SOURCE_ROOT=${SEVENZIP_SOURCE_ROOT:-}
SEVENZIP_SOURCE_ARCHIVE=${SEVENZIP_SOURCE_ARCHIVE:-}
RAR_BACKEND_ROOT=${RAR_BACKEND_ROOT:-}
RAR_SOURCE_ARCHIVE=${RAR_SOURCE_ARCHIVE:-}
RAR_LICENSE_FILE=${RAR_LICENSE_FILE:-}
APPIMAGE_UPDATE_INFORMATION=${APPIMAGE_UPDATE_INFORMATION:-}

make -C "$SCRIPT_DIR" BUILD_DIR="$BUILD_DIR" VERSION="$VERSION" \
	SEVENZIP_SOURCE_ROOT="$SEVENZIP_SOURCE_ROOT" RAR_BACKEND_ROOT="$RAR_BACKEND_ROOT" all
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" \
	"$APPDIR/usr/share/icons/hicolor/64x64/apps" "$APPDIR/usr/share/jpegview" \
	"$APPDIR/usr/share/metainfo"
cp "$BUILD_DIR/jpegview-linux" "$APPDIR/usr/bin/jpegview-linux"
cp "$SCRIPT_DIR/AppRun" "$APPDIR/AppRun"
cp "$SCRIPT_DIR/jpegview.desktop" "$APPDIR/usr/share/applications/jpegview-linux.desktop"
cp "$SCRIPT_DIR/jpegview.desktop" "$APPDIR/jpegview-linux.desktop"
"$BUILD_DIR/jpegview-linux" --export-app-icon "$APPDIR/jpegview-linux.png"
cp "$APPDIR/jpegview-linux.png" "$APPDIR/usr/share/icons/hicolor/64x64/apps/jpegview-linux.png"
cp "$SCRIPT_DIR/../src/JPEGView/res/JPEGView.ico" "$APPDIR/usr/share/jpegview/JPEGView.ico"
cp "$SCRIPT_DIR/jpegview-linux.appdata.xml" \
	"$APPDIR/usr/share/metainfo/io.github.qusielle.vibe-jpegview-linux.appdata.xml"
ln -s jpegview-linux.png "$APPDIR/.DirIcon"
for desktop_file in "$APPDIR/jpegview-linux.desktop" \
	"$APPDIR/usr/share/applications/jpegview-linux.desktop"; do
	printf 'X-AppImage-Version=%s\n' "$VERSION" >> "$desktop_file"
done
chmod +x "$APPDIR/AppRun"

if [ ! -x "$APPDIR/AppRun" ] || [ ! -L "$APPDIR/.DirIcon" ] || \
		[ ! -f "$APPDIR/.DirIcon" ] || \
		[ "$(readlink "$APPDIR/.DirIcon")" != jpegview-linux.png ]; then
	echo "AppDir is missing an executable AppRun or its required PNG .DirIcon" >&2
	exit 1
fi
if [ "$(file --brief --mime-type "$(readlink -f "$APPDIR/.DirIcon")")" != image/png ]; then
	echo "AppDir .DirIcon must resolve to a PNG image" >&2
	exit 1
fi
set -- "$APPDIR"/*.desktop
if [ "$#" -ne 1 ] || [ ! -f "$1" ]; then
	echo "AppDir must contain exactly one root .desktop file" >&2
	exit 1
fi
for desktop_key in Icon Categories; do
	key_count=$(grep -c "^${desktop_key}=" "$1" || true)
	if [ "$key_count" -ne 1 ]; then
		echo "AppDir desktop entry must contain $desktop_key exactly once" >&2
		exit 1
	fi
done

if command -v desktop-file-validate >/dev/null 2>&1; then
	desktop-file-validate "$APPDIR/jpegview-linux.desktop" \
		"$APPDIR/usr/share/applications/jpegview-linux.desktop"
elif [ -n "$APPIMAGE_UPDATE_INFORMATION" ]; then
	echo "desktop-file-validate is required for release AppImage packaging" >&2
	exit 1
else
	echo "warning: desktop-file-validate not found; skipping desktop-entry validation" >&2
fi
if command -v appstreamcli >/dev/null 2>&1; then
	appstreamcli validate-tree --no-net "$APPDIR"
elif [ -n "$APPIMAGE_UPDATE_INFORMATION" ]; then
	echo "appstreamcli is required for release AppImage packaging" >&2
	exit 1
else
	echo "warning: appstreamcli not found; skipping AppStream validation" >&2
fi

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
				libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|ld-linux*.so.*|linux-vdso.so.*|libstdc++.so.*|libgcc_s.so.*)
					# Host graphics drivers (notably Mesa's software renderer) load
					# into this process too. An older AppImage-bundled C++ runtime
					# can prevent those host drivers from loading on newer systems.
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
		SEVENZIP_SOURCE_ARCHIVE="$BUILD_DIR/7zip-24.09-source.tar.gz"
		tar -czf "$SEVENZIP_SOURCE_ARCHIVE" -C "$(dirname -- "$SEVENZIP_SOURCE_ROOT")" \
			"$(basename -- "$SEVENZIP_SOURCE_ROOT")"
	fi
	install -D -m 0644 "$SEVENZIP_PLUGIN" \
		"$APPDIR/usr/lib/jpegview-linux/7z.so"
	copy_runtime_dependencies "$APPDIR/usr/lib/jpegview-linux/7z.so"
	install -D -m 0644 "$SCRIPT_DIR/7zip-24.09-notice.txt" \
		"$APPDIR/usr/share/doc/jpegview-linux/7zip-24.09-notice.txt"
	install -D -m 0644 "$SEVENZIP_SOURCE_ROOT/DOC/License.txt" \
		"$APPDIR/usr/share/doc/jpegview-linux/7zip-24.09-License.txt"
	install -D -m 0644 "$SEVENZIP_SOURCE_ROOT/DOC/copying.txt" \
		"$APPDIR/usr/share/doc/jpegview-linux/7zip-24.09-LGPL-2.1.txt"
	install -D -m 0644 "$SEVENZIP_SOURCE_ARCHIVE" \
		"$APPDIR/usr/share/doc/jpegview-linux/7zip-24.09-source.tar.gz"
else
	echo "warning: building AppImage without SEVENZIP_SOURCE_ROOT; encrypted 7z support is unavailable" >&2
fi

if [ -n "$RAR_BACKEND_ROOT" ]; then
	RAR_PLUGIN="$BUILD_DIR/lib/jpegview-linux/librar_backend.so"
	if [ ! -f "$RAR_PLUGIN" ]; then
		echo "RAR support was requested but the private reader plugin is missing: $RAR_PLUGIN" >&2
		exit 1
	fi
	if [ -z "$RAR_SOURCE_ARCHIVE" ] || [ -z "$RAR_LICENSE_FILE" ]; then
		echo "RAR release packaging requires RAR_SOURCE_ARCHIVE and RAR_LICENSE_FILE" >&2
		exit 1
	fi
	install -D -m 0644 "$RAR_PLUGIN" \
		"$APPDIR/usr/lib/jpegview-linux/librar_backend.so"
	copy_runtime_dependencies "$APPDIR/usr/lib/jpegview-linux/librar_backend.so"
	sh "$SCRIPT_DIR/package-rar-backend-source.sh" "$RAR_BACKEND_ROOT" \
		"$RAR_SOURCE_ARCHIVE" "$RAR_LICENSE_FILE" \
		"$APPDIR/usr/share/doc/jpegview-linux"
else
	echo "warning: building AppImage without RAR_BACKEND_ROOT; encrypted RAR support is unavailable" >&2
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
	set --
	if [ -n "${APPIMAGETOOL_ARGS:-}" ]; then
		set -- "$@" "$APPIMAGETOOL_ARGS"
	fi
	if [ -n "$APPIMAGE_UPDATE_INFORMATION" ]; then
		set -- "$@" -u "$APPIMAGE_UPDATE_INFORMATION"
	fi
	# zsyncmake writes its sidecar in the working directory, beside this output.
	output_directory=$(dirname -- "$OUTPUT")
	output_filename=$(basename -- "$OUTPUT")
	mkdir -p -- "$output_directory"
	output_directory=$(cd -- "$output_directory" && pwd)
	appdir_absolute=$(cd -- "$APPDIR" && pwd)
	set -- "$@" "$appdir_absolute" "$output_filename"
	(
		cd -- "$output_directory"
		ARCH=x86_64 "$APPIMAGE_TOOL" "$@"
	)
	if [ -n "$APPIMAGE_UPDATE_INFORMATION" ] && [ ! -f "$OUTPUT.zsync" ]; then
		echo "appimagetool did not create the requested update file: $OUTPUT.zsync" >&2
		exit 1
	fi
	printf 'Created %s\n' "$OUTPUT"
	if command -v sha256sum >/dev/null 2>&1; then sha256sum "$OUTPUT"; fi
	exit 0
fi

if [ -n "$APPIMAGE_UPDATE_INFORMATION" ]; then
	echo "APPIMAGE_UPDATE_INFORMATION requires appimagetool to create an updateable AppImage" >&2
	exit 1
fi

echo "Prepared AppDir: $APPDIR"
echo "Install appimagetool, then run:"
echo "  ARCH=x86_64 appimagetool '$APPDIR' '$OUTPUT'"

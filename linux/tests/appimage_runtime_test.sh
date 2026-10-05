#!/bin/sh
set -eu

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

MOCK_BIN="$TEMP_DIR/bin"
BUILD_DIR="$TEMP_DIR/build"
APPDIR="$TEMP_DIR/appdir"
LIB_DIR="$TEMP_DIR/libs"
mkdir -p "$MOCK_BIN" "$BUILD_DIR" "$LIB_DIR"

cat > "$MOCK_BIN/make" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$MOCK_BIN/patchelf" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$MOCK_BIN/appimagetool" <<'EOF'
#!/bin/sh
printf 'ARG<%s>\n' "$@" > "$APPIMAGE_TEST_TOOL_LOG"
output=
has_update_information=0
for argument in "$@"; do
	output=$argument
	if [ "$argument" = -u ]; then has_update_information=1; fi
done
: > "$output"
if [ "$has_update_information" -eq 1 ] && [ "${APPIMAGE_TEST_OMIT_ZSYNC:-0}" -ne 1 ]; then
	# appimagetool runs zsyncmake with the AppImage filename, and zsyncmake
	# writes its sidecar into the current working directory.
	: > "$(basename -- "$output").zsync"
fi
exit 0
EOF
cat > "$MOCK_BIN/ldd" <<'EOF'
#!/bin/sh
if [ "${1##*/}" = jpegview-linux ]; then
	printf ' libjpegview-test.so.1 => %s/libjpegview-test.so.1 (0x1)\n' "$APPIMAGE_TEST_LIB_DIR"
	printf ' libstdc++.so.6 => %s/libstdc++.so.6 (0x2)\n' "$APPIMAGE_TEST_LIB_DIR"
	printf ' libgcc_s.so.1 => %s/libgcc_s.so.1 (0x3)\n' "$APPIMAGE_TEST_LIB_DIR"
fi
EOF
chmod 755 "$MOCK_BIN/make" "$MOCK_BIN/patchelf" "$MOCK_BIN/appimagetool" "$MOCK_BIN/ldd"

cat > "$MOCK_BIN/desktop-file-validate" <<'EOF'
#!/bin/sh
test "$#" -eq 2
test -f "$1"
test -f "$2"
printf 'desktop-file-validate\n' >> "${APPIMAGE_TEST_VALIDATION_LOG:-/dev/null}"
EOF
cat > "$MOCK_BIN/appstreamcli" <<'EOF'
#!/bin/sh
test "$#" -eq 3
test "$1" = validate-tree
test "$2" = --no-net
test -d "$3"
test -f "$3/usr/share/metainfo/io.github.qusielle.vibe-jpegview-linux.appdata.xml"
printf 'appstreamcli\n' >> "${APPIMAGE_TEST_VALIDATION_LOG:-/dev/null}"
EOF
chmod 755 "$MOCK_BIN/desktop-file-validate" "$MOCK_BIN/appstreamcli"

for library in libjpegview-test.so.1 libstdc++.so.6 libgcc_s.so.1 \
	libSDL2-2.0.so.0 libpangoft2-test.so.0; do
	: > "$LIB_DIR/$library"
done
cat > "$BUILD_DIR/jpegview-linux" <<'EOF'
#!/bin/sh
if [ "${1:-}" = --export-app-icon ]; then
	cp "$APPIMAGE_TEST_ICON" "$2"
elif [ "${1:-}" = --print-render-driver ]; then
	printf '%s\n' "${SDL_RENDER_DRIVER:-<unset>}"
fi
EOF
chmod 755 "$BUILD_DIR/jpegview-linux"

APPIMAGE_TEST_LIB_DIR="$LIB_DIR" \
APPIMAGE_TEST_ICON="$REPO_DIR/linux/screenshots/main-window-panels.png" \
APPIMAGE_TEST_TOOL_LOG="$TEMP_DIR/appimagetool-arguments" \
APPIMAGE_TEST_VALIDATION_LOG="$TEMP_DIR/validation-calls" \
PATH="$MOCK_BIN:/usr/bin:/bin" \
BUILD_DIR="$BUILD_DIR" \
APPDIR="$APPDIR" \
OUTPUT="$TEMP_DIR/test.AppImage" \
PANGOFT2_LIBRARY="$LIB_DIR/libpangoft2-test.so.0" \
SDL2_LIBRARY="$LIB_DIR/libSDL2-2.0.so.0" \
WEBP_LIBRARY=/nonexistent/libwebp.so \
APPIMAGETOOL="$MOCK_BIN/appimagetool" \
	sh "$REPO_DIR/linux/package-appimage.sh" 1.0.0 >/dev/null

if grep -Fqx 'ARG<-u>' "$TEMP_DIR/appimagetool-arguments"; then
	echo 'Default AppImage packaging unexpectedly embedded update information' >&2
	exit 1
fi

update_information='gh-releases-zsync|qusielle|vibe-jpegview-linux|latest|JPEGView-*-ubuntu20-x86_64.AppImage.zsync'
APPIMAGE_TEST_LIB_DIR="$LIB_DIR" \
APPIMAGE_TEST_ICON="$REPO_DIR/linux/screenshots/main-window-panels.png" \
APPIMAGE_TEST_TOOL_LOG="$TEMP_DIR/appimagetool-update-arguments" \
APPIMAGE_TEST_VALIDATION_LOG="$TEMP_DIR/validation-calls" \
PATH="$MOCK_BIN:/usr/bin:/bin" \
BUILD_DIR="$BUILD_DIR" \
APPDIR="$APPDIR" \
OUTPUT="$TEMP_DIR/test-update.AppImage" \
PANGOFT2_LIBRARY="$LIB_DIR/libpangoft2-test.so.0" \
SDL2_LIBRARY="$LIB_DIR/libSDL2-2.0.so.0" \
WEBP_LIBRARY=/nonexistent/libwebp.so \
APPIMAGETOOL_ARGS=--appimage-extract-and-run \
APPIMAGE_UPDATE_INFORMATION="$update_information" \
APPIMAGETOOL="$MOCK_BIN/appimagetool" \
	sh "$REPO_DIR/linux/package-appimage.sh" 1.0.0 >/dev/null

test -f "$TEMP_DIR/test-update.AppImage.zsync" || {
	echo 'AppImage update packaging did not retain the generated zsync file' >&2
	exit 1
}
test -f "$TEMP_DIR/test-update.AppImage" || {
	echo 'AppImage update packaging did not write the image to its requested output path' >&2
	exit 1
}
grep -Fqx 'ARG<--appimage-extract-and-run>' "$TEMP_DIR/appimagetool-update-arguments"
grep -Fqx 'ARG<-u>' "$TEMP_DIR/appimagetool-update-arguments"
grep -Fqx "ARG<$update_information>" "$TEMP_DIR/appimagetool-update-arguments"
grep -Fqx 'ARG<test-update.AppImage>' "$TEMP_DIR/appimagetool-update-arguments"

if APPIMAGE_TEST_LIB_DIR="$LIB_DIR" \
	APPIMAGE_TEST_ICON="$REPO_DIR/linux/screenshots/main-window-panels.png" \
	APPIMAGE_TEST_TOOL_LOG="$TEMP_DIR/appimagetool-missing-zsync-arguments" \
	APPIMAGE_TEST_OMIT_ZSYNC=1 \
	PATH="$MOCK_BIN:/usr/bin:/bin" \
	BUILD_DIR="$BUILD_DIR" \
	APPDIR="$APPDIR" \
	OUTPUT="$TEMP_DIR/test-missing-zsync.AppImage" \
	PANGOFT2_LIBRARY="$LIB_DIR/libpangoft2-test.so.0" \
	SDL2_LIBRARY="$LIB_DIR/libSDL2-2.0.so.0" \
	WEBP_LIBRARY=/nonexistent/libwebp.so \
	APPIMAGE_UPDATE_INFORMATION="$update_information" \
	APPIMAGETOOL="$MOCK_BIN/appimagetool" \
	sh "$REPO_DIR/linux/package-appimage.sh" 1.0.0 >/dev/null 2>&1; then
	echo 'AppImage packaging accepted update metadata without a generated zsync file' >&2
	exit 1
fi

test -f "$APPDIR/usr/lib/libjpegview-test.so.1" || {
	echo 'AppImage packaging stopped bundling an ordinary runtime dependency' >&2
	exit 1
}
test -L "$APPDIR/.DirIcon" || {
	echo 'AppImage AppDir is missing the required .DirIcon link' >&2
	exit 1
}
test -f "$APPDIR/.DirIcon" || {
	echo 'AppImage .DirIcon points to a missing icon image' >&2
	exit 1
}
test "$(readlink "$APPDIR/.DirIcon")" = jpegview-linux.png || {
	echo 'AppImage .DirIcon does not point to the root PNG icon' >&2
	exit 1
}
test "$(file --brief --mime-type "$(readlink -f "$APPDIR/.DirIcon")")" = image/png || {
	echo 'AppImage .DirIcon is not a PNG image' >&2
	exit 1
}
test -f "$APPDIR/usr/share/metainfo/io.github.qusielle.vibe-jpegview-linux.appdata.xml" || {
	echo 'AppImage AppDir is missing its AppStream metainfo' >&2
	exit 1
}
cmp -s "$REPO_DIR/linux/jpegview-linux.appdata.xml" \
	"$APPDIR/usr/share/metainfo/io.github.qusielle.vibe-jpegview-linux.appdata.xml" || {
	echo 'AppImage AppStream metainfo differs from the source metadata' >&2
	exit 1
}
test "$(find "$APPDIR" -maxdepth 1 -type f -name '*.desktop' | wc -l)" -eq 1 || {
	echo 'AppImage AppDir must contain exactly one root desktop entry' >&2
	exit 1
}
for key in Icon Categories; do
	count=$(grep -c "^${key}=" "$APPDIR/jpegview-linux.desktop" || true)
	test "$count" -eq 1 || {
		echo "AppImage root desktop entry must contain $key exactly once" >&2
		exit 1
	}
done
test -x "$APPDIR/AppRun" || {
	echo 'AppImage AppDir AppRun is not executable' >&2
	exit 1
}
test "$(sort -u "$TEMP_DIR/validation-calls" | tr '\n' ' ')" = \
	'appstreamcli desktop-file-validate ' || {
	echo 'AppImage packaging did not run both desktop and AppStream validators' >&2
	exit 1
}
for runtime in libstdc++.so.6 libgcc_s.so.1; do
	if [ -e "$APPDIR/usr/lib/$runtime" ]; then
		echo "AppImage unexpectedly bundled host runtime $runtime" >&2
		exit 1
	fi
done

mkdir -p "$TEMP_DIR/no-gl-drivers"
renderer=$(LIBGL_DRIVERS_PATH="$TEMP_DIR/no-gl-drivers" \
	"$APPDIR/AppRun" --print-render-driver)
has_accessible_render_device=0
for device in /dev/dri/card* /dev/dri/renderD* /dev/nvidia[0-9]* /dev/nvidiactl; do
	if [ -c "$device" ] && [ -r "$device" ] && [ -w "$device" ]; then
		has_accessible_render_device=1
		break
	fi
done
if [ "$has_accessible_render_device" -eq 0 ]; then
	test "$renderer" = software || {
		echo 'AppRun did not select SDL software rendering without a GL driver or device' >&2
		exit 1
	}
else
	test "$renderer" = '<unset>' || {
		echo 'AppRun changed renderer selection despite an accessible GPU device' >&2
		exit 1
	}
fi

renderer=$(SDL_RENDER_DRIVER=opengl LIBGL_DRIVERS_PATH="$TEMP_DIR/no-gl-drivers" \
	"$APPDIR/AppRun" --print-render-driver)
test "$renderer" = opengl || {
	echo 'AppRun overwrote an explicitly selected SDL renderer' >&2
	exit 1
}

echo 'AppImage host-runtime packaging tests passed.'

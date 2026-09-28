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
: > "$2"
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

for library in libjpegview-test.so.1 libstdc++.so.6 libgcc_s.so.1 \
	libSDL2-2.0.so.0 libpangoft2-test.so.0; do
	: > "$LIB_DIR/$library"
done
cat > "$BUILD_DIR/jpegview-linux" <<'EOF'
#!/bin/sh
if [ "${1:-}" = --export-app-icon ]; then
	cp "$APPIMAGE_TEST_ICON" "$2"
fi
EOF
chmod 755 "$BUILD_DIR/jpegview-linux"

APPIMAGE_TEST_LIB_DIR="$LIB_DIR" \
APPIMAGE_TEST_ICON="$REPO_DIR/src/JPEGView/res/JPEGView.ico" \
PATH="$MOCK_BIN:/usr/bin:/bin" \
BUILD_DIR="$BUILD_DIR" \
APPDIR="$APPDIR" \
OUTPUT="$TEMP_DIR/test.AppImage" \
PANGOFT2_LIBRARY="$LIB_DIR/libpangoft2-test.so.0" \
SDL2_LIBRARY="$LIB_DIR/libSDL2-2.0.so.0" \
WEBP_LIBRARY=/nonexistent/libwebp.so \
APPIMAGETOOL="$MOCK_BIN/appimagetool" \
	sh "$REPO_DIR/linux/package-appimage.sh" 1.0.0 >/dev/null

test -f "$APPDIR/usr/lib/libjpegview-test.so.1" || {
	echo 'AppImage packaging stopped bundling an ordinary runtime dependency' >&2
	exit 1
}
for runtime in libstdc++.so.6 libgcc_s.so.1; do
	if [ -e "$APPDIR/usr/lib/$runtime" ]; then
		echo "AppImage unexpectedly bundled host runtime $runtime" >&2
		exit 1
	fi
done

echo 'AppImage host-runtime packaging tests passed.'

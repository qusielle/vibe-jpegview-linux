#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

MOCK_BIN="$TEMP_DIR/bin"
OUTPUT_DIR="$TEMP_DIR/output"
mkdir -p "$MOCK_BIN" "$OUTPUT_DIR"

cat > "$MOCK_BIN/docker" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

output_dir=
entrypoint=
update_information=
requested_output=
mode=
for ((index = 1; index < $#; index++)); do
	case "${!index}" in
		-v|--volume)
			next=$((index + 1))
			volume=${!next}
			output_dir=${volume%:/out}
			;;
		--entrypoint)
			next=$((index + 1))
			entrypoint=${!next}
			;;
		--env)
			next=$((index + 1))
			environment_assignment=${!next}
			case "$environment_assignment" in
				APPIMAGE_UPDATE_INFORMATION=*) update_information=${environment_assignment#*=} ;;
				OUTPUT=*) requested_output=${environment_assignment#*=} ;;
			esac
			;;
		appimage|binary) mode=${!index} ;;
	esac
done

if [[ "$entrypoint" == /out/* ]]; then
	echo "JPEGView Linux $TEST_RELEASE_VERSION"
	exit 0
fi

if [[ -z "$output_dir" ]]; then
	echo 'release-assets test docker mock did not receive an /out volume' >&2
	exit 1
fi

case "$mode" in
	appimage)
		appimage="$output_dir/${requested_output#/out/}"
		printf 'mock AppImage\n' > "$appimage"
		printf '%s\n' "$update_information" > "$RELEASE_ASSETS_TEST_TEMP_DIR/update-information"
		printf '%s\n' "$requested_output" > "$RELEASE_ASSETS_TEST_TEMP_DIR/appimage-output"
		printf 'mock zsync\n' > "$appimage.zsync"
		;;
	binary)
		printf 'mock executable\n' > "$output_dir/jpegview-linux"
		mkdir -p "$output_dir/lib/jpegview-linux" \
			"$output_dir/share/doc/jpegview-linux"
		touch "$output_dir/lib/jpegview-linux/7z.so" \
			"$output_dir/lib/jpegview-linux/librar_backend.so"
		for file in \
			7zip-24.09-notice.txt 7zip-24.09-License.txt 7zip-24.09-LGPL-2.1.txt \
			7zip-24.09-source.tar.gz rar-backend-notice.txt \
			jpegview-rar-backend-source.tar.gz rars-afc60e4-source.tar.gz \
			rars-Apache-2.0.txt jpegview-rar-backend-GPL-2.0.txt; do
			touch "$output_dir/share/doc/jpegview-linux/$file"
		done
		;;
	*)
		echo "release-assets test docker mock received unknown mode: $mode" >&2
		exit 1
		;;
esac
EOF

cat > "$MOCK_BIN/gh" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$RELEASE_ASSETS_TEST_UPLOAD_LOG"
EOF
chmod 755 "$MOCK_BIN/docker" "$MOCK_BIN/gh"

TEST_RELEASE_VERSION=1.4.2 \
RELEASE_ASSETS_TEST_TEMP_DIR="$TEMP_DIR" \
RELEASE_ASSETS_TEST_UPLOAD_LOG="$TEMP_DIR/upload-arguments" \
RELEASE_TAG=1.4.2 \
UBUNTU_VERSION=20 \
APP_VERSION=1.4.2 \
OUTPUT_DIR="$OUTPUT_DIR" \
PATH="$MOCK_BIN:$PATH" \
	bash "$REPO_DIR/linux/release-assets.sh" >/dev/null

appimage_name='JPEGView-1.4.2-ubuntu20-x86_64.AppImage'
zsync_name="${appimage_name}.zsync"
expected_update_information='gh-releases-zsync|qusielle|vibe-jpegview-linux|latest|JPEGView-*-ubuntu20-x86_64.AppImage.zsync'
test "$(<"$TEMP_DIR/update-information")" = "$expected_update_information"
test "$(<"$TEMP_DIR/appimage-output")" = "/out/$appimage_name"
test -f "$OUTPUT_DIR/$appimage_name"
test -f "$OUTPUT_DIR/$zsync_name"
grep -Fqx "$OUTPUT_DIR/$appimage_name" "$TEMP_DIR/upload-arguments"
grep -Fqx "$OUTPUT_DIR/$zsync_name" "$TEMP_DIR/upload-arguments"
grep -Fq "  $appimage_name" "$OUTPUT_DIR/SHA256SUMS-ubuntu20.txt"
grep -Fq "  $zsync_name" "$OUTPUT_DIR/SHA256SUMS-ubuntu20.txt"

for ubuntu_version in 20 22 24 26; do
	if ! grep -Eq '(^|[[:space:]])zsync([[:space:]\\]|$)' \
		"$REPO_DIR/linux/Dockerfile.ubuntu${ubuntu_version}"; then
		echo "Ubuntu $ubuntu_version AppImage image does not install zsyncmake" >&2
		exit 1
	fi
done

echo 'Release AppImage update metadata and zsync upload tests passed.'

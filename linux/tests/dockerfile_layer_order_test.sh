#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd)
BUILD_HELPER_COPY='COPY linux/fetch-7zip-source.sh linux/install-rust-toolchain.sh linux/fetch-rars-source.sh linux/retry-command.sh /tmp/jpegview-build-scripts/'

line_for() {
	awk -v needle="$2" 'index($0, needle) { print NR; found = 1; exit } END { if (!found) exit 1 }' "$1"
}

assert_before() {
	file=$1
	first_pattern=$2
	second_pattern=$3
	if ! first_line=$(line_for "$file" "$first_pattern"); then
		echo "$file: missing Dockerfile instruction containing: $first_pattern" >&2
		exit 1
	fi
	if ! second_line=$(line_for "$file" "$second_pattern"); then
		echo "$file: missing Dockerfile instruction containing: $second_pattern" >&2
		exit 1
	fi
	if [ "$first_line" -ge "$second_line" ]; then
		echo "$file: expected '$first_pattern' before '$second_pattern'" >&2
		exit 1
	fi
}

for dockerfile in \
	Dockerfile.ubuntu20 \
	Dockerfile.ubuntu22 \
	Dockerfile.ubuntu24 \
	Dockerfile.ubuntu26 \
	Dockerfile.deb.ubuntu24 \
	Dockerfile.deb.ubuntu26; do
	file="$REPO_DIR/linux/$dockerfile"
	assert_before "$file" "$BUILD_HELPER_COPY" 'RUN sh /tmp/jpegview-build-scripts/fetch-7zip-source.sh'
	assert_before "$file" 'RUN sh /tmp/jpegview-build-scripts/fetch-7zip-source.sh' \
		'RUN sh /tmp/jpegview-build-scripts/install-rust-toolchain.sh'
	assert_before "$file" 'RUN sh /tmp/jpegview-build-scripts/install-rust-toolchain.sh' 'COPY . /src'

	case "$dockerfile" in
		Dockerfile.ubuntu20)
			appimagetool_download="RUN curl --fail --location --retry 8 --retry-max-time 90 \"\$APPIMAGETOOL_URL\""
			assert_before "$file" 'RUN apt-get' "$appimagetool_download"
			assert_before "$file" 'RUN sh /tmp/jpegview-build-scripts/install-rust-toolchain.sh' \
				"$appimagetool_download"
			assert_before "$file" "$appimagetool_download" 'COPY . /src'
			;;
		Dockerfile.ubuntu22|Dockerfile.ubuntu24|Dockerfile.ubuntu26)
			appimagetool_download="RUN curl --fail --location --retry 3 \"\$APPIMAGETOOL_URL\""
			assert_before "$file" 'RUN apt-get' "$appimagetool_download"
			assert_before "$file" 'RUN sh /tmp/jpegview-build-scripts/install-rust-toolchain.sh' \
				"$appimagetool_download"
			assert_before "$file" "$appimagetool_download" 'COPY . /src'
			;;
	esac
done

echo 'Dockerfile layer order tests passed.'

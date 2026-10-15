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

assert_contains() {
	file=$1
	pattern=$2
	if ! grep -Fq -- "$pattern" "$file"; then
		echo "$file: missing required Dockerfile content: $pattern" >&2
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

	case "$dockerfile" in
		Dockerfile.ubuntu20|Dockerfile.ubuntu22|Dockerfile.ubuntu24|Dockerfile.ubuntu26)
			assert_contains "$file" 'ARG APPIMAGETOOL_VERSION=1.9.1'
			assert_contains "$file" 'ARG APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0'
			assert_contains "$file" 'ARG APPIMAGETOOL_RUNTIME_VERSION=20251108'
			assert_contains "$file" 'ARG APPIMAGETOOL_RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d'
			assert_before "$file" "\"\$APPIMAGETOOL_SHA256\" /usr/local/bin/appimagetool | sha256sum --check" \
				"\"\$APPIMAGETOOL_RUNTIME_SHA256\" /usr/local/lib/appimagetool/runtime-x86_64 | sha256sum --check"
			assert_before "$file" 'APPIMAGETOOL_RUNTIME_FILE=/usr/local/lib/appimagetool/runtime-x86_64' 'COPY . /src'
			;;
	esac

	case "$dockerfile" in
		Dockerfile.ubuntu20|Dockerfile.ubuntu22)
			assert_contains "$file" 'ARG HIGHWAY_SHA256=5434488108186c170a5e2fca5e3c9b6ef59a1caa4d520b008a9b8be6b8abe6c5'
			assert_contains "$file" 'ARG LIBJXL_SHA256=3114bba1fabb36f6f4adc2632717209aa6f84077bc4e93b420e0d63fa0455c5e'
			assert_before "$file" '--output /tmp/jpegview-codecs/highway.tar.gz' \
				"\"\$HIGHWAY_SHA256\" /tmp/jpegview-codecs/highway.tar.gz | sha256sum --check"
			assert_before "$file" '--output /tmp/jpegview-codecs/libjxl.tar.gz' \
				"\"\$LIBJXL_SHA256\" /tmp/jpegview-codecs/libjxl.tar.gz | sha256sum --check"
			;;
	esac
done

echo 'Dockerfile layer order tests passed.'

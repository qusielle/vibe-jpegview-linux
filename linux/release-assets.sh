#!/usr/bin/env bash
set -euo pipefail

RELEASE_TAG=${RELEASE_TAG:?RELEASE_TAG is required}
UBUNTU_VERSION=${UBUNTU_VERSION:?UBUNTU_VERSION is required}
OUTPUT_DIR=${OUTPUT_DIR:-/out}

safe_version=${RELEASE_TAG//[^a-zA-Z0-9._+-]/-}
image="jpegview-linux-build:ubuntu${UBUNTU_VERSION}"
appimage_name="JPEGView-Linux-${safe_version}-ubuntu${UBUNTU_VERSION}-x86_64.AppImage"
binary_name="jpegview-linux-${safe_version}-ubuntu${UBUNTU_VERSION}-x86_64"
checksums_name="SHA256SUMS-ubuntu${UBUNTU_VERSION}.txt"
checksum_inputs=("$appimage_name" "$binary_name")
assets=("$OUTPUT_DIR/$appimage_name" "$OUTPUT_DIR/$binary_name")

mkdir -p "$OUTPUT_DIR"

docker run --rm -v "$OUTPUT_DIR:/out" "$image" appimage "$safe_version"
mv "$OUTPUT_DIR/JPEGView-Linux-${safe_version}-x86_64.AppImage" \
	"$OUTPUT_DIR/$appimage_name"

docker run --rm -v "$OUTPUT_DIR:/out" "$image" binary
mv "$OUTPUT_DIR/jpegview-linux" "$OUTPUT_DIR/$binary_name"

if [[ "$UBUNTU_VERSION" == 24 ]]; then
	deb_image=jpegview-linux-deb-build:ubuntu24
	deb_name="jpegview-linux_${safe_version}_ubuntu24_amd64.deb"
	deb_path="$OUTPUT_DIR/$deb_name"

	docker build \
		--file linux/Dockerfile.deb.ubuntu24 \
		--tag "$deb_image" \
		.
	docker run --rm -v "$OUTPUT_DIR:/out" "$deb_image" \
		deb "$safe_version" 24

	dpkg-deb --info "$deb_path" >/dev/null
	sudo apt-get install --yes --no-install-recommends "$deb_path"
	jpegview-linux --help >/dev/null

	checksum_inputs+=("$deb_name")
	assets+=("$deb_path")
fi

(
	cd "$OUTPUT_DIR"
	sha256sum "${checksum_inputs[@]}" > "$checksums_name"
)
assets+=("$OUTPUT_DIR/$checksums_name")

gh release upload "$RELEASE_TAG" "${assets[@]}" --clobber

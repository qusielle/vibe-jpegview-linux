#!/usr/bin/env bash
set -euo pipefail

RELEASE_TAG=${RELEASE_TAG:?RELEASE_TAG is required}
UBUNTU_VERSION=${UBUNTU_VERSION:?UBUNTU_VERSION is required}
OUTPUT_DIR=${OUTPUT_DIR:-/out}
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
APP_VERSION=${APP_VERSION:-$(cd -- "$REPO_DIR" && sh "$SCRIPT_DIR/version.sh")}

safe_version=${APP_VERSION//[^a-zA-Z0-9._+-]/-}
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

docker run --rm -v "$OUTPUT_DIR:/out" "$image" binary "$safe_version"
mv "$OUTPUT_DIR/jpegview-linux" "$OUTPUT_DIR/$binary_name"
binary_version=$(docker run --rm -v "$OUTPUT_DIR:/out" \
	--entrypoint "/out/$binary_name" "$image" --version)
test "$binary_version" = "JPEGView Linux $safe_version"

if [[ "$UBUNTU_VERSION" == 24 || "$UBUNTU_VERSION" == 26 ]]; then
	deb_image="jpegview-linux-deb-build:ubuntu${UBUNTU_VERSION}"
	deb_name="jpegview-linux_${safe_version}_ubuntu${UBUNTU_VERSION}_amd64.deb"
	deb_path="$OUTPUT_DIR/$deb_name"

	docker build \
		--file "linux/Dockerfile.deb.ubuntu${UBUNTU_VERSION}" \
		--tag "$deb_image" \
		.
	docker run --rm -v "$OUTPUT_DIR:/out" "$deb_image" \
		deb "$safe_version" "$UBUNTU_VERSION"

	dpkg-deb --info "$deb_path" >/dev/null
	test "$(dpkg-deb -f "$deb_path" Version)" = "$safe_version"
	docker run --rm \
		--env DEB_NAME="$deb_name" \
		--env APP_VERSION="$safe_version" \
		--volume "$OUTPUT_DIR:/out" \
		"ubuntu:${UBUNTU_VERSION}.04" \
		bash -euc '
			apt-get update
			apt-get install --yes --no-install-recommends "/out/$DEB_NAME"
			jpegview-linux --help >/dev/null
			test "$(jpegview-linux --version)" = "JPEGView Linux $APP_VERSION"
		'

	checksum_inputs+=("$deb_name")
	assets+=("$deb_path")
fi

(
	cd "$OUTPUT_DIR"
	sha256sum "${checksum_inputs[@]}" > "$checksums_name"
)
assets+=("$OUTPUT_DIR/$checksums_name")

gh release upload "$RELEASE_TAG" "${assets[@]}" --clobber

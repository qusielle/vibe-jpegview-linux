#!/bin/sh
set -eu

VERSION=24.09
SOURCE_URL="https://github.com/ip7z/7zip/archive/refs/tags/$VERSION.tar.gz"
SOURCE_SHA256=e4757b307925227724a2044651193664ba3d04e9ac13b8e631f1667896014bbf

DESTINATION=${1:-}
if [ "$#" -ne 1 ] || [ -z "$DESTINATION" ]; then
	echo "Usage: $0 /absolute/path/to/7zip-$VERSION" >&2
	exit 2
fi
case "$DESTINATION" in
	/*) ;;
	*) echo "7-Zip source destination must be an absolute path" >&2; exit 2 ;;
esac
if [ -e "$DESTINATION" ]; then
	echo "7-Zip source destination already exists: $DESTINATION" >&2
	exit 2
fi

DESTINATION_PARENT=$(dirname -- "$DESTINATION")
SOURCE_ARCHIVE="$DESTINATION_PARENT/7zip-$VERSION-source.tar.gz"
if [ -e "$SOURCE_ARCHIVE" ]; then
	echo "7-Zip source archive already exists: $SOURCE_ARCHIVE" >&2
	exit 2
fi
mkdir -p "$DESTINATION_PARENT"
TEMP_DIR=$(mktemp -d "$DESTINATION_PARENT/.7zip-$VERSION.XXXXXX")
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

curl --fail --location --retry 3 "$SOURCE_URL" --output "$TEMP_DIR/source.tar.gz"
printf '%s  %s\n' "$SOURCE_SHA256" "$TEMP_DIR/source.tar.gz" | sha256sum --check
tar -xzf "$TEMP_DIR/source.tar.gz" -C "$TEMP_DIR"
SOURCE_ROOT="$TEMP_DIR/7zip-$VERSION"
if [ ! -f "$SOURCE_ROOT/CPP/7zip/Bundles/Format7zF/makefile.gcc" ] ||
	[ ! -f "$SOURCE_ROOT/DOC/License.txt" ] ||
	[ ! -f "$SOURCE_ROOT/DOC/copying.txt" ]; then
	echo "Downloaded 7-Zip source archive has an unexpected layout" >&2
	exit 1
fi
mv "$TEMP_DIR/source.tar.gz" "$SOURCE_ARCHIVE"
mv "$SOURCE_ROOT" "$DESTINATION"
printf 'Installed verified 7-Zip %s source at %s\n' "$VERSION" "$DESTINATION"

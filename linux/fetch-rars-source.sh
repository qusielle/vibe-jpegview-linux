#!/bin/sh
set -eu

RARS_REV=afc60e4c669ba1fe6a18748b16b08164e69c5e44
RARS_URL=https://github.com/bitplane/rars.git

DESTINATION=${1:-}
if [ "$#" -ne 1 ] || [ -z "$DESTINATION" ]; then
	echo "Usage: $0 /absolute/path/prefix-for-rars-source-files" >&2
	exit 2
fi
case "$DESTINATION" in
	/*) ;;
	*) echo "RARS source destination must be an absolute path" >&2; exit 2 ;;
esac
SOURCE_ARCHIVE="$DESTINATION-source.tar.gz"
LICENSE_FILE="$DESTINATION-COPYING"
if [ -e "$SOURCE_ARCHIVE" ] || [ -e "$LICENSE_FILE" ]; then
	echo "RARS source output already exists for: $DESTINATION" >&2
	exit 2
fi

DESTINATION_PARENT=$(dirname -- "$DESTINATION")
mkdir -p "$DESTINATION_PARENT"
TEMP_DIR=$(mktemp -d "$DESTINATION_PARENT/.rars-source.XXXXXX")
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
git init -q "$TEMP_DIR/repository"
git -C "$TEMP_DIR/repository" remote add origin "$RARS_URL"
git -C "$TEMP_DIR/repository" fetch --quiet --depth 1 origin "$RARS_REV"
git -C "$TEMP_DIR/repository" checkout --quiet --detach FETCH_HEAD
ACTUAL_REV=$(git -C "$TEMP_DIR/repository" rev-parse HEAD)
if [ "$ACTUAL_REV" != "$RARS_REV" ] ||
	[ ! -f "$TEMP_DIR/repository/crates/rars/Cargo.toml" ] ||
	[ ! -f "$TEMP_DIR/repository/COPYING" ]; then
	echo "Fetched RARS source did not match the pinned revision or expected layout" >&2
	exit 1
fi
git -C "$TEMP_DIR/repository" archive --format=tar \
	--prefix="rars-${RARS_REV}/" "$RARS_REV" | gzip -n > "$TEMP_DIR/source.tar.gz"
cp "$TEMP_DIR/repository/COPYING" "$TEMP_DIR/COPYING"
mv "$TEMP_DIR/source.tar.gz" "$SOURCE_ARCHIVE"
mv "$TEMP_DIR/COPYING" "$LICENSE_FILE"
printf 'Prepared RARS source %s at %s\n' "$RARS_REV" "$SOURCE_ARCHIVE"

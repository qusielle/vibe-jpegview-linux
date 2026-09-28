#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
	echo "Usage: $0 /rar/backend/root /rars/source.tar.gz /rars/COPYING /docs/directory" >&2
	exit 2
fi
RAR_BACKEND_ROOT=$1
RAR_SOURCE_ARCHIVE=$2
RAR_LICENSE_FILE=$3
DOCS_DIRECTORY=$4
if [ ! -f "$RAR_SOURCE_ARCHIVE" ] || [ ! -f "$RAR_LICENSE_FILE" ]; then
	echo "RAR release packaging requires the pinned RARS source archive and Apache license" >&2
	exit 1
fi
if [ ! -f "$RAR_BACKEND_ROOT/Cargo.toml" ] ||
	[ ! -f "$RAR_BACKEND_ROOT/Cargo.lock" ] ||
	[ ! -f "$RAR_BACKEND_ROOT/src/lib.rs" ]; then
	echo "RAR backend source root is incomplete: $RAR_BACKEND_ROOT" >&2
	exit 1
fi

mkdir -p "$DOCS_DIRECTORY"
TEMP_DIR=$(mktemp -d "$DOCS_DIRECTORY/.rar-backend-source.XXXXXX")
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
mkdir -p "$TEMP_DIR/jpegview-rar-backend"
cp "$RAR_BACKEND_ROOT/Cargo.toml" "$RAR_BACKEND_ROOT/Cargo.lock" \
	"$RAR_BACKEND_ROOT/rust-toolchain.toml" "$TEMP_DIR/jpegview-rar-backend/"
cp -R "$RAR_BACKEND_ROOT/src" "$TEMP_DIR/jpegview-rar-backend/src"
if [ -d "$RAR_BACKEND_ROOT/examples" ]; then
	cp -R "$RAR_BACKEND_ROOT/examples" "$TEMP_DIR/jpegview-rar-backend/examples"
fi
tar -czf "$DOCS_DIRECTORY/jpegview-rar-backend-source.tar.gz" \
	-C "$TEMP_DIR" jpegview-rar-backend
cp "$RAR_SOURCE_ARCHIVE" "$DOCS_DIRECTORY/rars-afc60e4-source.tar.gz"
cp "$RAR_LICENSE_FILE" "$DOCS_DIRECTORY/rars-Apache-2.0.txt"
cp "$(dirname -- "$0")/../COPYING.txt" \
	"$DOCS_DIRECTORY/jpegview-rar-backend-GPL-2.0.txt"
cp "$(dirname -- "$0")/rar-backend-notice.txt" \
	"$DOCS_DIRECTORY/rar-backend-notice.txt"

#!/bin/sh
set -eu

RUSTUP_VERSION=1.28.2
RUSTUP_SHA256=20a06e644b0d9bd2fbdbfd52d42540bdde820ea7df86e92e533c073da0cdd43c
RUST_VERSION=1.89.0

DESTINATION=${1:-}
if [ "$#" -ne 1 ] || [ -z "$DESTINATION" ]; then
	echo "Usage: $0 /absolute/path/to/rust-toolchain" >&2
	exit 2
fi
case "$DESTINATION" in
	/*) ;;
	*) echo "Rust toolchain destination must be an absolute path" >&2; exit 2 ;;
esac
if [ -e "$DESTINATION" ]; then
	echo "Rust toolchain destination already exists: $DESTINATION" >&2
	exit 2
fi

mkdir -p "$DESTINATION"
TEMP_DIR=$(mktemp -d "$DESTINATION/.install.XXXXXX")
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
curl --fail --location --retry 3 \
	"https://static.rust-lang.org/rustup/archive/$RUSTUP_VERSION/x86_64-unknown-linux-gnu/rustup-init" \
	--output "$TEMP_DIR/rustup-init"
printf '%s  %s\n' "$RUSTUP_SHA256" "$TEMP_DIR/rustup-init" | sha256sum --check
chmod 755 "$TEMP_DIR/rustup-init"

export RUSTUP_HOME="$DESTINATION/rustup"
export CARGO_HOME="$DESTINATION/cargo"
"$TEMP_DIR/rustup-init" -y --no-modify-path --profile minimal \
	--default-host x86_64-unknown-linux-gnu --default-toolchain "$RUST_VERSION"
"$CARGO_HOME/bin/rustc" --version | grep -F "rustc $RUST_VERSION " >/dev/null
printf 'Installed Rust %s with rustup %s under %s\n' \
	"$RUST_VERSION" "$RUSTUP_VERSION" "$DESTINATION"

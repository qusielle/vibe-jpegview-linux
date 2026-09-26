#!/bin/sh
set -eu

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
VERSION_SCRIPT="$SCRIPT_DIR/../version.sh"
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
REPO="$TEMP_DIR/repo"
mkdir -p "$REPO"
git -C "$REPO" init -q
git -C "$REPO" config user.name 'Version Test'
git -C "$REPO" config user.email 'version-test@example.invalid'

commit_file() {
	printf '%s\n' "$1" >> "$REPO/history.txt"
	git -C "$REPO" add history.txt
	git -C "$REPO" commit -q -m "$1"
}

get_version() (
	cd "$REPO"
	sh "$VERSION_SCRIPT"
)

expect_version() {
	expected=$1
	actual=$(get_version)
	if [ "$actual" != "$expected" ]; then
		echo "Expected version '$expected', got '$actual'" >&2
		exit 1
	fi
}

commit_file initial
git -C "$REPO" tag -a 1.3.46-linux.3 -m 'Linux release tag'
expect_version 1.3.46-linux.3

commit_file dev1
expect_version 1.3.46-linux.3+dev1
commit_file dev2
commit_file dev3
commit_file dev4
commit_file dev5
expect_version 1.3.46-linux.3+dev5

git -C "$REPO" tag -a release-candidate -m 'Non-semantic tag'
git -C "$REPO" tag -a 1.4 -m 'Invalid semantic version'
expect_version 1.3.46-linux.3+dev5

git -C "$REPO" tag -a v1.3.47 -m 'Version tag with conventional v prefix'
expect_version 1.3.47
commit_file next1
commit_file next2
expect_version 1.3.47+dev2
printf '%s\n' dirty >> "$REPO/history.txt"
expect_version 1.3.47+dev2.dirty

UNVERSIONED_REPO="$TEMP_DIR/unversioned-repo"
mkdir -p "$UNVERSIONED_REPO"
git -C "$UNVERSIONED_REPO" init -q
git -C "$UNVERSIONED_REPO" config user.name 'Version Test'
git -C "$UNVERSIONED_REPO" config user.email 'version-test@example.invalid'
printf '%s\n' unversioned > "$UNVERSIONED_REPO/history.txt"
git -C "$UNVERSIONED_REPO" add history.txt
git -C "$UNVERSIONED_REPO" commit -q -m unversioned
git -C "$UNVERSIONED_REPO" tag -a snapshot -m 'Non-semantic only tag'
unversioned_hash=$(git -C "$UNVERSIONED_REPO" rev-parse --short HEAD)
unversioned_version=$(cd "$UNVERSIONED_REPO" && sh "$VERSION_SCRIPT")
if [ "$unversioned_version" != "0.0.0+dev.g${unversioned_hash}" ]; then
	echo "Expected unversioned fallback 0.0.0+dev.g${unversioned_hash}, got '$unversioned_version'" >&2
	exit 1
fi

mkdir -p "$TEMP_DIR/no-repository"
fallback=$(cd "$TEMP_DIR/no-repository" && sh "$VERSION_SCRIPT")
if [ "$fallback" != '0.0.0+unknown' ]; then
	echo "Expected no-repository fallback 0.0.0+unknown, got '$fallback'" >&2
	exit 1
fi

echo 'Version tests passed.'

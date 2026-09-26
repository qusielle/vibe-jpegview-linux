#!/bin/sh
set -eu

output=${1:?Usage: write-build-info.sh OUTPUT VERSION}
version=${2:?Usage: write-build-info.sh OUTPUT VERSION}

case "$version" in
	''|*[!A-Za-z0-9.+-]*)
		echo "Invalid application version: $version" >&2
		exit 2
		;;
esac

mkdir -p "$(dirname -- "$output")"
temporary="${output}.tmp.$$"
trap 'rm -f "$temporary"' EXIT HUP INT TERM
printf '#define JPEGVIEW_APP_VERSION "%s"\n' "$version" > "$temporary"
mv -f "$temporary" "$output"
trap - EXIT HUP INT TERM

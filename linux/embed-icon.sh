#!/bin/sh
set -eu

input=$1
output=$2
temporary="$output.tmp"

{
	printf '%s\n' '#pragma once' '' '#include <cstddef>' '' 'namespace jpegview_linux {'
	printf '%s\n' 'inline constexpr unsigned char kApplicationIconIco[] = {'
	od -An -v -t u1 "$input" | awk '
		BEGIN { column = 0 }
		{
			for (field = 1; field <= NF; ++field) {
				printf "%s%3d,", column == 0 ? "\t" : " ", $field
				column = (column + 1) % 16
				if (column == 0) printf "\n"
			}
		}
		END { if (column != 0) printf "\n" }
	'
	printf '%s\n' '};' \
		'inline constexpr std::size_t kApplicationIconIcoSize = sizeof(kApplicationIconIco);' \
		'} // namespace jpegview_linux'
} > "$temporary"

if [ -f "$output" ] && cmp -s "$temporary" "$output"; then
	rm -f "$temporary"
else
	mv "$temporary" "$output"
fi

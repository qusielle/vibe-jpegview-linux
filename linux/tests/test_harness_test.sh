#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: test_harness_test.sh /path/to/jpegview-tests" >&2
	exit 2
fi

test_binary=$1
temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' EXIT HUP INT TERM

"$test_binary" --list-suites > "$temporary_directory/suites"
cat > "$temporary_directory/expected-suites" <<'EOF'
source_navigation
work_admission
codec_display
image_cache
image_operations
viewer_models
dialogs_sessions
EOF
if ! cmp -s "$temporary_directory/expected-suites" "$temporary_directory/suites"; then
	echo "test suite listing changed unexpectedly" >&2
	diff -u "$temporary_directory/expected-suites" "$temporary_directory/suites" >&2 || true
	exit 1
fi

"$test_binary" --list > "$temporary_directory/tests"
if [ ! -s "$temporary_directory/tests" ]; then
	echo "test listing was empty" >&2
	exit 1
fi
if sort "$temporary_directory/tests" | uniq -d | grep -q .; then
	echo "test listing contains duplicate names" >&2
	exit 1
fi
grep -Fxq "viewport-manual-zoom-pan-and-restore" "$temporary_directory/tests"

"$test_binary" --suite viewer_models \
	--filter viewport-manual-zoom-pan-and-restore > "$temporary_directory/focused"
if [ "$(grep -c '^PASS ' "$temporary_directory/focused")" -ne 1 ] ||
	! grep -Fxq "PASS viewport-manual-zoom-pan-and-restore" "$temporary_directory/focused" ||
	! grep -Fxq "All selected core tests passed (1)" "$temporary_directory/focused"; then
	echo "focused suite and substring selection did not run exactly one case" >&2
	cat "$temporary_directory/focused" >&2
	exit 1
fi

if "$test_binary" --suite missing_suite > "$temporary_directory/invalid" 2>&1; then
	echo "unknown suite was accepted" >&2
	exit 1
fi
if "$test_binary" --filter no_such_core_test > "$temporary_directory/invalid" 2>&1; then
	echo "empty test selection was accepted" >&2
	exit 1
fi

test_count=$(wc -l < "$temporary_directory/tests" | tr -d ' ')
echo "Test harness CLI checks passed ($test_count unique test names)."

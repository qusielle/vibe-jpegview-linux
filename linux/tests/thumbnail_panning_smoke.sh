#!/bin/sh
set -eu

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
BINARY=${1:-./build/jpegview-linux}
if [ ! -x "$BINARY" ]; then
	echo "thumbnail pan smoke test: binary not found: $BINARY" >&2
	exit 2
fi
BINARY=$(cd -- "$(dirname -- "$BINARY")" && pwd)/$(basename -- "$BINARY")

for command in Xvfb xdotool openbox cc convert compare import awk; do
	if ! command -v "$command" >/dev/null 2>&1; then
		echo "thumbnail pan smoke test: SKIP (missing $command)"
		exit 0
	fi
done

temporary=$(mktemp -d)
viewer_pid=''
window_manager_pid=''
xvfb_pid=''
display_number=''
release_file="$temporary/read.release"
active_file="$temporary/read.active"

cleanup() {
	: > "$release_file"
	if [ -n "$viewer_pid" ]; then
		kill "$viewer_pid" 2>/dev/null || true
		wait "$viewer_pid" 2>/dev/null || true
	fi
	if [ -n "$window_manager_pid" ]; then kill "$window_manager_pid" 2>/dev/null || true; fi
	if [ -n "$xvfb_pid" ]; then kill "$xvfb_pid" 2>/dev/null || true; fi
	rm -rf -- "$temporary"
}
trap cleanup EXIT INT TERM

Xvfb -displayfd 1 -screen 0 1280x800x24 >"$temporary/display" 2>"$temporary/xvfb.log" &
xvfb_pid=$!
for _ in $(seq 1 100); do
	if [ -s "$temporary/display" ]; then
		display_number=$(sed -n '1p' "$temporary/display")
		break
	fi
	sleep 0.02
done
if [ -z "$display_number" ]; then
	echo "thumbnail pan smoke test: Xvfb did not start" >&2
	cat "$temporary/xvfb.log" >&2
	exit 1
fi
DISPLAY=":$display_number" openbox >"$temporary/openbox.log" 2>&1 &
window_manager_pid=$!

source_directory="$temporary/images"
configuration="$temporary/config/jpegview-linux"
mkdir -p "$source_directory" "$configuration" "$temporary/home" "$temporary/state"
draw_commands=''
for stripe_x in $(seq 0 96 1728); do
	stripe_end=$((stripe_x + 47))
	draw_commands="$draw_commands rectangle $stripe_x,0 $stripe_end,1199"
done
convert -size 1800x1200 xc:white -fill '#202020' -draw "$draw_commands" \
	-depth 8 "$source_directory/01-current.ppm"
convert -size 640x480 plasma:fractal -quality 90 "$source_directory/02-blocked.jpg"
printf '%s\n' \
	'thumbnail_panel_visible=1' \
	'thumbnail_panel_width=164' \
	'cache_size_mb=0' \
	'double_page_mode_enabled=0' \
	'spacebar_navigates_images=0' \
	>"$configuration/settings.conf"
cc -shared -fPIC "$SCRIPT_DIR/delay_mmap.c" -o "$temporary/slow_map.so" -ldl

trace_file="$temporary/perf.csv"
started_file="$temporary/read.started"
DISPLAY=":$display_number" HOME="$temporary/home" \
	XDG_CONFIG_HOME="$temporary/config" XDG_STATE_HOME="$temporary/state" \
	LD_PRELOAD="$temporary/slow_map.so" \
	JPEGVIEW_TEST_SLOW_MAP="$source_directory/02-blocked.jpg" \
	JPEGVIEW_TEST_SLOW_MAP_STARTED="$started_file" \
	JPEGVIEW_TEST_SLOW_MAP_ACTIVE="$active_file" \
	JPEGVIEW_TEST_SLOW_MAP_MAX_ATTEMPTS=6000 \
	JPEGVIEW_TEST_SLOW_MAP_RELEASE="$release_file" \
	JPEGVIEW_PERF_TRACE="$trace_file" \
	"$BINARY" "$source_directory/01-current.ppm" >"$temporary/viewer.log" 2>&1 &
viewer_pid=$!

window_id=''
for _ in $(seq 1 200); do
	window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
		--class jpegview-linux 2>/dev/null | head -1 || true)
	if [ -n "$window_id" ]; then break; fi
	sleep 0.02
done
if [ -z "$window_id" ]; then
	echo "thumbnail pan smoke test: viewer window did not appear" >&2
	cat "$temporary/viewer.log" >&2
	exit 1
fi
DISPLAY=":$display_number" xdotool windowactivate "$window_id"

map_started=0
for _ in $(seq 1 300); do
	if [ -f "$started_file" ]; then map_started=1; break; fi
	if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
	sleep 0.02
done
if [ "$map_started" -ne 1 ]; then
	: > "$release_file"
	echo "thumbnail pan smoke test: independent thumbnail read did not reach the controlled barrier" >&2
	cat "$temporary/viewer.log" >&2
	exit 1
fi

require_barrier_active() {
	if [ ! -f "$active_file" ]; then
		echo "thumbnail pan smoke test: controlled mmap barrier was no longer active during $1" >&2
		return 1
	fi
}

capture_image_area() {
	image=$1
	require_barrier_active "capture of $image"
	DISPLAY=":$display_number" import -window "$window_id" "$image.window.png"
	# The crop is entirely inside the image area and above the zoom navigator.
	convert "$image.window.png" -crop 800x300+180+100 +repage "$image"
	require_barrier_active "capture of $image"
}

pattern_color_count() {
	convert "$1" -format '%k' info:
}

fit_image="$temporary/fit.png"
pattern_colors=1
for _ in $(seq 1 200); do
	capture_image_area "$fit_image"
	pattern_colors=$(pattern_color_count "$fit_image")
	if [ "$pattern_colors" -gt 1 ]; then break; fi
	sleep 0.02
done
if [ "$pattern_colors" -le 1 ]; then
	: > "$release_file"
	echo "thumbnail pan smoke test: patterned current image was not visible while the mmap barrier was active" >&2
	cat "$temporary/viewer.log" >&2
	exit 1
fi

input_presentations() {
	awk -F, '$2 == "input_to_present" { count++ } END { print count + 0 }' "$trace_file"
}

wait_for_presentation_after() {
	minimum=$1
	for _ in $(seq 1 120); do
		current=$(input_presentations)
		if [ "$current" -gt "$minimum" ]; then return 0; fi
		sleep 0.02
	done
	return 1
}

pixel_difference() {
	require_barrier_active "pixel comparison of $1 and $2"
	metric=$(compare -metric AE "$1" "$2" null: 2>&1 || true)
	difference=${metric%% *}
	case "$difference" in
		''|*[!0-9]*)
			echo "thumbnail pan smoke test: could not parse pixel difference '$metric'" >&2
			return 1
			;;
	esac
	require_barrier_active "pixel comparison of $1 and $2"
	printf '%s\n' "$difference"
}

wait_for_stable_image_area() {
	image=$1
	previous="$image.previous.png"
	stable_samples=0
	for _ in $(seq 1 40); do
		capture_image_area "$image"
		if [ -f "$previous" ] && [ "$(pixel_difference "$previous" "$image")" -eq 0 ]; then
			stable_samples=$((stable_samples + 1))
		else
			stable_samples=0
		fi
		if [ "$stable_samples" -ge 3 ]; then return 0; fi
		cp "$image" "$previous"
		sleep 0.05
	done
	return 1
}

presented_actual_size=0
presented_bare_shift=0
presented_pan=0
before_actual_size=$(input_presentations)
DISPLAY=":$display_number" xdotool key space
if wait_for_presentation_after "$before_actual_size"; then
	presented_actual_size=1
fi
if [ "$presented_actual_size" -eq 1 ]; then
	if ! wait_for_stable_image_area "$temporary/actual-size.png"; then
		: > "$release_file"
		echo "thumbnail pan smoke test: actual-size presentation did not settle while an unrelated thumbnail read was blocked" >&2
		exit 1
	fi
	fit_to_actual_difference=$(pixel_difference "$fit_image" "$temporary/actual-size.png")
	if [ "$fit_to_actual_difference" -le 10000 ]; then
		: > "$release_file"
		echo "thumbnail pan smoke test: Space did not visibly switch fit-to-window to actual size ($fit_to_actual_difference pixels changed)" >&2
		exit 1
	fi
	before_bare_shift=$(input_presentations)
	DISPLAY=":$display_number" xdotool key shift
	if wait_for_presentation_after "$before_bare_shift"; then
		presented_bare_shift=1
	fi
	if [ "$presented_bare_shift" -eq 1 ]; then
		if ! wait_for_stable_image_area "$temporary/bare-shift.png"; then
			: > "$release_file"
			echo "thumbnail pan smoke test: bare-Shift presentation did not settle while an unrelated thumbnail read was blocked" >&2
			exit 1
		fi
		bare_shift_difference=$(pixel_difference "$temporary/actual-size.png" "$temporary/bare-shift.png")
		if [ "$bare_shift_difference" -ne 0 ]; then
			: > "$release_file"
			echo "thumbnail pan smoke test: bare Shift unexpectedly changed the image area ($bare_shift_difference pixels changed)" >&2
			exit 1
		fi
	fi
	before_pan=$(input_presentations)
	DISPLAY=":$display_number" xdotool key shift+Right
	if wait_for_presentation_after "$before_pan"; then
		presented_pan=1
	else
		presented_pan=0
	fi
	if [ "$presented_pan" -eq 1 ]; then
		capture_image_area "$temporary/pan.png"
		pan_difference=$(pixel_difference "$temporary/actual-size.png" "$temporary/pan.png")
		if [ "$pan_difference" -le 180000 ]; then
			: > "$release_file"
			echo "thumbnail pan smoke test: Shift+Right did not present the expected 48-pixel image translation ($pan_difference pixels changed)" >&2
			exit 1
		fi
	fi
fi

# Release the held read before reporting failure so the test never strands a
# process waiting on its test-only barrier.
barrier_was_active=0
if [ -f "$active_file" ]; then barrier_was_active=1; fi
: > "$release_file"
if [ "$presented_actual_size" -ne 1 ] || [ "$presented_pan" -ne 1 ]; then
	if [ "$presented_actual_size" -ne 1 ]; then
		echo "thumbnail pan smoke test: actual-size input was not presented while an unrelated thumbnail read was blocked" >&2
	else
		echo "thumbnail pan smoke test: pan input was not presented while an unrelated thumbnail read was blocked" >&2
	fi
	cat "$temporary/viewer.log" >&2
	exit 1
fi

if [ "$presented_bare_shift" -ne 1 ]; then
	echo "thumbnail pan smoke test: bare-Shift negative control did not produce its input presentation" >&2
	cat "$temporary/viewer.log" >&2
	exit 1
fi
if [ "$barrier_was_active" -ne 1 ]; then
	echo "thumbnail pan smoke test: controlled mmap barrier timed out before the pan assertions completed" >&2
	cat "$temporary/viewer.log" >&2
	exit 1
fi

echo "thumbnail pan smoke test passed (fit-to-actual: $fit_to_actual_difference changed pixels; bare Shift: $bare_shift_difference; pan: $pan_difference; mmap barrier active)"

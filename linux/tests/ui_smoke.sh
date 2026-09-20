#!/bin/sh
set -eu

BINARY=${1:-./build/jpegview-linux}
if [ ! -x "$BINARY" ]; then
	echo "UI smoke test: binary not found: $BINARY" >&2
	exit 2
fi

for command in Xvfb xdotool openbox wmctrl; do
	if ! command -v "$command" >/dev/null 2>&1; then
		echo "UI smoke test: SKIP (missing $command)"
		exit 0
	fi
done

visual_assertions=1
for command in import compare convert identify xprop; do
	if ! command -v "$command" >/dev/null 2>&1; then
		visual_assertions=0
	fi
done

temporary=$(mktemp -d)
xvfb_pid=''
window_manager_pid=''
viewer_pid=''

cleanup() {
	if [ -n "$viewer_pid" ]; then kill "$viewer_pid" 2>/dev/null || true; fi
	if [ -n "$window_manager_pid" ]; then kill "$window_manager_pid" 2>/dev/null || true; fi
	if [ -n "$xvfb_pid" ]; then kill "$xvfb_pid" 2>/dev/null || true; fi
	rm -rf -- "$temporary"
}
trap cleanup EXIT INT TERM

mkdir -p "$temporary/images"

write_ppm() {
	filename=$1
	red=$2
	green=$3
	blue=$4
	{
		printf 'P6\n2 2\n255\n'
		printf "\\%03o\\%03o\\%03o" "$red" "$green" "$blue"
		printf "\\%03o\\%03o\\%03o" "$red" "$green" "$blue"
		printf "\\%03o\\%03o\\%03o" "$red" "$green" "$blue"
		printf "\\%03o\\%03o\\%03o" "$red" "$green" "$blue"
	} > "$filename"
}

write_ppm "$temporary/images/01-red.ppm" 255 0 0
write_ppm "$temporary/images/02-green.ppm" 0 255 0
write_ppm "$temporary/images/03-blue.ppm" 0 0 255
write_ppm "$temporary/images/04-yellow.ppm" 255 255 0
write_ppm "$temporary/images/05-cyan.ppm" 0 255 255
mkdir -p "$temporary/images/00-album/first-subdir" "$temporary/images/00-album/second-subdir"
write_ppm "$temporary/images/00-album/first.ppm" 128 64 32
write_ppm "$temporary/images/00-album/second.ppm" 32 64 128
touch -t 202001010000.00 "$temporary/images/00-album/first.ppm" "$temporary/images/00-album/second.ppm"
mkdir -p "$temporary/images/00-entry-test"
write_ppm "$temporary/images/00-entry-test/inside-first.ppm" 64 128 32
mkdir -p "$temporary/images/00-wheel-test"
for index in $(seq 0 39); do
	filename=$(printf '%02d' "$index")
	write_ppm "$temporary/images/00-wheel-test/wheel-$filename.ppm" 64 32 16
done
touch -t 202001010000.00 "$temporary/images/01-red.ppm" "$temporary/images/02-green.ppm" \
	"$temporary/images/03-blue.ppm" "$temporary/images/04-yellow.ppm" "$temporary/images/05-cyan.ppm"
touch -t 202001010000.00 "$temporary/images/00-album"
touch -t 202201010000.00 "$temporary/images/00-entry-test"
touch -t 201901010000.00 "$temporary/images/00-wheel-test"

help_text=$($BINARY --help)
case "$help_text" in
	*"mouse wheel up/down navigates previous/next"*"Ctrl+mouse wheel zooms"*) ;;
	*) echo "UI smoke test: --help does not describe wheel controls" >&2; exit 1 ;;
esac

Xvfb -displayfd 1 -screen 0 1280x800x24 >"$temporary/display" 2>"$temporary/xvfb.log" &
xvfb_pid=$!
display_number=''
for _ in $(seq 1 50); do
	if [ -s "$temporary/display" ]; then
		display_number=$(sed -n '1p' "$temporary/display")
		break
	fi
	sleep 0.1
done
if [ -z "$display_number" ]; then
	echo "UI smoke test: Xvfb did not start" >&2
	exit 1
fi
DISPLAY=":$display_number" openbox >"$temporary/openbox.log" 2>&1 &
window_manager_pid=$!
sleep 0.5

launch_viewer() {
	DISPLAY=":$display_number" HOME="$temporary/home" XDG_CONFIG_HOME="$temporary/config" \
		"$BINARY" "$temporary/images" >"$temporary/viewer.log" 2>&1 &
	viewer_pid=$!
	window_id=''
	for _ in $(seq 1 50); do
		window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible --class jpegview-linux 2>/dev/null | head -1 || true)
		if [ -n "$window_id" ]; then break; fi
		sleep 0.1
	done
	if [ -z "$window_id" ]; then
		echo "UI smoke test: viewer window did not appear" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	sleep 0.3
}

stop_viewer() {
	DISPLAY=":$display_number" xdotool key q || true
	wait "$viewer_pid" || true
	viewer_pid=''
}

click_file_dialog_sort() {
	dialog_window_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^WIDTH=//p')
	dialog_window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
	dialog_width=$((dialog_window_width - 40))
	if [ "$dialog_width" -gt 900 ]; then dialog_width=900; fi
	if [ "$dialog_width" -lt 320 ]; then dialog_width=320; fi
	dialog_height=$((dialog_window_height - 40))
	if [ "$dialog_height" -gt 650 ]; then dialog_height=650; fi
	if [ "$dialog_height" -lt 260 ]; then dialog_height=260; fi
	sort_x=$(((dialog_window_width - dialog_width) / 2 + dialog_width - 88))
	sort_y=$(((dialog_window_height - dialog_height) / 2 + 72))
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" "$sort_x" "$sort_y" click 1
}

launch_viewer
if [ "$visual_assertions" -eq 1 ]; then
	window_icon=$(DISPLAY=":$display_number" xprop -id "$window_id" _NET_WM_ICON 2>/dev/null || true)
	case "$window_icon" in
		*"Icon (64 x 64):"*) ;;
		*) echo "UI smoke test: native window did not publish the embedded 64x64 icon" >&2; exit 1 ;;
	esac
fi

DISPLAY=":$display_number" xdotool key ctrl+o
sleep 0.3
if [ "$visual_assertions" -eq 1 ]; then
	summary_rendered=0
	for _ in $(seq 1 20); do
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/open-dialog-empty.png"
		open_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^WIDTH=//p')
		open_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
		dialog_width=$((open_width - 40))
		if [ "$dialog_width" -gt 900 ]; then dialog_width=900; fi
		if [ "$dialog_width" -lt 320 ]; then dialog_width=320; fi
		dialog_height=$((open_height - 40))
		if [ "$dialog_height" -gt 650 ]; then dialog_height=650; fi
		if [ "$dialog_height" -lt 260 ]; then dialog_height=260; fi
		preview_width=0
		if [ "$dialog_width" -ge 560 ]; then
			preview_width=$((dialog_width / 3))
			if [ "$preview_width" -lt 200 ]; then preview_width=200; fi
			if [ "$preview_width" -gt 260 ]; then preview_width=260; fi
		fi
		summary_probe_x=$(((open_width - dialog_width) / 2 + dialog_width - 24 - preview_width - 119))
		summary_probe_y=$(((open_height - dialog_height) / 2 + 141))
		convert "$temporary/open-dialog-empty.png" -crop "28x11+${summary_probe_x}+${summary_probe_y}" +repage \
			-format %c histogram:info:- >"$temporary/summary-histogram.txt"
		if grep -qi '#9BAFC3' "$temporary/summary-histogram.txt"; then
			summary_rendered=1
			break
		fi
		sleep 0.05
	done
	if [ "$summary_rendered" -ne 1 ]; then
		echo "UI smoke test: Ctrl+O did not render the right-aligned directory image/subdirectory count" >&2
		exit 1
	fi
fi
DISPLAY=":$display_number" xdotool type --delay 20 '03-BLUE'
sleep 0.3
if [ "$visual_assertions" -eq 1 ]; then
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/open-dialog-filtered.png"
	open_dialog_difference=$(compare -metric AE "$temporary/open-dialog-empty.png" \
		"$temporary/open-dialog-filtered.png" null: 2>&1 || true)
	if [ "$open_dialog_difference" = "0" ]; then
		echo "UI smoke test: Ctrl+O did not show the typed filename filter" >&2
		exit 1
	fi
fi
DISPLAY=":$display_number" xdotool key Return
sleep 0.4
filtered_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$filtered_title" in
	03-blue.ppm*) ;;
	*) echo "UI smoke test: Ctrl+O filename filter did not open the matching image" >&2; exit 1 ;;
esac

DISPLAY=":$display_number" xdotool key ctrl+o
click_file_dialog_sort
DISPLAY=":$display_number" xdotool key Home
DISPLAY=":$display_number" xdotool key Down
DISPLAY=":$display_number" xdotool key ctrl+Return
sleep 0.3
immediate_directory_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$immediate_directory_title" in
	inside-first.ppm*) ;;
	*) echo "UI smoke test: modification-date sorting did not reorder folders newest first" >&2; exit 1 ;;
esac

# Restore name sorting and a root-level image so the ordinary directory-entry
# behavior below starts from the same state as the initial open-dialog checks.
DISPLAY=":$display_number" xdotool key ctrl+o
click_file_dialog_sort
DISPLAY=":$display_number" xdotool key BackSpace
DISPLAY=":$display_number" xdotool type --delay 20 '03-BLUE'
DISPLAY=":$display_number" xdotool key Return
sleep 0.3

DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool type --delay 20 '00-ENTRY-TEST'
DISPLAY=":$display_number" xdotool key Return
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
entered_directory_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$entered_directory_title" in
	inside-first.ppm*) ;;
	*) echo "UI smoke test: entering a directory focused [..] instead of its first child" >&2; exit 1 ;;
esac

DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool key BackSpace
DISPLAY=":$display_number" xdotool key Return
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
restored_directory_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$restored_directory_title" in
	inside-first.ppm*) ;;
	*) echo "UI smoke test: returning to the parent did not focus the directory just exited" >&2; exit 1 ;;
esac

DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool key BackSpace
DISPLAY=":$display_number" xdotool type --delay 20 '.ppm'
DISPLAY=":$display_number" xdotool keydown Down
sleep 0.9
DISPLAY=":$display_number" xdotool keyup Down
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
repeated_dialog_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$repeated_dialog_title" in
	05-cyan.ppm*) ;;
	*) echo "UI smoke test: held Down did not repeat selection in the Ctrl+O dialog" >&2; exit 1 ;;
esac

DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool type --delay 20 '.ppm'
DISPLAY=":$display_number" xdotool key Page_Down
DISPLAY=":$display_number" xdotool key Page_Up
DISPLAY=":$display_number" xdotool key Down
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
paged_dialog_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$paged_dialog_title" in
	01-red.ppm*) ;;
	*) echo "UI smoke test: PageUp/PageDown did not page through the Ctrl+O dialog" >&2; exit 1 ;;
esac

DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool type --delay 20 '.ppm'
DISPLAY=":$display_number" xdotool key End
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
end_dialog_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$end_dialog_title" in
	05-cyan.ppm*) ;;
	*) echo "UI smoke test: End did not select the last row in the Ctrl+O dialog" >&2; exit 1 ;;
esac

DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool type --delay 20 '.ppm'
DISPLAY=":$display_number" xdotool key Home
DISPLAY=":$display_number" xdotool key Down
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
home_dialog_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$home_dialog_title" in
	01-red.ppm*) ;;
	*) echo "UI smoke test: Home did not select the first row in the Ctrl+O dialog" >&2; exit 1 ;;
esac

# A wheel event over an overflowing listing scrolls rows under the pointer and
# activates the newly focused item, instead of navigating the viewer behind it.
DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool type --delay 10 '00-WHEEL-TEST'
DISPLAY=":$display_number" xdotool key Return
sleep 0.2
wheel_window_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^WIDTH=//p')
wheel_window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
wheel_dialog_width=$((wheel_window_width - 40))
if [ "$wheel_dialog_width" -gt 900 ]; then wheel_dialog_width=900; fi
if [ "$wheel_dialog_width" -lt 320 ]; then wheel_dialog_width=320; fi
wheel_dialog_height=$((wheel_window_height - 40))
if [ "$wheel_dialog_height" -gt 650 ]; then wheel_dialog_height=650; fi
if [ "$wheel_dialog_height" -lt 260 ]; then wheel_dialog_height=260; fi
wheel_dialog_x=$(((wheel_window_width - wheel_dialog_width) / 2))
wheel_dialog_y=$(((wheel_window_height - wheel_dialog_height) / 2))
wheel_list_x=$((wheel_dialog_x + 28))
wheel_list_y=$((wheel_dialog_y + 112 + 2 * 26 + 13))
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" "$wheel_list_x" "$wheel_list_y"
DISPLAY=":$display_number" xdotool click 5
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
wheel_dialog_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$wheel_dialog_title" in
	wheel-04.ppm*) ;;
	*) echo "UI smoke test: mouse wheel did not scroll and select through the open-dialog file list" >&2; exit 1 ;;
esac
DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool key BackSpace
DISPLAY=":$display_number" xdotool type --delay 10 '01-RED'
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
wheel_restore_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$wheel_restore_title" in
	01-red.ppm*) ;;
	*) echo "UI smoke test: returning from the wheel fixture did not restore the root image listing" >&2; exit 1 ;;
esac

# Alt+Left/Right jump between populated sibling folders and choose the first
# image in that folder, independently of the currently selected root image.
DISPLAY=":$display_number" xdotool key ctrl+o
sleep 0.2
DISPLAY=":$display_number" xdotool key Home
DISPLAY=":$display_number" xdotool key Down
DISPLAY=":$display_number" xdotool key ctrl+Return
sleep 0.3
sibling_start_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$sibling_start_title" in
	first.ppm*) ;;
	*) echo "UI smoke test: could not enter the first sibling-folder fixture ($sibling_start_title)" >&2; exit 1 ;;
esac
DISPLAY=":$display_number" xdotool key alt+Right
sleep 0.3
sibling_next_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$sibling_next_title" in
	inside-first.ppm*) ;;
	*) echo "UI smoke test: Alt+Right did not open the next sibling folder's first image" >&2; exit 1 ;;
esac
DISPLAY=":$display_number" xdotool key alt+Left
sleep 0.3
sibling_previous_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$sibling_previous_title" in
	first.ppm*) ;;
	*) echo "UI smoke test: Alt+Left did not open the previous sibling folder's first image" >&2; exit 1 ;;
esac
DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool key BackSpace
DISPLAY=":$display_number" xdotool type --delay 10 '01-RED'
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
sibling_restore_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$sibling_restore_title" in
	01-red.ppm*) ;;
	*) echo "UI smoke test: sibling-folder test did not restore the root image" >&2; exit 1 ;;
esac

# Resize the dialog and its preview/list split at the end of the dialog tests,
# so later launches can verify the saved dimensions and ratio.
DISPLAY=":$display_number" xdotool key ctrl+o
sleep 0.2
open_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^WIDTH=//p')
open_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
dialog_width=$((open_width - 40))
if [ "$dialog_width" -gt 900 ]; then dialog_width=900; fi
if [ "$dialog_width" -lt 320 ]; then dialog_width=320; fi
dialog_height=$((open_height - 40))
if [ "$dialog_height" -gt 650 ]; then dialog_height=650; fi
if [ "$dialog_height" -lt 260 ]; then dialog_height=260; fi
dialog_x=$(((open_width - dialog_width) / 2))
dialog_y=$(((open_height - dialog_height) / 2))
resize_delta_x=60
resize_delta_y=35
resize_start_x=$((dialog_x + dialog_width - 2))
resize_start_y=$((dialog_y + dialog_height - 2))
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" "$resize_start_x" "$resize_start_y"
DISPLAY=":$display_number" xdotool mousedown 1
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
	$((resize_start_x + resize_delta_x)) $((resize_start_y + resize_delta_y))
DISPLAY=":$display_number" xdotool mouseup 1
dialog_width=$((dialog_width + resize_delta_x))
dialog_height=$((dialog_height + resize_delta_y))
preview_width=$((dialog_width / 3))
if [ "$preview_width" -lt 200 ]; then preview_width=200; fi
if [ "$preview_width" -gt 260 ]; then preview_width=260; fi
divider_x=$((dialog_x + dialog_width - preview_width - 18))
divider_y=$((dialog_y + 112 + 120))
preview_expand=60
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" "$divider_x" "$divider_y"
DISPLAY=":$display_number" xdotool mousedown 1
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
	$((divider_x - preview_expand)) "$divider_y"
DISPLAY=":$display_number" xdotool mouseup 1
preview_width=$((preview_width + preview_expand))
if [ "$visual_assertions" -eq 1 ]; then
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((dialog_x + 40)) $((dialog_y + 20))
	sleep 0.2
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/open-dialog-resized.png"
	resize_right_x=$((dialog_x + dialog_width - 1))
	resize_border=$(convert "$temporary/open-dialog-resized.png" \
		-format "%[hex:p{$resize_right_x,$((dialog_y + 10))}]" info:)
	if [ "${resize_border#BEBEBE}" = "$resize_border" ]; then
		echo "UI smoke test: dragging the file-dialog corner did not resize its right edge" >&2
		exit 1
	fi
	preview_left_x=$((dialog_x + dialog_width - 12 - preview_width))
	preview_border=$(convert "$temporary/open-dialog-resized.png" \
		-format "%[hex:p{$preview_left_x,$((dialog_y + 122))}]" info:)
	if [ "${preview_border#4B4B4B}" = "$preview_border" ]; then
		echo "UI smoke test: dragging the preview divider did not resize the preview" >&2
		exit 1
	fi
fi
DISPLAY=":$display_number" xdotool key Escape

# A short press must advance exactly once. Background display preparation can
# delay a frame, so treating the still-physical key as a hold immediately after
# the first render used to advance from 01 directly to 03.
DISPLAY=":$display_number" xdotool key Right
sleep 0.2
single_press_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$single_press_title" in
	02-green.ppm*) ;;
	*) echo "UI smoke test: one Right press skipped over the adjacent image" >&2; exit 1 ;;
esac
DISPLAY=":$display_number" xdotool key Left
sleep 0.2

title_before=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
DISPLAY=":$display_number" xdotool mousemove 640 400
DISPLAY=":$display_number" xdotool click 4
sleep 0.4
title_after_wheel=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
if [ "$title_before" = "$title_after_wheel" ]; then
	echo "UI smoke test: plain wheel did not navigate" >&2
	exit 1
fi

title_before_hold=$title_after_wheel
DISPLAY=":$display_number" xdotool keydown Right
sleep 0.7
DISPLAY=":$display_number" xdotool keyup Right
sleep 0.2
title_after_hold=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
if [ "$title_before_hold" = "$title_after_hold" ]; then
	echo "UI smoke test: held Right key did not repeat navigation" >&2
	exit 1
fi
sleep 0.3
title_after_release_settled=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
if [ "$title_after_hold" != "$title_after_release_settled" ]; then
	echo "UI smoke test: navigation continued after Right was released" >&2
	exit 1
fi

if [ "$visual_assertions" -eq 1 ]; then
	# Persistent overlays must remain painted on the intermediate frame shown
	# before each held-navigation decode.  The EXIF panel starts at (4,4), where
	# its opaque grey border is distinct from the dark image-area background.
	DISPLAY=":$display_number" xdotool key F2
	sleep 0.2
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/info-before-hold.png"
	info_border=$(convert "$temporary/info-before-hold.png" -format '%[hex:p{4,4}]' info:)
	case "$info_border" in
		696969*) ;;
		*) echo "UI smoke test: F2 did not show the EXIF overlay at its expected position" >&2; exit 1 ;;
	esac

	info_overlay_stable=1
	DISPLAY=":$display_number" xdotool keydown Right
	for sample in $(seq 1 24); do
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/info-held-$sample.png"
		info_border=$(convert "$temporary/info-held-$sample.png" -format '%[hex:p{4,4}]' info:)
		case "$info_border" in
			696969*) ;;
			*) info_overlay_stable=0 ;;
		esac
	done
	DISPLAY=":$display_number" xdotool keyup Right
	DISPLAY=":$display_number" xdotool key F2
	if [ "$info_overlay_stable" -ne 1 ]; then
		echo "UI smoke test: EXIF overlay disappeared during held navigation" >&2
		exit 1
	fi
fi

title_before_ctrl_wheel=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
DISPLAY=":$display_number" xdotool keydown ctrl
DISPLAY=":$display_number" xdotool click 5
DISPLAY=":$display_number" xdotool keyup ctrl
sleep 0.3
title_after_ctrl_wheel=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
if [ "$title_before_ctrl_wheel" != "$title_after_ctrl_wheel" ]; then
	echo "UI smoke test: Ctrl+wheel navigated instead of zooming" >&2
	exit 1
fi

DISPLAY=":$display_number" xdotool key Home
sleep 0.2
if [ "$visual_assertions" -eq 1 ]; then
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/thumbnails-before.png"
fi
DISPLAY=":$display_number" xdotool key ctrl+t
sleep 0.5
if [ "$visual_assertions" -eq 1 ]; then
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/thumbnails-open.png"
	thumbnail_difference=$(compare -metric AE "$temporary/thumbnails-before.png" \
		"$temporary/thumbnails-open.png" null: 2>&1 || true)
	if [ "$thumbnail_difference" = "0" ]; then
		echo "UI smoke test: Ctrl+T did not show the thumbnail panel" >&2
		exit 1
	fi
	thumbnail_capture_width=$(identify -format '%w' "$temporary/thumbnails-open.png")
	thumbnail_capture_height=$(identify -format '%h' "$temporary/thumbnails-open.png")
	thumbnail_content_width=$((thumbnail_capture_width - 180))
	if [ "$thumbnail_content_width" -gt 0 ]; then
		convert "$temporary/thumbnails-before.png" \
			-crop "${thumbnail_content_width}x${thumbnail_capture_height}+180+0" +repage \
			"$temporary/thumbnails-content-before.png"
		convert "$temporary/thumbnails-open.png" \
			-crop "${thumbnail_content_width}x${thumbnail_capture_height}+180+0" +repage \
			"$temporary/thumbnails-content-open.png"
		thumbnail_content_difference=$(compare -metric AE "$temporary/thumbnails-content-before.png" \
			"$temporary/thumbnails-content-open.png" null: 2>&1 || true)
		if [ "$thumbnail_content_difference" = "0" ]; then
			echo "UI smoke test: thumbnail panel overlaid the image instead of reserving space" >&2
			exit 1
		fi
	fi
fi
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 164 300
DISPLAY=":$display_number" xdotool mousedown 1
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 240 300
DISPLAY=":$display_number" xdotool mouseup 1
sleep 0.3
title_before_thumbnail_click=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
thumbnail_window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
thumbnail_neighbor_y=$(((thumbnail_window_height + 163) / 2))
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 80 "$thumbnail_neighbor_y"
DISPLAY=":$display_number" xdotool click 1
sleep 0.3
title_after_thumbnail_click=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
if [ "$title_before_thumbnail_click" = "$title_after_thumbnail_click" ]; then
	echo "UI smoke test: clicking a neighboring thumbnail did not navigate" >&2
	exit 1
fi
DISPLAY=":$display_number" xdotool key ctrl+t
sleep 0.2

if [ "$visual_assertions" -eq 1 ]; then
	DISPLAY=":$display_number" xdotool mousemove 640 400
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/context-before.png"
	DISPLAY=":$display_number" xdotool click 3
	sleep 0.2
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/context-open.png"
	DISPLAY=":$display_number" xdotool key Escape
	sleep 0.2
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/context-after.png"
	context_difference=$(compare -metric AE "$temporary/context-before.png" "$temporary/context-after.png" null: 2>&1 || true)
	if [ "$context_difference" != "0" ]; then
		echo "UI smoke test: context-menu close left a repaint difference ($context_difference)" >&2
		 exit 1
	fi
	# Repeat the repaint check at the lower edge. Closing the menu restores the
	# auto-revealed navigation panel, which exercises a different overlay path
	# than the center-of-window check above.
	DISPLAY=":$display_number" xdotool mousemove 640 790
	sleep 0.2
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/context-panel-before.png"
	DISPLAY=":$display_number" xdotool click 3
	sleep 0.2
	DISPLAY=":$display_number" xdotool key Escape
	sleep 0.2
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/context-panel-after.png"
	context_panel_difference=$(compare -metric AE "$temporary/context-panel-before.png" \
		"$temporary/context-panel-after.png" null: 2>&1 || true)
	if [ "$context_panel_difference" != "0" ]; then
		echo "UI smoke test: context-menu close damaged the revealed navigation panel ($context_panel_difference)" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool click 3
	sleep 0.2
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/context-compact.png"
	DISPLAY=":$display_number" xdotool key Down
	DISPLAY=":$display_number" xdotool key Down
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.2
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/context-advanced.png"
	advanced_difference=$(compare -metric AE "$temporary/context-compact.png" "$temporary/context-advanced.png" null: 2>&1 || true)
	if [ "$advanced_difference" = "0" ]; then
		echo "UI smoke test: Advanced Options did not expand the context menu" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool key Escape
fi

DISPLAY=":$display_number" xdotool key n
DISPLAY=":$display_number" xdotool key shift+n
DISPLAY=":$display_number" xdotool key F2
DISPLAY=":$display_number" xdotool key ctrl+t
DISPLAY=":$display_number" wmctrl -i -r "$window_id" -b add,maximized_vert,maximized_horz
sleep 0.5
if [ "$visual_assertions" -eq 1 ]; then
	window_state=$(DISPLAY=":$display_number" xprop -id "$window_id" _NET_WM_STATE 2>/dev/null || true)
	case "$window_state" in
		*MAXIMIZED_VERT*MAXIMIZED_HORZ*) ;;
		*) echo "UI smoke test: test window could not be maximized" >&2; exit 1 ;;
	esac
fi
sleep 0.2
stop_viewer

settings="$temporary/config/jpegview-linux/settings.conf"
grep -q '^scale_mode=fit_no_enlarge$' "$settings"
grep -q '^manual_zoom=1$' "$settings"
grep -q '^sort_mode=file_name$' "$settings"
grep -q '^sort_ascending=1$' "$settings"
grep -q '^show_filename=1$' "$settings"
grep -q '^info_visible=1$' "$settings"
grep -q '^thumbnail_panel_visible=1$' "$settings"
grep -q '^thumbnail_panel_width=240$' "$settings"
grep -q '^file_dialog_width=960$' "$settings"
grep -q '^file_dialog_height=685$' "$settings"
awk -F= '$1 == "file_dialog_preview_ratio" && $2 > 0.34 && $2 < 0.35 { found = 1 } END { exit !found }' "$settings"
grep -q '^maximized=1$' "$settings"

launch_viewer
if [ "$visual_assertions" -eq 1 ]; then
	window_state=$(DISPLAY=":$display_number" xprop -id "$window_id" _NET_WM_STATE 2>/dev/null || true)
	case "$window_state" in
		*MAXIMIZED_VERT*MAXIMIZED_HORZ*) ;;
		*) echo "UI smoke test: maximized window state was not restored at launch" >&2; exit 1 ;;
	esac
fi
DISPLAY=":$display_number" xdotool key Home
sleep 0.3
title_after_reload=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
case "$title_after_reload" in
	01-red.ppm\ *) ;;
	*) echo "UI smoke test: persisted filename ordering was not restored" >&2; exit 1 ;;
esac
thumbnail_window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
thumbnail_neighbor_y=$(((thumbnail_window_height + 163) / 2))
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 200 "$thumbnail_neighbor_y"
DISPLAY=":$display_number" xdotool click 1
sleep 0.3
title_after_restored_thumbnail_click=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
if [ "$title_after_reload" = "$title_after_restored_thumbnail_click" ]; then
	echo "UI smoke test: persisted thumbnail panel was not interactive after relaunch" >&2
	exit 1
fi
DISPLAY=":$display_number" xdotool key ctrl+o
sleep 0.3
if [ "$visual_assertions" -eq 1 ]; then
	reopened_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^WIDTH=//p')
	reopened_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
	reopened_dialog_x=$(((reopened_width - 960) / 2))
	reopened_dialog_y=$(((reopened_height - 685) / 2))
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 40 20
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/open-dialog-restored.png"
	restored_right_border=$(convert "$temporary/open-dialog-restored.png" \
		-format "%[hex:p{$((reopened_dialog_x + 959)),$((reopened_dialog_y + 10))}]" info:)
	restored_bottom_border=$(convert "$temporary/open-dialog-restored.png" \
		-format "%[hex:p{$((reopened_dialog_x + 10)),$((reopened_dialog_y + 684))}]" info:)
	if [ "${restored_right_border#BEBEBE}" = "$restored_right_border" ] || \
		[ "${restored_bottom_border#BEBEBE}" = "$restored_bottom_border" ]; then
		echo "UI smoke test: the open dialog's resized dimensions were not restored after relaunch" >&2
		exit 1
	fi
	restored_preview_left=$((reopened_dialog_x + 960 - 12 - 320))
	restored_preview_border=$(convert "$temporary/open-dialog-restored.png" \
		-format "%[hex:p{$restored_preview_left,$((reopened_dialog_y + 122))}]" info:)
	if [ "${restored_preview_border#4B4B4B}" = "$restored_preview_border" ]; then
		echo "UI smoke test: the open dialog's preview proportion was not restored after relaunch" >&2
		exit 1
	fi
fi
DISPLAY=":$display_number" xdotool key Escape
stop_viewer

if [ "$visual_assertions" -eq 1 ]; then
	# A narrower portrait image must repaint the side margins after a wide image
	# has occupied them.  Check the left pillarbox after a landscape/portrait
	# round trip, including the exact portrait -> landscape -> portrait sequence.
	mkdir -p "$temporary/aspect-images" "$temporary/aspect-config"
	convert -size 1600x900 xc:red "$temporary/aspect-images/01-landscape.png"
	convert -size 400x600 xc:blue "$temporary/aspect-images/02-portrait.png"
	DISPLAY=":$display_number" HOME="$temporary/home" \
		XDG_CONFIG_HOME="$temporary/aspect-config" "$BINARY" "$temporary/aspect-images" \
		>"$temporary/aspect-viewer.log" 2>&1 &
	viewer_pid=$!
	window_id=''
	for _ in $(seq 1 50); do
		window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null | head -1 || true)
		if [ -n "$window_id" ]; then break; fi
		sleep 0.1
	done
	if [ -z "$window_id" ]; then
		echo "UI smoke test: aspect-ratio repaint viewer did not appear" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	sleep 0.3
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/aspect-landscape.png"
	window_height=$(identify -format '%h' "$temporary/aspect-landscape.png")
	pillarbox_y=$((window_height / 2))
	landscape_pixel=$(convert "$temporary/aspect-landscape.png" \
		-format "%[pixel:p{100,$pillarbox_y}]" info:)
	if [ "$landscape_pixel" != "srgb(255,0,0)" ]; then
		echo "UI smoke test: aspect-ratio fixture did not cover the portrait side margin ($landscape_pixel)" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool key Right
	sleep 0.2
	DISPLAY=":$display_number" xdotool key Left
	sleep 0.2
	DISPLAY=":$display_number" xdotool key Right
	sleep 0.2
	portrait_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
	case "$portrait_title" in
		02-portrait.png\ *) ;;
		*) echo "UI smoke test: aspect-ratio navigation did not return to the portrait image" >&2; exit 1 ;;
	esac
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/aspect-portrait.png"
	window_height=$(identify -format '%h' "$temporary/aspect-portrait.png")
	pillarbox_y=$((window_height / 2))
	pillarbox_color=$(convert "$temporary/aspect-portrait.png" \
		-format "%[pixel:p{100,$pillarbox_y}]" info:)
	if [ "$pillarbox_color" != "srgb(18,18,18)" ]; then
		echo "UI smoke test: portrait navigation left stale landscape pixels in its side margin ($pillarbox_color)" >&2
		exit 1
	fi
	stop_viewer
fi

echo "UI smoke tests passed"

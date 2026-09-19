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
for command in import compare xprop; do
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
touch -t 202001010000.00 "$temporary/images/01-red.ppm" "$temporary/images/02-green.ppm" \
	"$temporary/images/03-blue.ppm" "$temporary/images/04-yellow.ppm" "$temporary/images/05-cyan.ppm"

help_text=$($BINARY --help)
case "$help_text" in
	*"mouse wheel up/down navigates previous/next"*"Ctrl+mouse wheel zooms"*) ;;
	*) echo "UI smoke test: --help does not describe wheel controls" >&2; exit 1 ;;
esac

Xvfb -displayfd 1 -screen 0 1280x800x24 >"$temporary/display" 2>"$temporary/xvfb.log" &
xvfb_pid=$!
display_number=''
for attempt in $(seq 1 50); do
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
	for attempt in $(seq 1 50); do
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

launch_viewer
if [ "$visual_assertions" -eq 1 ]; then
	window_icon=$(DISPLAY=":$display_number" xprop -id "$window_id" _NET_WM_ICON 2>/dev/null || true)
	case "$window_icon" in
		*"Icon (64 x 64):"*) ;;
		*) echo "UI smoke test: native window did not publish the embedded 64x64 icon" >&2; exit 1 ;;
	esac
fi
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

DISPLAY=":$display_number" xdotool keydown ctrl
DISPLAY=":$display_number" xdotool click 5
DISPLAY=":$display_number" xdotool keyup ctrl
sleep 0.3
title_after_ctrl_wheel=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
if [ "$title_after_hold" != "$title_after_ctrl_wheel" ]; then
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
fi
title_before_thumbnail_click=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
DISPLAY=":$display_number" xdotool mousemove 80 500
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
stop_viewer

echo "UI smoke tests passed"

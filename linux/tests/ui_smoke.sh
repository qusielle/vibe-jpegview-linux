#!/bin/sh
set -eu

CDPATH=
export CDPATH
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
BINARY=${1:-./build/jpegview-linux}
ARCHIVE_FIXTURE_WRITER=${2:-}
RAR_FIXTURE_WRITER=${3:-}
if [ ! -x "$BINARY" ]; then
	echo "UI smoke test: binary not found: $BINARY" >&2
	exit 2
fi
BINARY=$(cd -- "$(dirname -- "$BINARY")" && pwd)/$(basename -- "$BINARY")

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
XDG_STATE_HOME="$temporary/state"
export XDG_STATE_HOME
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
mkdir -p "$temporary/bin"
cat >"$temporary/bin/xdg-open" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$JPEGVIEW_TEST_URL_LOG"
EOF
chmod 755 "$temporary/bin/xdg-open"

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

write_solid_ppm() {
	filename=$1
	red=$2
	green=$3
	blue=$4
	{
		printf 'P3\n240 320\n255\n'
		yes "$red $green $blue" 2>/dev/null | head -n 76800
	} > "$filename"
}

write_ppm "$temporary/images/01-red.ppm" 255 0 0
write_ppm "$temporary/images/02-green.ppm" 0 255 0
write_ppm "$temporary/images/03-blue.ppm" 0 0 255
write_ppm "$temporary/images/04-yellow.ppm" 255 255 0
write_ppm "$temporary/images/05-cyan.ppm" 0 255 255
mkdir -p "$temporary/double-page-fixtures"
write_solid_ppm "$temporary/double-page-fixtures/00-cover.ppm" 35 75 220
write_solid_ppm "$temporary/double-page-fixtures/01-first.ppm" 30 220 60
write_solid_ppm "$temporary/double-page-fixtures/02-second.ppm" 220 40 30
write_solid_ppm "$temporary/double-page-fixtures/03-last.ppm" 225 200 25
mkdir -p "$temporary/deletion-preview"
write_solid_ppm "$temporary/deletion-preview/01-delete-preview.ppm" 255 0 255
if command -v zip >/dev/null 2>&1; then
	mkdir -p "$temporary/archive-source"
	write_ppm "$temporary/archive-source/inside-archive.ppm" 48 96 144
	(
		cd "$temporary/archive-source"
		zip -q "$temporary/images/06-archive.zip" inside-archive.ppm
	)
	touch -t 201801010000.00 "$temporary/images/06-archive.zip"
	cp "$temporary/images/06-archive.zip" "$temporary/images/14-comic.cbz"
	touch -t 201801010000.00 "$temporary/images/14-comic.cbz"
fi
if command -v tar >/dev/null 2>&1 && command -v gzip >/dev/null 2>&1; then
	mkdir -p "$temporary/tar-source"
	write_ppm "$temporary/tar-source/inside-tar.ppm" 48 96 144
	tar -cf "$temporary/images/07-archive.tar" -C "$temporary/tar-source" inside-tar.ppm
	tar -czf "$temporary/images/08-archive.tar.gz" -C "$temporary/tar-source" inside-tar.ppm
	touch -t 201801010000.00 "$temporary/images/07-archive.tar" "$temporary/images/08-archive.tar.gz"
fi
if [ -x "$ARCHIVE_FIXTURE_WRITER" ]; then
	"$ARCHIVE_FIXTURE_WRITER" "$temporary/images/09-archive.7z"
	touch -t 201801010000.00 "$temporary/images/09-archive.7z"
	cp "$temporary/images/09-archive.7z" "$temporary/images/15-comic.cb7"
	touch -t 201801010000.00 "$temporary/images/15-comic.cb7"
fi
if [ -x "$RAR_FIXTURE_WRITER" ]; then
	"$RAR_FIXTURE_WRITER" "$temporary/images/10-archive.rar" rar5-solid
	touch -t 201801010000.00 "$temporary/images/10-archive.rar"
fi
cp "$SCRIPT_DIR/fixtures/encrypted-zip.zip" "$temporary/images/11-password.zip"
touch -t 201801010000.00 "$temporary/images/11-password.zip"
sevenzip_plugin="$(dirname -- "$BINARY")/lib/jpegview-linux/7z.so"
if [ "${JPEGVIEW_TEST_HAS_7Z_PLUGIN:-0}" = 1 ] && [ -f "$sevenzip_plugin" ] &&
	[ -f "$SCRIPT_DIR/fixtures/header-encrypted.7z" ]; then
	cp "$SCRIPT_DIR/fixtures/header-encrypted.7z" \
		"$temporary/images/12-header-password.7z"
	touch -t 201801010000.00 "$temporary/images/12-header-password.7z"
fi
rar_plugin="$(dirname -- "$BINARY")/lib/jpegview-linux/librar_backend.so"
if [ "${JPEGVIEW_TEST_HAS_RAR_PLUGIN:-0}" = 1 ] && [ -f "$rar_plugin" ] &&
	[ -f "$SCRIPT_DIR/fixtures/encrypted_rar/rar4-header-encrypted.rar" ]; then
	cp "$SCRIPT_DIR/fixtures/encrypted_rar/rar4-header-encrypted.rar" \
		"$temporary/images/13-rar-header-password.rar"
	touch -t 201801010000.00 "$temporary/images/13-rar-header-password.rar"
fi
mkdir -p "$temporary/images/00-album/first-subdir" "$temporary/images/00-album/second-subdir"
write_ppm "$temporary/images/00-album/first.ppm" 128 64 32
write_ppm "$temporary/images/00-album/second.ppm" 32 64 128
touch -t 202001010000.00 "$temporary/images/00-album/first.ppm" "$temporary/images/00-album/second.ppm"
mkdir -p "$temporary/images/00-empty-startup"
touch -t 201701010000.00 "$temporary/images/00-empty-startup"
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
case "$help_text" in
	*"Ctrl+M marks an image"*"Ctrl+Left/Right toggles"*) ;;
	*) echo "UI smoke test: --help does not describe marked-image toggling" >&2; exit 1 ;;
esac
case "$help_text" in
	*"N/M/C select display order"*"Z toggles the magnifying glass"*) ;;
	*) echo "UI smoke test: --help does not describe the magnifying-glass shortcut" >&2; exit 1 ;;
esac
case "$help_text" in
	*"D toggles double-page mode"*"J reverses manga reading order"*) ;;
	*) echo "UI smoke test: --help does not describe the double-page shortcuts" >&2; exit 1 ;;
esac
case "$help_text" in
	*"spacebar_navigates_images=1 maps Space/Shift+Space to next/previous"*) ;;
	*) echo "UI smoke test: --help does not describe the configurable Space navigation keys" >&2; exit 1 ;;
esac
case "$help_text" in
	*"folder_wrap_around=0 disables F7/list wrapping"*) ;;
	*) echo "UI smoke test: --help does not describe the folder-wrap setting" >&2; exit 1 ;;
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
	viewer_input=${1:-$temporary/images}
	viewer_home=${VIEWER_TEST_HOME:-$temporary/home}
	viewer_config=${VIEWER_TEST_CONFIG_HOME:-$temporary/config}
	DISPLAY=":$display_number" HOME="$viewer_home" XDG_CONFIG_HOME="$viewer_config" \
		XDG_STATE_HOME="$XDG_STATE_HOME" \
		PATH="$temporary/bin:$PATH" JPEGVIEW_TEST_URL_LOG="$temporary/opened-url" \
		"$BINARY" "$viewer_input" >"$temporary/viewer.log" 2>&1 &
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

assert_title_prefix() {
	expected_prefix=$1
	failure_message=$2
	current_title=''
	for _ in $(seq 1 40); do
		current_title=$(DISPLAY=":$display_number" command xdotool getwindowname "$window_id")
		case "$current_title" in
			"$expected_prefix"*) return 0 ;;
			\[*/*\]*)
				title_without_position=${current_title#*] }
				case "$title_without_position" in
					"$expected_prefix"*) return 0 ;;
				esac
				;;
		esac
		sleep 0.05
	done
	echo "UI smoke test: $failure_message ($current_title)" >&2
	exit 1
}

window_title_without_position() {
	DISPLAY=":$display_number" command xdotool getwindowname "$window_id" |
		sed -E 's/^\[[0-9]+(-[0-9]+)?\/[0-9]+\] //'
}

assert_window_title_exact() {
	expected_title=$1
	failure_message=$2
	current_title=''
	for _ in $(seq 1 60); do
		current_title=$(DISPLAY=":$display_number" window_title_without_position)
		if [ "$current_title" = "$expected_title" ]; then return 0; fi
		sleep 0.05
	done
	echo "UI smoke test: $failure_message ($current_title)" >&2
	exit 1
}

set_clipboard_text() {
	printf '%s' "$1" | DISPLAY=":$display_number" xclip -selection clipboard -i
	sleep 0.1
}

clear_clipboard_text() {
	: | DISPLAY=":$display_number" xclip -selection clipboard -i
	sleep 0.1
}

# Two adjacent portrait pages should render together after the standalone
# cover, navigation should step over the partner, and J should reverse both
# the physical page placement and left/right reading direction.
mkdir -p "$temporary/double-page-config/jpegview-linux"
printf 'scale_mode=fit\ndouble_page_mode_enabled=0\nmanga_reading_order_enabled=0\n' \
	> "$temporary/double-page-config/jpegview-linux/settings.conf"
XDG_STATE_HOME="$temporary/double-page-state"
export XDG_STATE_HOME
VIEWER_TEST_HOME="$temporary/double-page-home" \
	VIEWER_TEST_CONFIG_HOME="$temporary/double-page-config" \
	launch_viewer "$temporary/double-page-fixtures"
assert_title_prefix "00-cover.ppm" "double-page fixture did not start on its standalone cover"
assert_title_prefix "[1/4] " "window title did not show the initial image position"
DISPLAY=":$display_number" xdotool key d
sleep 0.15
DISPLAY=":$display_number" xdotool key Right
assert_title_prefix "01-first.ppm" "double-page mode did not advance from the single cover"
assert_title_prefix "[2-3/4] " "window title did not show both positions in the active double-page spread"
if [ "$visual_assertions" -eq 1 ]; then
	spread_rendered=0
	for _ in $(seq 1 40); do
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/spread.png"
		spread_left=$(convert "$temporary/spread.png" -format '%[hex:p{320,400}]' info:)
		spread_right=$(convert "$temporary/spread.png" -format '%[hex:p{960,400}]' info:)
		case "$spread_left:$spread_right" in
			*1EDC3C*:*DC281E*) spread_rendered=1; break ;;
		esac
		sleep 0.05
	done
	if [ "$spread_rendered" -ne 1 ]; then
		echo "UI smoke test: double-page mode did not render the adjacent portrait pages side by side" >&2
		exit 1
	fi
fi
DISPLAY=":$display_number" xdotool key Right
sleep 0.15
assert_title_prefix "03-last.ppm" "double-page navigation did not skip the displayed partner"
DISPLAY=":$display_number" xdotool key Left
sleep 0.15
assert_title_prefix "01-first.ppm" "backward double-page navigation did not return to the preceding spread"
DISPLAY=":$display_number" xdotool key j
sleep 0.15
if [ "$visual_assertions" -eq 1 ]; then
	spread_reversed=0
	for _ in $(seq 1 40); do
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/spread.png"
		spread_left=$(convert "$temporary/spread.png" -format '%[hex:p{320,400}]' info:)
		spread_right=$(convert "$temporary/spread.png" -format '%[hex:p{960,400}]' info:)
		case "$spread_left:$spread_right" in
			*DC281E*:*1EDC3C*) spread_reversed=1; break ;;
		esac
		sleep 0.05
	done
	if [ "$spread_reversed" -ne 1 ]; then
		echo "UI smoke test: manga mode did not swap the spread pages" >&2
		exit 1
	fi
fi
DISPLAY=":$display_number" xdotool key Left
sleep 0.15
assert_title_prefix "03-last.ppm" "manga reading order did not make Left advance past the spread partner"
DISPLAY=":$display_number" xdotool key Right
sleep 0.15
assert_title_prefix "01-first.ppm" "manga reading order did not make Right return to the preceding spread"
if [ "$visual_assertions" -eq 1 ]; then
	DISPLAY=":$display_number" xdotool key ctrl+t
	sleep 0.2
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/spread-thumbnails.png"
	thumbnail_capture_height=$(identify -format '%h' "$temporary/spread-thumbnails.png")
	thumbnail_row_height=112
	thumbnail_current_y=$(((thumbnail_capture_height - thumbnail_row_height) / 2 + thumbnail_row_height / 2))
	thumbnail_partner_y=$((thumbnail_current_y + thumbnail_row_height))
	thumbnail_following_y=$((thumbnail_partner_y + thumbnail_row_height))
	current_thumbnail_background=$(convert "$temporary/spread-thumbnails.png" \
		-format "%[pixel:p{5,$thumbnail_current_y}]" info:)
	partner_thumbnail_background=$(convert "$temporary/spread-thumbnails.png" \
		-format "%[pixel:p{5,$thumbnail_partner_y}]" info:)
	following_thumbnail_background=$(convert "$temporary/spread-thumbnails.png" \
		-format "%[pixel:p{5,$thumbnail_following_y}]" info:)
	if [ "$current_thumbnail_background" != 'srgb(32,58,82)' ] || \
		[ "$partner_thumbnail_background" != 'srgb(32,58,82)' ] || \
		[ "$following_thumbnail_background" = 'srgb(32,58,82)' ]; then
		echo "UI smoke test: thumbnail panel did not highlight exactly the active spread pair" >&2
		exit 1
	fi
fi
stop_viewer

# A reopened file must recover its own two mode flags even when the current
# global defaults differ; navigating from Recents exposes both settings.
double_settings="$temporary/double-page-config/jpegview-linux/settings.conf"
sed -i 's/^double_page_mode_enabled=.*/double_page_mode_enabled=0/; s/^manga_reading_order_enabled=.*/manga_reading_order_enabled=0/; s/^thumbnail_panel_visible=.*/thumbnail_panel_visible=0/' \
	"$double_settings"
VIEWER_TEST_HOME="$temporary/recents-home" \
	VIEWER_TEST_CONFIG_HOME="$temporary/double-page-config" \
	launch_viewer "$temporary/images/01-red.ppm"
DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool key ctrl+Tab
DISPLAY=":$display_number" xdotool key Down
DISPLAY=":$display_number" xdotool key Return
assert_title_prefix "01-first.ppm" "Recents did not reopen the double-page fixture image"
if [ "$visual_assertions" -eq 1 ]; then
	recent_spread_rendered=0
	for _ in $(seq 1 40); do
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/recent-spread.png"
		recent_spread_left=$(convert "$temporary/recent-spread.png" -format '%[hex:p{320,400}]' info:)
		recent_spread_right=$(convert "$temporary/recent-spread.png" -format '%[hex:p{960,400}]' info:)
		case "$recent_spread_left:$recent_spread_right" in
			*DC281E*:*1EDC3C*) recent_spread_rendered=1; break ;;
		esac
		sleep 0.05
	done
	if [ "$recent_spread_rendered" -ne 1 ]; then
		echo "UI smoke test: Recents did not restore the complete double-page spread before navigation ($recent_spread_left:$recent_spread_right)" >&2
		exit 1
	fi
else
	sleep 0.15
fi
DISPLAY=":$display_number" xdotool key Left
assert_title_prefix "03-last.ppm" "Recents did not restore double-page and manga mode for the selected image"
DISPLAY=":$display_number" xdotool key Right
assert_title_prefix "01-first.ppm" "restored manga mode did not persist while navigating after opening from Recents"
stop_viewer

# The config option disables manga-mode key inversion while leaving page
# placement and spread navigation in manga order.
printf 'scale_mode=fit\ndouble_page_mode_enabled=1\nmanga_reading_order_enabled=1\nmanga_mode_inverts_left_right=0\n' \
	> "$double_settings"
VIEWER_TEST_HOME="$temporary/manga-no-key-inversion-home" \
	VIEWER_TEST_CONFIG_HOME="$temporary/double-page-config" \
	launch_viewer "$temporary/double-page-fixtures/01-first.ppm"
DISPLAY=":$display_number" xdotool key Left
assert_title_prefix "00-cover.ppm" "configured manga mode still inverted the Left key"
DISPLAY=":$display_number" xdotool key Right
assert_title_prefix "01-first.ppm" "configured manga mode still inverted the Right key"
stop_viewer

# Space navigation is opt-in and Shift+Space selects the previous image.
printf 'scale_mode=fit\ndouble_page_mode_enabled=0\nmanga_reading_order_enabled=0\nspacebar_navigates_images=1\n' \
	> "$double_settings"
XDG_STATE_HOME="$temporary/space-navigation-state"
export XDG_STATE_HOME
VIEWER_TEST_HOME="$temporary/space-navigation-home" \
	VIEWER_TEST_CONFIG_HOME="$temporary/double-page-config" \
	launch_viewer "$temporary/double-page-fixtures/00-cover.ppm"
DISPLAY=":$display_number" xdotool key space
assert_title_prefix "01-first.ppm" "configured Space key did not navigate to the next image"
DISPLAY=":$display_number" xdotool key shift+space
assert_title_prefix "00-cover.ppm" "configured Shift+Space key did not navigate to the previous image"
stop_viewer
XDG_STATE_HOME="$temporary/state"
export XDG_STATE_HOME

# Starting with a directory that has no direct images should leave the viewer
# open in Browse at that directory, rather than exiting or falling back to cwd.
DISPLAY=":$display_number" HOME="$temporary/empty-startup-home" \
	XDG_CONFIG_HOME="$temporary/empty-startup-config" \
	XDG_STATE_HOME="$temporary/empty-startup-state" \
	"$BINARY" "$temporary/images/00-empty-startup" \
	>"$temporary/empty-startup-viewer.log" 2>&1 &
viewer_pid=$!
window_id=''
for _ in $(seq 1 50); do
	window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible --class jpegview-linux 2>/dev/null | head -1 || true)
	if [ -n "$window_id" ]; then break; fi
	sleep 0.1
done
if [ -z "$window_id" ]; then
	echo "UI smoke test: empty-directory startup did not leave a visible viewer window" >&2
	exit 1
fi
DISPLAY=":$display_number" xdotool windowactivate "$window_id"
assert_window_title_exact "JPEGView" "empty-directory startup did not finish scanning into the Browse dialog"
DISPLAY=":$display_number" xdotool key BackSpace
DISPLAY=":$display_number" xdotool type --delay 20 '00-empty-startup'
DISPLAY=":$display_number" xdotool key Return
DISPLAY=":$display_number" xdotool key BackSpace
DISPLAY=":$display_number" xdotool type --delay 20 '01-RED'
DISPLAY=":$display_number" xdotool key Return
assert_title_prefix "01-red.ppm" "empty-directory startup did not browse from the invoked directory"
stop_viewer

# Starting with no positional arguments should browse the current working
# directory and let the user select an image instead of exiting.
(
	cd "$temporary/images"
	DISPLAY=":$display_number" HOME="$temporary/no-argument-home" \
		XDG_CONFIG_HOME="$temporary/no-argument-config" \
		XDG_STATE_HOME="$temporary/no-argument-state" \
		"$BINARY" >"$temporary/no-argument-viewer.log" 2>&1
) &
viewer_pid=$!
window_id=''
for _ in $(seq 1 50); do
	window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible --class jpegview-linux 2>/dev/null | head -1 || true)
	if [ -n "$window_id" ]; then break; fi
	sleep 0.1
done
if [ -z "$window_id" ]; then
	echo "UI smoke test: no-argument startup did not leave a visible viewer window" >&2
	exit 1
fi
DISPLAY=":$display_number" xdotool windowactivate "$window_id"
assert_title_prefix "JPEGView" "no-argument startup did not open Browse"
DISPLAY=":$display_number" xdotool type --delay 20 '01-RED'
DISPLAY=":$display_number" xdotool key Return
assert_title_prefix "01-red.ppm" "no-argument startup did not browse the current working directory"
stop_viewer

# Delete confirmation should identify the target visually without doing a new
# image decode; Escape must leave the source file untouched.
VIEWER_TEST_HOME="$temporary/delete-preview-home" \
	VIEWER_TEST_CONFIG_HOME="$temporary/delete-preview-config" \
	launch_viewer "$temporary/deletion-preview/01-delete-preview.ppm"
assert_title_prefix "01-delete-preview.ppm" "delete-preview fixture did not open"
DISPLAY=":$display_number" xdotool key Delete
sleep 0.3
if [ "$visual_assertions" -eq 1 ]; then
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/delete-confirmation.png"
	viewer_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" |
		sed -n 's/^WIDTH=//p')
	viewer_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" |
		sed -n 's/^HEIGHT=//p')
	confirmation_width=$((viewer_width - 40))
	if [ "$confirmation_width" -gt 760 ]; then confirmation_width=760; fi
	if [ "$confirmation_width" -lt 360 ]; then confirmation_width=360; fi
	preview_pixel_x=$(((viewer_width - confirmation_width) / 2 + 18 + 56))
	preview_pixel_y=$(((viewer_height - 180) / 2 + 64 + 36))
	preview_pixel=$(convert "$temporary/delete-confirmation.png" \
		-format "%[hex:p{$preview_pixel_x,$preview_pixel_y}]" info: | tr 'A-F' 'a-f')
	case "$preview_pixel" in
		ff00ff*) ;;
		*) echo "UI smoke test: delete confirmation did not show the cached image preview ($preview_pixel)" >&2; exit 1 ;;
	esac
fi
DISPLAY=":$display_number" xdotool key Escape
assert_title_prefix "01-delete-preview.ppm" "Escape did not cancel delete confirmation"
if [ ! -f "$temporary/deletion-preview/01-delete-preview.ppm" ]; then
	echo "UI smoke test: canceling delete confirmation removed the source image" >&2
	exit 1
fi
stop_viewer

if command -v cc >/dev/null 2>&1 && command -v convert >/dev/null 2>&1; then
	# Delay the decoder's memory map of a known JPEG. The final viewer window
	# must already be mapped and painted while that initial load is blocked.
	cc -shared -fPIC "$SCRIPT_DIR/delay_mmap.c" -o "$temporary/slow_map.so" -ldl
	convert "$temporary/images/01-red.ppm" "$temporary/startup-delay.jpg"
	DISPLAY=":$display_number" HOME="$temporary/home" XDG_CONFIG_HOME="$temporary/startup-config" \
		LD_PRELOAD="$temporary/slow_map.so" \
		JPEGVIEW_TEST_SLOW_MAP="$temporary/startup-delay.jpg" \
		"$BINARY" "$temporary/startup-delay.jpg" >"$temporary/startup-viewer.log" 2>&1 &
	viewer_pid=$!
	window_id=''
	for _ in $(seq 1 20); do
		window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null | head -1 || true)
		if [ -n "$window_id" ]; then break; fi
		sleep 0.1
	done
	if [ -z "$window_id" ]; then
		echo "UI smoke test: startup window waited for initial image decoding" >&2
		exit 1
	fi
	startup_title=$(DISPLAY=":$display_number" window_title_without_position)
	case "$startup_title" in
		*Loading*) ;;
		*) echo "UI smoke test: startup window did not show loading state" >&2; exit 1 ;;
	esac
	loaded_title=''
	for _ in $(seq 1 50); do
		loaded_title=$(DISPLAY=":$display_number" window_title_without_position)
		case "$loaded_title" in
			startup-delay.jpg\ *) break ;;
		esac
		sleep 0.1
	done
	case "$loaded_title" in
		startup-delay.jpg\ *) ;;
		*) echo "UI smoke test: delayed startup image never completed" >&2; exit 1 ;;
	esac
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	stop_viewer
fi

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

click_file_dialog_blank_space() {
	window_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^WIDTH=//p')
	window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
	dialog_width=$((window_width - 40))
	if [ "$dialog_width" -gt 900 ]; then dialog_width=900; fi
	if [ "$dialog_width" -lt 320 ]; then dialog_width=320; fi
	dialog_height=$((window_height - 40))
	if [ "$dialog_height" -gt 650 ]; then dialog_height=650; fi
	if [ "$dialog_height" -lt 260 ]; then dialog_height=260; fi
	dialog_x=$(((window_width - dialog_width) / 2))
	dialog_y=$(((window_height - dialog_height) / 2))
	blank_x=$((dialog_x + dialog_width / 2))
	blank_y=$((dialog_y + 22))
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" "$blank_x" "$blank_y" click 1
}

click_file_dialog_tab() {
	tab=$1
	requested_width=${2:-900}
	requested_height=${3:-650}
	window_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^WIDTH=//p')
	window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
	dialog_width=$requested_width
	if [ "$dialog_width" -gt $((window_width - 40)) ]; then dialog_width=$((window_width - 40)); fi
	dialog_height=$requested_height
	if [ "$dialog_height" -gt $((window_height - 40)) ]; then dialog_height=$((window_height - 40)); fi
	dialog_x=$(((window_width - dialog_width) / 2))
	dialog_y=$(((window_height - dialog_height) / 2))
	if [ "$tab" = browse ]; then
		tab_x=$((dialog_x + dialog_width - 150 + 31))
	else
		tab_x=$((dialog_x + dialog_width - 82 + 35))
	fi
	tab_y=$((dialog_y + 22))
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" "$tab_x" "$tab_y" click 1
}

if [ -f "$temporary/images/11-password.zip" ]; then
	# A directly opened encrypted archive should fall back to Browse instead of
	# exiting on its first locked image. Exercise wrong-password retry and ensure
	# the accepted credential is reused for another open in the same run.
	XDG_STATE_HOME="$temporary/direct-password-state"
	export XDG_STATE_HOME
	launch_viewer "$temporary/images/11-password.zip"
	assert_title_prefix "JPEGView — Open image" "encrypted archive startup did not open Browse"
	sleep 0.3
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/password-dialog.png"
		password_border=$(convert "$temporary/password-dialog.png" \
			-format "%[hex:p{400,311}]" info:)
		case "$password_border" in
			BEAA78*|beaa78*) ;;
			*) echo "UI smoke test: encrypted archive startup did not show the password prompt" >&2; exit 1 ;;
		esac
	fi
	DISPLAY=":$display_number" xdotool type --delay 20 'wrong-password'
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.3
	if command -v xclip >/dev/null 2>&1; then
		set_clipboard_text 'jpegview-test-password'
		DISPLAY=":$display_number" xdotool key ctrl+v
	else
		DISPLAY=":$display_number" xdotool type --delay 20 'jpegview-test-password'
	fi
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.2
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "inside-password.ppm" "correct ZIP password did not open the encrypted image"
	DISPLAY=":$display_number" xdotool key ctrl+o
	sleep 0.3
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/password-reopen.png"
		password_reopen_border=$(convert "$temporary/password-reopen.png" \
			-format "%[hex:p{400,311}]" info:)
		case "$password_reopen_border" in
			BEAA78*|beaa78*) echo "UI smoke test: cached ZIP password was requested again" >&2; exit 1 ;;
		esac
	fi
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "inside-password.ppm" "reopening an encrypted ZIP asked again for its session password"
	stop_viewer
	if command -v xclip >/dev/null 2>&1; then
		for shortcut in ctrl+shift+v shift+Insert; do
			launch_viewer "$temporary/images/11-password.zip"
			set_clipboard_text 'jpegview-test-password'
			DISPLAY=":$display_number" xdotool key "$shortcut"
			DISPLAY=":$display_number" xdotool key Return
			sleep 0.2
			DISPLAY=":$display_number" xdotool key Return
			assert_title_prefix "inside-password.ppm" \
				"encrypted ZIP password paste shortcut '$shortcut' did not unlock the image"
			stop_viewer
		done
	else
		echo "UI smoke test: SKIP password clipboard shortcuts (missing xclip)"
	fi
	if command -v xclip >/dev/null 2>&1; then clear_clipboard_text; fi
	XDG_STATE_HOME="$temporary/state"
	export XDG_STATE_HOME
fi

if [ -f "$temporary/images/12-header-password.7z" ]; then
	# Header-encrypted 7z must not expose names before the shared password dialog
	# accepts a credential. This runs only when the optional Format7zF plugin is
	# beside the executable; normal local fallback builds still test core behavior.
	XDG_STATE_HOME="$temporary/header-password-state"
	export XDG_STATE_HOME
	launch_viewer "$temporary/images/12-header-password.7z"
	assert_title_prefix "JPEGView" \
		"header-encrypted 7z startup did not open Browse"
	sleep 0.3
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/header-password-dialog.png"
		password_border=$(convert "$temporary/header-password-dialog.png" \
			-format "%[hex:p{400,311}]" info:)
		case "$password_border" in
			BEAA78*|beaa78*) ;;
			*) echo "UI smoke test: header-encrypted 7z did not show the password prompt" >&2; exit 1 ;;
		esac
	fi
	DISPLAY=":$display_number" xdotool type --delay 20 'wrong-password'
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.3
	DISPLAY=":$display_number" xdotool type --delay 20 'test-secret'
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.2
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "visible.png" \
		"header-encrypted 7z did not reveal/open its image after password entry"
	stop_viewer
	XDG_STATE_HOME="$temporary/state"
	export XDG_STATE_HOME
	# Reselecting a marked container after canceling must retry against that
	# archive, not the filesystem folder currently containing it.
	XDG_STATE_HOME="$temporary/header-password-reselect-state"
	export XDG_STATE_HOME
	launch_viewer
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool type --delay 20 '12-header-password.7z'
	sleep 1.5
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.3
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/header-password-first-attempt.png"
		password_border=$(convert "$temporary/header-password-first-attempt.png" \
			-format "%[hex:p{400,311}]" info:)
		case "$password_border" in
			BEAA78*|beaa78*) ;;
			*) echo "UI smoke test: selecting a marked 7z row did not show its password prompt" >&2; exit 1 ;;
		esac
	fi
	DISPLAY=":$display_number" xdotool key Escape
	sleep 0.2
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.3
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/header-password-retry.png"
		password_border=$(convert "$temporary/header-password-retry.png" \
			-format "%[hex:p{400,311}]" info:)
		case "$password_border" in
			BEAA78*|beaa78*) ;;
			*) echo "UI smoke test: reselecting the canceled 7z row did not show a new password prompt" >&2; exit 1 ;;
		esac
	fi
	DISPLAY=":$display_number" xdotool type --delay 20 'test-secret'
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.6
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.3
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.3
	assert_title_prefix "visible.png" \
		"reselecting a canceled encrypted 7z prompt used the parent-folder path"
	stop_viewer
	XDG_STATE_HOME="$temporary/state"
	export XDG_STATE_HOME
fi

if [ -f "$temporary/images/13-rar-header-password.rar" ]; then
	# Header-encrypted RAR uses the same explicit-unlock flow as 7z. The wrong
	# legacy-header password must remain retryable, then reveal the image.
	XDG_STATE_HOME="$temporary/rar-header-password-state"
	export XDG_STATE_HOME
	launch_viewer "$temporary/images/13-rar-header-password.rar"
	assert_title_prefix "JPEGView" "header-encrypted RAR startup did not open Browse"
	sleep 0.3
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/rar-header-password-dialog.png"
		password_border=$(convert "$temporary/rar-header-password-dialog.png" \
			-format "%[hex:p{400,311}]" info:)
		case "$password_border" in
			BEAA78*|beaa78*) ;;
			*) echo "UI smoke test: header-encrypted RAR did not show the password prompt" >&2; exit 1 ;;
		esac
	fi
	DISPLAY=":$display_number" xdotool type --delay 20 'wrong-password'
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.3
	DISPLAY=":$display_number" xdotool type --delay 20 'test-secret'
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.2
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "private.png" \
		"header-encrypted RAR did not reveal/open its image after password entry"
	stop_viewer
	XDG_STATE_HOME="$temporary/state"
	export XDG_STATE_HOME
fi

# Exercise the mnemonic in an isolated viewer session so the rest of this
# smoke suite starts from the original first image and an empty recent history.
XDG_STATE_HOME="$temporary/mnemonic-state"
export XDG_STATE_HOME
launch_viewer
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 400 click 3
DISPLAY=":$display_number" xdotool key --clearmodifiers o
sleep 0.15
DISPLAY=":$display_number" xdotool type --delay 20 '00-entry-test'
DISPLAY=":$display_number" xdotool key Return
sleep 0.15
DISPLAY=":$display_number" xdotool key Return
assert_title_prefix "inside-first.ppm" "context-menu O mnemonic did not open the matching image"
stop_viewer
XDG_STATE_HOME="$temporary/state"
export XDG_STATE_HOME

# Advanced configuration is available as the last compact-menu command. The
# dialog stages values by category and persists only after Apply.
XDG_STATE_HOME="$temporary/advanced-config-state"
VIEWER_TEST_CONFIG_HOME="$temporary/advanced-config-config"
export XDG_STATE_HOME VIEWER_TEST_CONFIG_HOME
launch_viewer "$temporary/images/01-red.ppm"
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 400 click 3
DISPLAY=":$display_number" xdotool key End
DISPLAY=":$display_number" xdotool key Return
sleep 0.15
DISPLAY=":$display_number" xdotool key Down Down Return
DISPLAY=":$display_number" xdotool key Tab
DISPLAY=":$display_number" xdotool key Return
DISPLAY=":$display_number" xdotool key Tab
DISPLAY=":$display_number" xdotool key Return
DISPLAY=":$display_number" xdotool type --delay 30 '128'
DISPLAY=":$display_number" xdotool key Tab
DISPLAY=":$display_number" xdotool key ctrl+Return
sleep 0.2
advanced_config_settings="$VIEWER_TEST_CONFIG_HOME/jpegview-linux/settings.conf"
grep -q '^transparency_pattern=white$' "$advanced_config_settings"
grep -q '^thumbnail_panel_width=128$' "$advanced_config_settings"
grep -q '^folder_wrap_around=0$' "$advanced_config_settings"
assert_title_prefix "01-red.ppm" "applying advanced configuration did not return to the viewer"
DISPLAY=":$display_number" xdotool key Left
sleep 0.1
assert_title_prefix "01-red.ppm" "disabled folder wrap moved past the first image"
DISPLAY=":$display_number" xdotool key Right
sleep 0.1
assert_title_prefix "02-green.ppm" "folder navigation did not continue from an interior image"

# Reopening and escaping discards an un-applied draft.
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 400 click 3
DISPLAY=":$display_number" xdotool key End
DISPLAY=":$display_number" xdotool key Return
sleep 0.1
DISPLAY=":$display_number" xdotool key Tab
DISPLAY=":$display_number" xdotool key Return
DISPLAY=":$display_number" xdotool key Escape
sleep 0.1
grep -q '^transparency_pattern=white$' "$advanced_config_settings"
grep -q '^folder_wrap_around=0$' "$advanced_config_settings"
stop_viewer
unset VIEWER_TEST_CONFIG_HOME
XDG_STATE_HOME="$temporary/state"
export XDG_STATE_HOME

launch_viewer
help_previous_title=$(DISPLAY=":$display_number" window_title_without_position)
DISPLAY=":$display_number" xdotool windowfocus --sync "$window_id"
DISPLAY=":$display_number" xdotool key --clearmodifiers F1
sleep 0.2
help_title=$(DISPLAY=":$display_number" window_title_without_position)
case "$help_title" in
	*Help*) ;;
	*) echo "UI smoke test: F1 did not open the quick-help panel (title: $help_title)" >&2; exit 1 ;;
esac
if [ "$visual_assertions" -eq 1 ]; then
	window_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^WIDTH=//p')
	window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
	help_panel_width=$((window_width - 40))
	if [ "$help_panel_width" -gt 940 ]; then help_panel_width=940; fi
	if [ "$help_panel_width" -lt 480 ]; then help_panel_width=480; fi
	help_panel_height=$((window_height - 40))
	if [ "$help_panel_height" -gt 360 ]; then help_panel_height=360; fi
	if [ "$help_panel_height" -lt 300 ]; then help_panel_height=300; fi
	help_panel_x=$(((window_width - help_panel_width) / 2))
	help_panel_y=$(((window_height - help_panel_height) / 2))
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/help-open.png"
	help_border=$(convert "$temporary/help-open.png" \
		-format "%[hex:p{$help_panel_x,$help_panel_y}]" info:)
	case "$help_border" in
		A0BEE1*) ;;
		*) echo "UI smoke test: F1 title changed but the quick-help panel was not rendered" >&2; exit 1 ;;
	esac
fi
DISPLAY=":$display_number" xdotool key Escape
sleep 0.2
help_closed_title=$(DISPLAY=":$display_number" window_title_without_position)
if [ "$help_closed_title" != "$help_previous_title" ]; then
	echo "UI smoke test: Escape did not close quick help and restore the image title" >&2
	exit 1
fi

# The About dialog should describe the project as a port and expose a working
# link through the desktop URL opener rather than listing generic features.
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 400 click 3
sleep 0.1
DISPLAY=":$display_number" xdotool key End
DISPLAY=":$display_number" xdotool key Up
DISPLAY=":$display_number" xdotool key Up
DISPLAY=":$display_number" xdotool key Return
assert_title_prefix "About JPEGView Linux" "context-menu navigation did not open About"
about_window_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^WIDTH=//p')
about_window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
about_panel_width=$((about_window_width - 40))
if [ "$about_panel_width" -gt 620 ]; then about_panel_width=620; fi
if [ "$about_panel_width" -lt 360 ]; then about_panel_width=360; fi
about_link_x=$(((about_window_width - about_panel_width) / 2 + 30))
about_link_y=$(((about_window_height - 196) / 2 + 120))
if [ "$visual_assertions" -eq 1 ]; then
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/about-open.png"
	about_link_color=$(convert "$temporary/about-open.png" \
		-crop "$((about_panel_width - 36))x32+$((about_link_x - 12))+$((about_link_y - 12))" \
		+repage -format %c histogram:info:- | grep -c 'srgb(125,185,255)' || true)
	if [ "$about_link_color" -eq 0 ]; then
		echo "UI smoke test: About did not render the repository URL as a blue underlined link" >&2
		exit 1
	fi
fi
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
	"$about_link_x" "$about_link_y" click 1
for _ in $(seq 1 30); do
	[ -s "$temporary/opened-url" ] && break
	sleep 0.1
done
opened_url=$(sed -n '1p' "$temporary/opened-url" 2>/dev/null || true)
if [ "$opened_url" != "https://github.com/qusielle/vibe-jpegview-linux" ]; then
	echo "UI smoke test: clicking the About repository link did not open the project URL ($opened_url)" >&2
	exit 1
fi
assert_title_prefix "About JPEGView Linux" "opening the project link unexpectedly closed About"
DISPLAY=":$display_number" xdotool key Escape
assert_title_prefix "01-red.ppm" "Escape did not close About and restore the image title"

# Ctrl+M marks one image. Ctrl+Left/Right then alternate between it and the
# image that was current at the first toggle.
DISPLAY=":$display_number" xdotool key ctrl+m
DISPLAY=":$display_number" xdotool key Right
assert_title_prefix "02-green.ppm" "mark/toggle fixture did not reach the second image"
DISPLAY=":$display_number" xdotool key ctrl+Left
assert_title_prefix "01-red.ppm" "Ctrl+Left did not return to the marked image"
DISPLAY=":$display_number" xdotool key ctrl+Right
assert_title_prefix "02-green.ppm" "Ctrl+Right did not toggle back to the paired image"
DISPLAY=":$display_number" xdotool key Left
assert_title_prefix "01-red.ppm" "mark/toggle smoke test did not restore its starting image"

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
click_file_dialog_tab recents
click_file_dialog_tab browse
click_file_dialog_blank_space
DISPLAY=":$display_number" xdotool key Return
sleep 0.4
filtered_title=$(DISPLAY=":$display_number" window_title_without_position)
case "$filtered_title" in
	03-blue.ppm*) ;;
	*) echo "UI smoke test: Ctrl+O filename filter did not open the matching image" >&2; exit 1 ;;
esac

if [ -f "$temporary/images/06-archive.zip" ]; then
	# ZIP containers behave like directories in Browse and archive members remain
	# openable from the Recents tab after they become the current image.
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool type --delay 20 '06-archive.zip'
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.2
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "inside-archive.ppm" "open dialog did not enter a ZIP and open its image member"
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool key ctrl+Tab
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "inside-archive.ppm" "Recents did not reopen an image stored inside a ZIP"
	# Return the smoke suite to its normal filesystem directory before its
	# existing sorting and directory-entry assertions.
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool key BackSpace
	DISPLAY=":$display_number" xdotool type --delay 20 '03-BLUE'
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "03-blue.ppm" "open dialog could not leave the archive and return to a filesystem image"
fi

if [ -f "$temporary/images/14-comic.cbz" ]; then
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool type --delay 20 '14-comic.cbz'
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.2
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "inside-archive.ppm" "open dialog did not browse and open a CBZ image member"
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool key BackSpace
	DISPLAY=":$display_number" xdotool type --delay 20 '03-BLUE'
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "03-blue.ppm" "open dialog could not leave a CBZ and return to a filesystem image"
fi

if [ -f "$temporary/images/08-archive.tar.gz" ]; then
	# Compressed TAR catalogs are prepared in the background; wait for the
	# virtual directory result before opening its first image.
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool type --delay 20 '08-archive.tar.gz'
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.2
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "inside-tar.ppm" "open dialog did not browse and open a TGZ image member"
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool key BackSpace
	DISPLAY=":$display_number" xdotool type --delay 20 '03-BLUE'
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "03-blue.ppm" "open dialog could not leave a TGZ and return to a filesystem image"
fi

if [ -f "$temporary/images/09-archive.7z" ]; then
	# Unencrypted 7z catalogs use seekable libarchive input and publish through
	# the same cancellable Open-dialog worker and archive-member preview path.
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool type --delay 20 '09-archive.7z'
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.2
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "inside-7z.ppm" "open dialog did not browse and open a 7z image member"
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool key ctrl+Tab
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "inside-7z.ppm" "Recents did not reopen an image stored inside 7z"
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool key BackSpace
	DISPLAY=":$display_number" xdotool type --delay 20 '03-BLUE'
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "03-blue.ppm" "open dialog could not leave 7z and return to a filesystem image"
fi

if [ -f "$temporary/images/15-comic.cb7" ]; then
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool type --delay 20 '15-comic.cb7'
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.2
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "inside-7z.ppm" "open dialog did not browse and open a CB7 image member"
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool key BackSpace
	DISPLAY=":$display_number" xdotool type --delay 20 '03-BLUE'
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "03-blue.ppm" "open dialog could not leave a CB7 and return to a filesystem image"
fi

if [ -f "$temporary/images/10-archive.rar" ]; then
	# RAR5 uses the shared cancellable libarchive catalog and member-preview path.
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool type --delay 20 '10-archive.rar'
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.2
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "testfile.jpg" "open dialog did not browse and open a RAR image member"
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool key ctrl+Tab
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "testfile.jpg" "Recents did not reopen an image stored inside RAR"
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool key BackSpace
	DISPLAY=":$display_number" xdotool type --delay 20 '03-BLUE'
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "03-blue.ppm" "open dialog could not leave RAR and return to a filesystem image"
fi

DISPLAY=":$display_number" xdotool key ctrl+o
click_file_dialog_sort
DISPLAY=":$display_number" xdotool key Home
DISPLAY=":$display_number" xdotool key Down
DISPLAY=":$display_number" xdotool key ctrl+Return
sleep 0.3
immediate_directory_title=$(DISPLAY=":$display_number" window_title_without_position)
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
entered_directory_title=$(DISPLAY=":$display_number" window_title_without_position)
case "$entered_directory_title" in
	inside-first.ppm*) ;;
	*) echo "UI smoke test: entering a directory focused [..] instead of its first child" >&2; exit 1 ;;
esac

DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool key BackSpace
DISPLAY=":$display_number" xdotool key Return
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
restored_directory_title=$(DISPLAY=":$display_number" window_title_without_position)
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
repeated_dialog_title=$(DISPLAY=":$display_number" window_title_without_position)
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
paged_dialog_title=$(DISPLAY=":$display_number" window_title_without_position)
case "$paged_dialog_title" in
	01-red.ppm*) ;;
	*) echo "UI smoke test: PageUp/PageDown did not page through the Ctrl+O dialog" >&2; exit 1 ;;
esac

DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool type --delay 20 '.ppm'
DISPLAY=":$display_number" xdotool key End
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
end_dialog_title=$(DISPLAY=":$display_number" window_title_without_position)
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
home_dialog_title=$(DISPLAY=":$display_number" window_title_without_position)
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
wheel_dialog_title=$(DISPLAY=":$display_number" window_title_without_position)
case "$wheel_dialog_title" in
	wheel-04.ppm*) ;;
	*) echo "UI smoke test: mouse wheel did not scroll and select through the open-dialog file list" >&2; exit 1 ;;
esac

# Clicking the scrollbar track pages one viewport; dragging its proportional
# thumb to the end exposes and opens the final entry without changing the
# selected row until the pointer moves back into the list.
wheel_preview_width=$((wheel_dialog_width / 3))
if [ "$wheel_preview_width" -lt 200 ]; then wheel_preview_width=200; fi
if [ "$wheel_preview_width" -gt 260 ]; then wheel_preview_width=260; fi
wheel_list_width=$((wheel_dialog_width - 24 - wheel_preview_width - 12))
wheel_visible_rows=$(((wheel_dialog_height - 168) / 26))
if [ "$wheel_visible_rows" -lt 1 ]; then wheel_visible_rows=1; fi
wheel_track_x=$((wheel_dialog_x + 12 + wheel_list_width - 8))
wheel_track_y=$((wheel_dialog_y + 112 + 2))
wheel_track_height=$((wheel_visible_rows * 26 - 4))
wheel_thumb_height=$((wheel_track_height * wheel_visible_rows / 41))
if [ "$wheel_thumb_height" -lt 26 ]; then wheel_thumb_height=26; fi
wheel_last_row_y=$((wheel_dialog_y + 112 + (wheel_visible_rows - 1) * 26 + 13))
wheel_content_x=$((wheel_dialog_x + 30))

DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool type --delay 10 'wheel-'
if [ "$visual_assertions" -eq 1 ]; then
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/open-dialog-scrollbar.png"
	scrollbar_thumb_pixel=$(convert "$temporary/open-dialog-scrollbar.png" \
		-format "%[pixel:p{$wheel_track_x,$((wheel_track_y + wheel_thumb_height / 2))}]" info:)
	case "$scrollbar_thumb_pixel" in
		srgb\(128,128,128\)|srgb\(158,158,158\)|srgb\(185,185,185\)) ;;
		*) echo "UI smoke test: scrollbar thumb was not visibly rendered ($scrollbar_thumb_pixel)" >&2; exit 1 ;;
	esac
fi
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
	"$wheel_track_x" "$((wheel_track_y + wheel_track_height - 1))" click 1
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
	"$wheel_content_x" "$wheel_last_row_y" click 1
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
scrollbar_page_title=$(DISPLAY=":$display_number" window_title_without_position)
case "$scrollbar_page_title" in
	wheel-34.ppm*) ;;
	*) echo "UI smoke test: clicking below the scrollbar thumb did not page the file list" >&2; exit 1 ;;
esac

DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool type --delay 10 'wheel-'
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
	"$wheel_track_x" "$((wheel_track_y + wheel_thumb_height / 2))"
DISPLAY=":$display_number" xdotool mousedown 1
DISPLAY=":$display_number" xdotool mousemove --sync --window "$window_id" \
	"$wheel_track_x" "$((wheel_track_y + wheel_track_height - wheel_thumb_height / 2))"
DISPLAY=":$display_number" xdotool mouseup 1
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
	"$wheel_content_x" "$wheel_last_row_y" click 1
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
scrollbar_drag_title=$(DISPLAY=":$display_number" window_title_without_position)
case "$scrollbar_drag_title" in
	wheel-39.ppm*) ;;
	*) echo "UI smoke test: dragging the scrollbar thumb to its end did not reveal the final file" >&2; exit 1 ;;
esac

DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool key BackSpace
DISPLAY=":$display_number" xdotool type --delay 10 '01-RED'
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
wheel_restore_title=$(DISPLAY=":$display_number" window_title_without_position)
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
sibling_start_title=$(DISPLAY=":$display_number" window_title_without_position)
case "$sibling_start_title" in
	first.ppm*) ;;
	*) echo "UI smoke test: could not enter the first sibling-folder fixture ($sibling_start_title)" >&2; exit 1 ;;
esac
DISPLAY=":$display_number" xdotool key alt+Right
sleep 0.3
sibling_next_title=$(DISPLAY=":$display_number" window_title_without_position)
case "$sibling_next_title" in
	inside-first.ppm*) ;;
	*) echo "UI smoke test: Alt+Right did not open the next sibling folder's first image" >&2; exit 1 ;;
esac
DISPLAY=":$display_number" xdotool key alt+Left
sibling_previous_title=$(DISPLAY=":$display_number" window_title_without_position)
for _ in $(seq 1 40); do
	case "$sibling_previous_title" in
		first.ppm*) break ;;
	esac
	sleep 0.05
	sibling_previous_title=$(DISPLAY=":$display_number" window_title_without_position)
done
case "$sibling_previous_title" in
	first.ppm*) ;;
	*) echo "UI smoke test: Alt+Left did not open the previous sibling folder's first image ($sibling_previous_title)" >&2; exit 1 ;;
esac
DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool key BackSpace
DISPLAY=":$display_number" xdotool type --delay 10 '01-RED'
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
sibling_restore_title=$(DISPLAY=":$display_number" window_title_without_position)
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
single_press_title=$(DISPLAY=":$display_number" window_title_without_position)
case "$single_press_title" in
	02-green.ppm*) ;;
	*) echo "UI smoke test: one Right press skipped over the adjacent image" >&2; exit 1 ;;
esac
DISPLAY=":$display_number" xdotool key Left
sleep 0.2

title_before=$(DISPLAY=":$display_number" window_title_without_position)
DISPLAY=":$display_number" xdotool mousemove 640 400
DISPLAY=":$display_number" xdotool click 4
sleep 0.4
title_after_wheel=$(DISPLAY=":$display_number" window_title_without_position)
if [ "$title_before" = "$title_after_wheel" ]; then
	echo "UI smoke test: plain wheel did not navigate" >&2
	exit 1
fi

title_before_hold=$title_after_wheel
DISPLAY=":$display_number" xdotool keydown Right
sleep 0.7
DISPLAY=":$display_number" xdotool keyup Right
sleep 0.2
title_after_hold=$(DISPLAY=":$display_number" window_title_without_position)
if [ "$title_before_hold" = "$title_after_hold" ]; then
	echo "UI smoke test: held Right key did not repeat navigation" >&2
	exit 1
fi
sleep 0.3
title_after_release_settled=$(DISPLAY=":$display_number" window_title_without_position)
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

title_before_ctrl_wheel=$(DISPLAY=":$display_number" window_title_without_position)
DISPLAY=":$display_number" xdotool keydown ctrl
DISPLAY=":$display_number" xdotool click 5
DISPLAY=":$display_number" xdotool keyup ctrl
sleep 0.3
title_after_ctrl_wheel=$(DISPLAY=":$display_number" window_title_without_position)
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
DISPLAY=":$display_number" xdotool key Right
DISPLAY=":$display_number" xdotool key ctrl+m
DISPLAY=":$display_number" xdotool key Home
sleep 0.2
if [ "$visual_assertions" -eq 1 ]; then
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/thumbnail-marked.png"
	thumbnail_window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
	thumbnail_mark_y=$(((thumbnail_window_height + 163) / 2 + 1))
	thumbnail_mark_outline=$(convert "$temporary/thumbnail-marked.png" \
		-format "%[hex:p{2,$thumbnail_mark_y}]" info: | tr 'A-F' 'a-f')
	case "$thumbnail_mark_outline" in
		ffc341*) ;;
		*) echo "UI smoke test: Ctrl+M did not outline the marked thumbnail (pixel $thumbnail_mark_outline at y=$thumbnail_mark_y)" >&2; exit 1 ;;
	esac
fi
title_before_thumbnail_click=$(DISPLAY=":$display_number" window_title_without_position)
thumbnail_window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
thumbnail_neighbor_y=$(((thumbnail_window_height + 163) / 2))
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 80 "$thumbnail_neighbor_y"
DISPLAY=":$display_number" xdotool click 1
sleep 0.3
title_after_thumbnail_click=$(DISPLAY=":$display_number" window_title_without_position)
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
	# Anchor the compact menu near the top so the mnemonic pixel assertion has
	# stable coordinates independent of menu-edge repositioning.
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 20
	DISPLAY=":$display_number" xdotool click 3
	sleep 0.2
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/context-compact.png"
	# “Show Advanced Options” gets the `S` mnemonic. The five-pixel glyph ink
	# ends at x=656; x=657 is blank cell space and must not get an underline
	# endpoint. The menu is opened at (640,20), and this underline is at y=36.
	mnemonic_ink_pixel=$(convert "$temporary/context-compact.png" -crop 1x1+656+36 +repage \
		-colorspace Gray -threshold 50% -format "%[fx:mean]" info:)
	mnemonic_trailing_pixel=$(convert "$temporary/context-compact.png" -crop 1x1+657+36 +repage \
		-colorspace Gray -threshold 50% -format "%[fx:mean]" info:)
	if [ "$mnemonic_ink_pixel" != "1" ] || [ "$mnemonic_trailing_pixel" != "0" ]; then
		echo "UI smoke test: menu mnemonic underline did not follow the glyph ink bounds (ink=$mnemonic_ink_pixel trailing=$mnemonic_trailing_pixel)" >&2
		exit 1
	fi
	# The final `s` in Show Advanced Options has ink through column 4 of its
	# six-pixel cell. Column 5 must remain empty even with best-quality image
	# texture filtering selected globally.
	menu_terminal_ink=$(convert "$temporary/context-compact.png" -crop 1x1+776+29 +repage \
		-colorspace Gray -threshold 50% -format "%[fx:mean]" info:)
	menu_terminal_trailing=$(convert "$temporary/context-compact.png" -crop 1x1+777+29 +repage \
		-colorspace Gray -threshold 50% -format "%[fx:mean]" info:)
	if [ "$menu_terminal_ink" != "1" ] || [ "$menu_terminal_trailing" != "0" ]; then
		echo "UI smoke test: context-menu text gained a pixel beyond its final glyph (ink=$menu_terminal_ink trailing=$menu_terminal_trailing)" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool keydown Right
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 700 30
	sleep 0.1
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/context-right-key-held.png"
	DISPLAY=":$display_number" xdotool keyup Right
	sleep 0.2
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/context-right-key-release.png"
	right_key_difference=$(compare -metric AE "$temporary/context-right-key-held.png" \
		"$temporary/context-right-key-release.png" null: 2>&1 || true)
	if [ "$right_key_difference" = "0" ]; then
		echo "UI smoke test: releasing Right over a context-menu item did not activate it" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool key Escape
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 20
	DISPLAY=":$display_number" xdotool click 3
	sleep 0.2
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 700 31
	DISPLAY=":$display_number" xdotool click 1
	sleep 0.2
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/context-advanced.png"
	advanced_difference=$(compare -metric AE "$temporary/context-compact.png" "$temporary/context-advanced.png" null: 2>&1 || true)
	if [ "$advanced_difference" = "0" ]; then
		echo "UI smoke test: Advanced Options did not expand the context menu" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool key Escape
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 20
	DISPLAY=":$display_number" xdotool keydown Shift_L
	DISPLAY=":$display_number" xdotool click 3
	DISPLAY=":$display_number" xdotool keyup Shift_L
	sleep 0.2
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/context-shift-right.png"
	shift_right_difference=$(compare -metric AE "$temporary/context-advanced.png" \
		"$temporary/context-shift-right.png" null: 2>&1 || true)
	if [ "$shift_right_difference" != "0" ]; then
		echo "UI smoke test: Shift+right-click did not open the full context menu immediately ($shift_right_difference)" >&2
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
title_after_reload=$(DISPLAY=":$display_number" window_title_without_position)
case "$title_after_reload" in
	01-red.ppm\ *) ;;
	*) echo "UI smoke test: persisted filename ordering was not restored" >&2; exit 1 ;;
esac
thumbnail_window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
thumbnail_neighbor_y=$(((thumbnail_window_height + 163) / 2))
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 200 "$thumbnail_neighbor_y"
DISPLAY=":$display_number" xdotool click 1
sleep 0.3
title_after_restored_thumbnail_click=$(DISPLAY=":$display_number" window_title_without_position)
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
# Ctrl+Tab must switch from Browse to Recents: this path-only filter cannot
# match the Browse tab's filename list.
DISPLAY=":$display_number" xdotool key ctrl+Tab
DISPLAY=":$display_number" xdotool type --delay 20 '00-entry-test'
sleep 0.5
recent_database="$temporary/state/jpegview-linux/recent-files.db"
if [ ! -s "$recent_database" ]; then
	echo "UI smoke test: recent-file history was not persisted after the first viewer session" >&2
	exit 1
fi
if [ "$visual_assertions" -eq 1 ]; then
	preview_width=$(awk -F= '$1 == "file_dialog_preview_ratio" { printf "%d", ($2 * 924 + 0.5) }' "$settings")
	preview_left=$((reopened_dialog_x + 960 - 12 - preview_width))
	preview_rows=$(((685 - 168) / 26))
	preview_image_width=$((preview_width - 16))
	preview_image_height=$((preview_rows * 26 - 82))
	preview_pixel_x=$((preview_left + 8 + (preview_image_width - 2) / 2))
	preview_pixel_y=$((reopened_dialog_y + 112 + 28 + (preview_image_height - 2) / 2))
	# The existing shell fixture writes textual backslash-octal bytes after its P6 header.
	expected_preview_pixel='srgb(92,49,48)'
	preview_pixel=''
	for _ in $(seq 1 20); do
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/recent-dialog-preview.png"
		preview_pixel=$(convert "$temporary/recent-dialog-preview.png" \
			-format "%[pixel:p{$preview_pixel_x,$preview_pixel_y}]" info:)
		[ "$preview_pixel" = "$expected_preview_pixel" ] && break
		sleep 0.1
	done
	if [ "$preview_pixel" != "$expected_preview_pixel" ]; then
		echo "UI smoke test: Recents did not show the focused image preview at ${preview_pixel_x},${preview_pixel_y} ($preview_pixel)" >&2
		exit 1
	fi
	preview_details_y=$((reopened_dialog_y + 112 + preview_rows * 26 - 37))
	preview_details_ink=$(convert "$temporary/recent-dialog-preview.png" \
		-crop "${preview_image_width}x14+${preview_left}+${preview_details_y}" +repage txt:- |
		awk '/srgb\(165,175,185\)/ { found = 1; exit } END { if (found) print "present" }')
	if [ -z "$preview_details_ink" ]; then
		echo "UI smoke test: focused-image dimensions and size were not drawn in the preview footer" >&2
		exit 1
	fi
	file_list_x=$((reopened_dialog_x + 12))
	file_list_y=$((reopened_dialog_y + 112))
	file_list_width=$((960 - 24 - (preview_width + 12) - 16))
	file_size_ink=$(convert "$temporary/recent-dialog-preview.png" \
		-crop "${file_list_width}x$((preview_rows * 26))+${file_list_x}+${file_list_y}" +repage txt:- |
		awk '/srgb\(165,180,200\)/ { found = 1; exit } END { if (found) print "present" }')
	if [ -z "$file_size_ink" ]; then
		echo "UI smoke test: Recents file sizes were not drawn in the list" >&2
		exit 1
	fi
	help_start_x=$((reopened_dialog_x + 18))
	help_scan_width=$((960 - 36))
	help_last_row_y=$((reopened_dialog_y + 685 - 34 + 9))
	help_last_ink_offset=$(convert "$temporary/recent-dialog-preview.png" \
		-crop "${help_scan_width}x1+${help_start_x}+${help_last_row_y}" +repage txt:- |
		awk -F '[,:]' '/srgb\(170,170,170\)/ { last = $1 } END { if (last != "") print last }')
	if [ -z "$help_last_ink_offset" ]; then
		echo "UI smoke test: open-dialog help text was not visible on its expected row" >&2
		exit 1
	fi
	help_trailing_pixel=$(convert "$temporary/recent-dialog-preview.png" \
		-format "%[pixel:p{$((help_start_x + help_last_ink_offset + 1)),$help_last_row_y}]" info:)
	if [ "$help_trailing_pixel" = 'srgb(170,170,170)' ]; then
		echo "UI smoke test: open-dialog help text has a stray pixel after its final glyph (trailing=$help_trailing_pixel)" >&2
		exit 1
	fi
	dialog_rows=$(((685 - 168) / 26))
	divider_grip_x=$((reopened_dialog_x + 942 - preview_width))
	divider_grip_y=$((reopened_dialog_y + 112 + dialog_rows * 26 / 2))
	divider_grip_end=$(convert "$temporary/recent-dialog-preview.png" \
		-format "%[pixel:p{$((divider_grip_x + 2)),$divider_grip_y}]" info:)
	divider_grip_extra=$(convert "$temporary/recent-dialog-preview.png" \
		-format "%[pixel:p{$((divider_grip_x + 3)),$divider_grip_y}]" info:)
	if [ "$divider_grip_end" != 'srgb(145,160,178)' ] || \
		[ "$divider_grip_extra" = 'srgb(145,160,178)' ]; then
		echo "UI smoke test: the dialog divider grip draws beyond its intended five-pixel span (end=$divider_grip_end extra=$divider_grip_extra)" >&2
		exit 1
	fi
fi
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
recent_open_title=$(DISPLAY=":$display_number" window_title_without_position)
case "$recent_open_title" in
	inside-first.ppm\ *) ;;
	*) echo "UI smoke test: Enter did not open the focused recent image ($recent_open_title)" >&2; exit 1 ;;
esac
DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool key ctrl+Tab
DISPLAY=":$display_number" xdotool key BackSpace
DISPLAY=":$display_number" xdotool key ctrl+Tab
DISPLAY=":$display_number" xdotool type --delay 20 'inside-first'
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
# The empty-filter Backspace in Recents must not have moved the hidden Browse
# directory. A single Enter should therefore open the image and close the dialog.
DISPLAY=":$display_number" xdotool key Escape
for _ in $(seq 1 20); do
	if ! DISPLAY=":$display_number" xdotool search --onlyvisible --class jpegview-linux \
		>/dev/null 2>&1; then break; fi
	sleep 0.05
done
if DISPLAY=":$display_number" xdotool search --onlyvisible --class jpegview-linux \
	>/dev/null 2>&1; then
	echo "UI smoke test: empty-filter Backspace navigated the hidden Browse directory" >&2
	exit 1
fi
wait "$viewer_pid" || true
viewer_pid=''

# Removing recent rows works from both the Delete key and the Recents button;
# Ctrl+Z undoes removals in LIFO order only while the dialog remains open.
mkdir -p "$temporary/recent-removal/album-a" "$temporary/recent-removal/album-b"
write_solid_ppm "$temporary/recent-removal/album-a/01-a.ppm" 220 30 40
write_solid_ppm "$temporary/recent-removal/album-b/02-b.ppm" 35 80 225
XDG_STATE_HOME="$temporary/recent-removal-state"
export XDG_STATE_HOME
VIEWER_TEST_HOME="$temporary/recent-removal-home" \
	VIEWER_TEST_CONFIG_HOME="$temporary/recent-removal-config" \
	launch_viewer "$temporary/recent-removal/album-a/01-a.ppm"
assert_title_prefix "01-a.ppm" "recent-removal fixture did not open its first image"
stop_viewer
VIEWER_TEST_HOME="$temporary/recent-removal-home" \
	VIEWER_TEST_CONFIG_HOME="$temporary/recent-removal-config" \
	launch_viewer "$temporary/recent-removal/album-b/02-b.ppm"
assert_title_prefix "02-b.ppm" "recent-removal fixture did not open its second image"
DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool key ctrl+Tab
DISPLAY=":$display_number" xdotool key Delete
DISPLAY=":$display_number" xdotool key Delete
DISPLAY=":$display_number" xdotool key ctrl+z
DISPLAY=":$display_number" xdotool key ctrl+z
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
assert_title_prefix "02-b.ppm" "Ctrl+Z did not restore multiple recent removals in reverse order"

# The GUI button follows the same undo path as Delete.
DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool key ctrl+Tab
removal_window_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^WIDTH=//p')
removal_window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
remove_button_x=$(((removal_window_width - 900) / 2 + 846))
remove_button_y=$(((removal_window_height - 650) / 2 + 52))
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
	"$remove_button_x" "$remove_button_y" click 1
DISPLAY=":$display_number" xdotool key ctrl+z
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
assert_title_prefix "02-b.ppm" "the Recents Remove button or Ctrl+Z undo did not preserve its item"

# Delete persists after dialog close; Ctrl+Z outside the dialog has no effect.
DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool key ctrl+Tab
DISPLAY=":$display_number" xdotool key Delete
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
assert_title_prefix "01-a.ppm" "Delete did not remove the selected recent item"
DISPLAY=":$display_number" xdotool key ctrl+z
DISPLAY=":$display_number" xdotool key ctrl+o
DISPLAY=":$display_number" xdotool key ctrl+Tab
DISPLAY=":$display_number" xdotool key Return
sleep 0.3
assert_title_prefix "01-a.ppm" "Ctrl+Z outside the open dialog unexpectedly restored a recent item"
stop_viewer
XDG_STATE_HOME="$temporary/state"
export XDG_STATE_HOME

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
	portrait_title=$(DISPLAY=":$display_number" window_title_without_position)
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

if command -v convert >/dev/null 2>&1; then
	mkdir -p "$temporary/navigator-config"
	convert -size 1600x1200 xc:red -fill blue -draw 'rectangle 800,0 1599,1199' \
		"$temporary/zoom-navigator.ppm"
	env -u WAYLAND_DISPLAY -u XDG_SESSION_TYPE DISPLAY=":$display_number" \
		HOME="$temporary/home" XDG_CONFIG_HOME="$temporary/navigator-config" \
		"$BINARY" "$temporary/zoom-navigator.ppm" \
		>"$temporary/zoom-navigator-viewer.log" 2>&1 &
	viewer_pid=$!
	window_id=''
	for _ in $(seq 1 50); do
		window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null | head -1 || true)
		if [ -n "$window_id" ]; then break; fi
		sleep 0.1
	done
	if [ -z "$window_id" ]; then
		echo "UI smoke test: zoom-navigator viewer did not appear" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	sleep 0.3
	DISPLAY=":$display_number" xdotool key space
	sleep 0.2
	navigator_window_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^WIDTH=//p')
	navigator_window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
	navigator_hot_width=$(((navigator_window_width * 20 + 50) / 100 + 40))
	if [ "$navigator_hot_width" -gt 320 ]; then navigator_hot_width=320; fi
	if [ "$navigator_hot_width" -lt 133 ]; then navigator_hot_width=133; fi
	if [ "$navigator_hot_width" -gt $((navigator_window_width - 16)) ]; then
		navigator_hot_width=$((navigator_window_width - 16))
	fi
	navigator_hot_height=$(((navigator_hot_width * 3 + 2) / 4))
	if [ "$navigator_hot_height" -gt $((navigator_window_height - 16)) ]; then
		navigator_hot_height=$((navigator_window_height - 16))
	fi
	navigator_x=$((navigator_window_width - navigator_hot_width - 8))
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((navigator_x + 24)) 24
	sleep 0.2
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/zoom-navigator.png"
		navigator_frame=$(convert "$temporary/zoom-navigator.png" -format \
			"%[pixel:p{$((navigator_x - 2)),$((8 - 2))}]" info:)
		if [ "$navigator_frame" != "srgb(245,245,245)" ]; then
			echo "UI smoke test: zoom navigator did not appear in its corner hot area ($navigator_frame)" >&2
			exit 1
		fi
	fi
	# The overview click recenters the image around the selected source point.
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((navigator_x + navigator_hot_width / 5)) $((navigator_hot_height / 2 + 8)) click 1
	sleep 0.2
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/zoom-navigator-click.png"
		navigator_center_x=$((navigator_window_width / 2))
		navigator_center_y=$((navigator_window_height / 2))
		navigator_click_color=$(convert "$temporary/zoom-navigator-click.png" -format \
			"%[fx:p{$navigator_center_x,$navigator_center_y}.r>0.7&&p{$navigator_center_x,$navigator_center_y}.b<0.3]" info:)
		if [ "$navigator_click_color" != "1" ]; then
			echo "UI smoke test: clicking the navigator did not pan to the left image area ($navigator_click_color)" >&2
			exit 1
		fi
	fi
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((navigator_x + navigator_hot_width / 2)) $((navigator_hot_height / 2 + 8)) mousedown 1
	sleep 0.1
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((navigator_x + navigator_hot_width / 2 + navigator_hot_width / 3)) \
		$((navigator_hot_height / 2 + 8)) mouseup 1
	sleep 0.2
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/zoom-navigator-drag.png"
		navigator_center_x=$((navigator_window_width / 2))
		navigator_center_y=$((navigator_window_height / 2))
		navigator_drag_color=$(convert "$temporary/zoom-navigator-drag.png" -format \
			"%[fx:p{$navigator_center_x,$navigator_center_y}.b>0.7&&p{$navigator_center_x,$navigator_center_y}.r<0.3]" info:)
		if [ "$navigator_drag_color" != "1" ]; then
			echo "UI smoke test: dragging the navigator did not pan to the right image area ($navigator_drag_color)" >&2
			exit 1
		fi
	fi
	stop_viewer

	mkdir -p "$temporary/magnifier-images" "$temporary/magnifier-config"
	# Keep the centered lens pixel inside a solid region so dark center artifacts stand out.
	convert -size 1600x1200 xc:red -fill blue -draw 'rectangle 1100,0 1599,1199' \
		"$temporary/magnifier-images/01-magnifier.png"
	convert -size 1600x1200 xc:lime "$temporary/magnifier-images/02-next.png"
	launch_magnifier_viewer() {
		env -u WAYLAND_DISPLAY -u XDG_SESSION_TYPE DISPLAY=":$display_number" \
			HOME="$temporary/home" XDG_CONFIG_HOME="$temporary/magnifier-config" \
			"$BINARY" "$temporary/magnifier-images" >"$temporary/magnifier-viewer.log" 2>&1 &
		viewer_pid=$!
		window_id=''
		for _ in $(seq 1 50); do
			window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
				--class jpegview-linux 2>/dev/null | head -1 || true)
			if [ -n "$window_id" ]; then break; fi
			sleep 0.1
		done
		if [ -z "$window_id" ]; then
			echo "UI smoke test: magnifying-glass viewer did not appear" >&2
			exit 1
		fi
		DISPLAY=":$display_number" xdotool windowactivate "$window_id"
		sleep 0.3
	}
	launch_magnifier_viewer
	magnifier_window_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell \
		"$window_id" | sed -n 's/^WIDTH=//p')
	magnifier_window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell \
		"$window_id" | sed -n 's/^HEIGHT=//p')
	magnifier_center_x=$((magnifier_window_width / 2))
	magnifier_center_y=$((magnifier_window_height / 2))
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		"$magnifier_center_x" "$magnifier_center_y" key z
	sleep 0.4
	magnifier_title_before=$(DISPLAY=":$display_number" window_title_without_position)
	case "$magnifier_title_before" in
		01-magnifier.png\ *) ;;
		*) echo "UI smoke test: magnifying-glass fixture did not open its first image ($magnifier_title_before)" >&2; exit 1 ;;
	esac
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/magnifier-default.png"
		magnifier_default_border=$(convert "$temporary/magnifier-default.png" -format \
			"%[pixel:p{$((magnifier_center_x - 175)),$magnifier_center_y}]" info:)
		if [ "$magnifier_default_border" != "srgb(245,245,245)" ]; then
			echo "UI smoke test: Z did not draw the centered magnifying-glass lens ($magnifier_default_border)" >&2
			exit 1
		fi
		magnifier_center_pixel=$(convert "$temporary/magnifier-default.png" -format \
			"%[pixel:p{$magnifier_center_x,$magnifier_center_y}]" info:)
		if [ "$magnifier_center_pixel" != "srgb(255,0,0)" ]; then
			echo "UI smoke test: magnifying-glass center has an unexpected pixel ($magnifier_center_pixel)" >&2
			exit 1
		fi
	fi
	DISPLAY=":$display_number" xdotool click 5
	sleep 0.2
	magnifier_title_after=$(DISPLAY=":$display_number" window_title_without_position)
	if [ "$magnifier_title_before" != "$magnifier_title_after" ]; then
		echo "UI smoke test: wheel resizing the magnifier navigated to another image" >&2
		exit 1
	fi
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/magnifier-expanded.png"
		magnifier_expanded_border=$(convert "$temporary/magnifier-expanded.png" -format \
			"%[pixel:p{$((magnifier_center_x - 190)),$magnifier_center_y}]" info:)
		magnifier_old_border=$(convert "$temporary/magnifier-expanded.png" -format \
			"%[pixel:p{$((magnifier_center_x - 175)),$magnifier_center_y}]" info:)
		if [ "$magnifier_expanded_border" != "srgb(245,245,245)" ] ||
			[ "$magnifier_old_border" = "srgb(245,245,245)" ]; then
			echo "UI smoke test: wheel did not enlarge the lens as expected ($magnifier_expanded_border / $magnifier_old_border)" >&2
			exit 1
		fi
	fi
	DISPLAY=":$display_number" xdotool keydown shift click 5 keyup shift
	DISPLAY=":$display_number" xdotool key z
	DISPLAY=":$display_number" xdotool click 5
	assert_title_prefix "02-next.png" "wheel did not resume normal navigation after disabling the magnifier"
	stop_viewer
	magnifier_settings="$temporary/magnifier-config/jpegview-linux/settings.conf"
	grep -q '^magnifying_glass_width=380$' "$magnifier_settings"
	grep -q '^magnifying_glass_height=190$' "$magnifier_settings"
	awk -F= '$1 == "magnifying_glass_zoom_level" && $2 > 0.5249 && $2 < 0.5251 { found = 1 } END { exit !found }' \
		"$magnifier_settings"

	# On the next run, one wheel-up step should return the restored lens to its defaults.
	launch_magnifier_viewer
	magnifier_window_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell \
		"$window_id" | sed -n 's/^WIDTH=//p')
	magnifier_window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell \
		"$window_id" | sed -n 's/^HEIGHT=//p')
	magnifier_center_x=$((magnifier_window_width / 2))
	magnifier_center_y=$((magnifier_window_height / 2))
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		"$magnifier_center_x" "$magnifier_center_y" key z
	DISPLAY=":$display_number" xdotool click 4
	DISPLAY=":$display_number" xdotool keydown shift click 4 keyup shift
	grep -q '^magnifying_glass_width=350$' "$magnifier_settings"
	grep -q '^magnifying_glass_height=175$' "$magnifier_settings"
	awk -F= '$1 == "magnifying_glass_zoom_level" && $2 > 0.4999 && $2 < 0.5001 { found = 1 } END { exit !found }' \
		"$magnifier_settings"
	stop_viewer

	mkdir -p "$temporary/crop-images" "$temporary/crop-config"
	convert -size 160x128 xc:red -fill blue -draw 'rectangle 80,0 159,127' \
		-sampling-factor 2x2 "$temporary/crop-images/01-crop.jpg"
	env -u WAYLAND_DISPLAY -u XDG_SESSION_TYPE DISPLAY=":$display_number" \
		HOME="$temporary/home" XDG_CONFIG_HOME="$temporary/crop-config" \
		"$BINARY" "$temporary/crop-images/01-crop.jpg" \
		>"$temporary/crop-viewer.log" 2>&1 &
	viewer_pid=$!
	window_id=''
	for _ in $(seq 1 50); do
		window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null | head -1 || true)
		if [ -n "$window_id" ]; then break; fi
		sleep 0.1
	done
	if [ -z "$window_id" ]; then
		echo "UI smoke test: crop viewer did not appear" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	sleep 0.3
	crop_window_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^WIDTH=//p')
	crop_window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
	crop_image_left=$((crop_window_width / 2 - 80))
	crop_image_top=$((crop_window_height / 2 - 64))
	crop_settings="$temporary/crop-config/jpegview-linux/settings.conf"
	# A fresh launch must leave normal drags in view mode rather than silently creating a crop.
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/crop-disabled-before.png"
	fi
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((crop_image_left + 32)) $((crop_image_top + 32)) mousedown 1
	sleep 0.1
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((crop_image_left + 127)) $((crop_image_top + 95)) mouseup 1
	sleep 0.2
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/crop-disabled-after.png"
		convert "$temporary/crop-disabled-before.png" -crop "160x128+$crop_image_left+$crop_image_top" +repage \
			"$temporary/crop-disabled-before-area.png"
		convert "$temporary/crop-disabled-after.png" -crop "160x128+$crop_image_left+$crop_image_top" +repage \
			"$temporary/crop-disabled-after-area.png"
		if ! compare -metric AE "$temporary/crop-disabled-before-area.png" \
			"$temporary/crop-disabled-after-area.png" null: 2>"$temporary/crop-disabled-difference.txt"; then
			echo "UI smoke test: an ordinary drag created a crop while crop mode was disabled ($(sed -n '1p' "$temporary/crop-disabled-difference.txt"))" >&2
			exit 1
		fi
	fi
	# The crop-selection button is followed by the two paired-page mode controls.
	selection_button_x=$((crop_window_width / 2 + 117))
	selection_button_y=$((crop_window_height - 16))
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		"$selection_button_x" "$selection_button_y" click 1
	sleep 0.2
	grep -q '^selection_mode_enabled=1$' "$crop_settings"
	# A normal drag now opens the crop menu. Its first actionable item toggles the mode off.
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((crop_image_left + 32)) $((crop_image_top + 32)) mousedown 1
	sleep 0.1
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((crop_image_left + 127)) $((crop_image_top + 95)) mouseup 1
	sleep 0.2
	DISPLAY=":$display_number" xdotool key Down Return
	sleep 0.2
	grep -q '^selection_mode_enabled=0$' "$crop_settings"
	# Ctrl+E is the direct toggle; restore the mode for the rest of the crop regression.
	DISPLAY=":$display_number" xdotool key ctrl+e
	sleep 0.2
	grep -q '^selection_mode_enabled=1$' "$crop_settings"
	DISPLAY=":$display_number" xdotool key Escape
	# Shift-drag selects the source rectangle, zooms into it, and clears the overlay.
	DISPLAY=":$display_number" xdotool keydown Shift_L
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((crop_image_left + 32)) $((crop_image_top + 32)) mousedown 1
	sleep 0.1
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((crop_image_left + 127)) $((crop_image_top + 95))
	DISPLAY=":$display_number" xdotool mouseup 1 keyup Shift_L
	sleep 0.3
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/crop-zoom.png"
		zoom_sample_y=$((crop_window_height / 2))
		zoom_red_x=$((crop_window_width / 2 - 200))
		zoom_blue_x=$((crop_window_width / 2 + 200))
		zoom_red=$(convert "$temporary/crop-zoom.png" -format \
			"%[fx:p{$zoom_red_x,$zoom_sample_y}.r>0.7&&p{$zoom_red_x,$zoom_sample_y}.b<0.3]" info:)
		zoom_blue=$(convert "$temporary/crop-zoom.png" -format \
			"%[fx:p{$zoom_blue_x,$zoom_sample_y}.b>0.7&&p{$zoom_blue_x,$zoom_sample_y}.r<0.3]" info:)
		if [ "$zoom_red" != "1" ] || [ "$zoom_blue" != "1" ]; then
			echo "UI smoke test: Shift-drag did not zoom to the selected source pixels ($zoom_red/$zoom_blue)" >&2
			exit 1
		fi
	fi
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.2
	# Make a selection and use its context menu to open the fixed-size editor.
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((crop_image_left + 32)) $((crop_image_top + 32)) mousedown 1
	sleep 0.1
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((crop_image_left + 127)) $((crop_image_top + 95)) mouseup 1
	sleep 0.2
	if command -v jpegtran >/dev/null 2>&1; then crop_fixed_mode_steps=7; else crop_fixed_mode_steps=6; fi
	for _ in $(seq 1 "$crop_fixed_mode_steps"); do DISPLAY=":$display_number" xdotool key Down; done
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.2
	crop_dialog_title=$(DISPLAY=":$display_number" window_title_without_position)
	if [ "$crop_dialog_title" != "Set fixed crop size" ]; then
		echo "UI smoke test: crop menu did not open the fixed-size editor ($crop_dialog_title)" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool type --delay 30 '64'
	DISPLAY=":$display_number" xdotool key Tab
	DISPLAY=":$display_number" xdotool type --delay 30 '48'
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.2
	grep -q '^fixed_crop_width=64$' "$crop_settings"
	grep -q '^fixed_crop_height=48$' "$crop_settings"
	grep -q '^fixed_crop_screen_pixels=1$' "$crop_settings"
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/crop-fixed-resized.png"
		fixed_top_left=$(convert "$temporary/crop-fixed-resized.png" -format \
			"%[pixel:p{$((crop_image_left + 32)),$((crop_image_top + 32))}]" info:)
		fixed_right_edge=$(convert "$temporary/crop-fixed-resized.png" -format \
			"%[pixel:p{$((crop_image_left + 95)),$((crop_image_top + 56))}]" info:)
		old_right_edge=$(convert "$temporary/crop-fixed-resized.png" -format \
			"%[pixel:p{$((crop_image_left + 127)),$((crop_image_top + 56))}]" info:)
		if [ "$fixed_top_left" != "srgb(255,205,0)" ] || \
			[ "$fixed_right_edge" != "srgb(255,205,0)" ] || \
			[ "$old_right_edge" = "srgb(255,205,0)" ]; then
			echo "UI smoke test: applying fixed size did not resize the active selection ($fixed_top_left/$fixed_right_edge/$old_right_edge)" >&2
			exit 1
		fi
	fi
	# A fixed-size selection uses the configured screen dimensions at fit zoom.
	DISPLAY=":$display_number" xdotool key Escape
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((crop_image_left + 10)) $((crop_image_top + 10)) mousedown 1
	sleep 0.1
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((crop_image_left + 12)) $((crop_image_top + 12)) mouseup 1
	sleep 0.2
	DISPLAY=":$display_number" xdotool key Escape
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/crop-selection.png"
		selection_border=$(convert "$temporary/crop-selection.png" -format \
			"%[pixel:p{$((crop_image_left + 12)),$((crop_image_top + 12))}]" info:)
		if [ "$selection_border" != "srgb(255,205,0)" ]; then
			echo "UI smoke test: fixed-size crop selection border is missing ($selection_border)" >&2
			exit 1
		fi
	fi
	if command -v xclip >/dev/null 2>&1; then
		DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
			$((crop_image_left + 24)) $((crop_image_top + 24)) click 3
		if command -v jpegtran >/dev/null 2>&1; then crop_copy_steps=4; else crop_copy_steps=3; fi
		for _ in $(seq 1 "$crop_copy_steps"); do DISPLAY=":$display_number" xdotool key Down; done
		DISPLAY=":$display_number" xdotool key Return
		copied_selection_dimensions=''
		for _ in $(seq 1 30); do
			if DISPLAY=":$display_number" xclip -selection clipboard -t image/png -o \
				>"$temporary/copied-selection.png" 2>/dev/null; then
				copied_selection_dimensions=$(identify -format '%wx%h' \
					"$temporary/copied-selection.png" 2>/dev/null || true)
				if [ "$copied_selection_dimensions" = "64x48" ]; then break; fi
			fi
			sleep 0.1
		done
		if [ ! -s "$temporary/copied-selection.png" ] || \
			[ "$copied_selection_dimensions" != "64x48" ]; then
			echo "UI smoke test: Copy Selection did not place its source-size crop on the clipboard" >&2
			exit 1
		fi
		copied_selection_title=$(DISPLAY=":$display_number" window_title_without_position)
		case "$copied_selection_title" in
			"Copied selection to clipboard"*) ;;
			*) echo "UI smoke test: Copy Selection did not complete ($copied_selection_title)" >&2; exit 1 ;;
		esac
	fi
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		$((crop_image_left + 24)) $((crop_image_top + 24)) click 3
	DISPLAY=":$display_number" xdotool key Down Down Return
	sleep 0.3
	if [ "$(identify -format '%wx%h' "$temporary/crop-images/01-crop.jpg")" != "160x128" ]; then
		echo "UI smoke test: regular crop unexpectedly modified its source file" >&2
		exit 1
	fi
	cropped_title=$(DISPLAY=":$display_number" window_title_without_position)
	case "$cropped_title" in
		"01-crop.jpg (64x48,"*) ;;
		*) echo "UI smoke test: in-memory crop did not update the current image dimensions ($cropped_title)" >&2; exit 1 ;;
	esac
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/crop-applied.png"
		crop_sample_x=$((crop_window_width / 2))
		crop_sample_y=$((crop_window_height / 2))
		crop_center=$(convert "$temporary/crop-applied.png" -format \
			"%[fx:p{$crop_sample_x,$crop_sample_y}.r>0.7&&p{$crop_sample_x,$crop_sample_y}.b<0.3]" info:)
		crop_old_edge=$(convert "$temporary/crop-applied.png" -format \
			"%[pixel:p{$((crop_sample_x - 50)),$crop_sample_y}]" info:)
		if [ "$crop_center" != "1" ] || [ "$crop_old_edge" != "srgb(18,18,18)" ]; then
			echo "UI smoke test: regular crop did not replace the display with the selected area ($crop_center/$crop_old_edge)" >&2
			exit 1
		fi
	fi
	stop_viewer

	if command -v jpegtran >/dev/null 2>&1; then
		mkdir -p "$temporary/lossless-crop-config"
		env -u WAYLAND_DISPLAY -u XDG_SESSION_TYPE DISPLAY=":$display_number" \
			HOME="$temporary/home" XDG_CONFIG_HOME="$temporary/lossless-crop-config" "$BINARY" \
			"$temporary/crop-images/01-crop.jpg" >"$temporary/lossless-crop-viewer.log" 2>&1 &
		viewer_pid=$!
		window_id=''
		for _ in $(seq 1 50); do
			window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
				--class jpegview-linux 2>/dev/null | head -1 || true)
			if [ -n "$window_id" ]; then break; fi
			sleep 0.1
		done
		if [ -z "$window_id" ]; then
			echo "UI smoke test: lossless crop viewer did not appear" >&2
			exit 1
		fi
		DISPLAY=":$display_number" xdotool windowactivate "$window_id"
		sleep 0.3
		lossless_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^WIDTH=//p')
		lossless_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
		lossless_left=$((lossless_width / 2 - 80))
		lossless_top=$((lossless_height / 2 - 64))
		DISPLAY=":$display_number" xdotool key ctrl+e
		sleep 0.2
		grep -q '^selection_mode_enabled=1$' \
			"$temporary/lossless-crop-config/jpegview-linux/settings.conf"
		DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
			$((lossless_left + 29)) $((lossless_top + 29)) mousedown 1
		sleep 0.1
		DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
			$((lossless_left + 120)) $((lossless_top + 94)) mouseup 1
		sleep 0.2
		DISPLAY=":$display_number" xdotool key Escape
		DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
			$((lossless_left + 80)) $((lossless_top + 64)) click 3
		DISPLAY=":$display_number" xdotool key Down Down Down Return
		sleep 0.2
		DISPLAY=":$display_number" xdotool key Return
		for _ in $(seq 1 50); do
			if [ -f "$temporary/crop-images/01-crop_crop.jpg" ]; then break; fi
			sleep 0.1
		done
		if [ ! -f "$temporary/crop-images/01-crop_crop.jpg" ] || \
			[ "$(identify -format '%wx%h' "$temporary/crop-images/01-crop_crop.jpg")" != "112x80" ]; then
			echo "UI smoke test: lossless JPEG crop did not save its MCU-aligned region" >&2
			exit 1
		fi
		if [ "$visual_assertions" -eq 1 ]; then
			convert "$temporary/crop-images/01-crop.jpg" -crop 112x80+16+16 +repage \
				"$temporary/crop-lossless-reference.png"
			convert "$temporary/crop-images/01-crop_crop.jpg" "$temporary/crop-lossless-result.png"
			if ! compare -metric AE "$temporary/crop-lossless-reference.png" \
				"$temporary/crop-lossless-result.png" null: 2>"$temporary/crop-lossless-difference.txt"; then
				echo "UI smoke test: lossless JPEG crop changed decoded pixels" >&2
				exit 1
			fi
		fi
		crop_umask=$(umask)
		expected_crop_mode=$(printf '%o' $((0666 & ~crop_umask)))
		actual_crop_mode=$(stat -c '%a' "$temporary/crop-images/01-crop_crop.jpg")
		if [ "$actual_crop_mode" != "$expected_crop_mode" ]; then
			echo "UI smoke test: new lossless crop did not respect the process umask ($actual_crop_mode/$expected_crop_mode)" >&2
			exit 1
		fi
		lossless_title=$(DISPLAY=":$display_number" window_title_without_position)
		case "$lossless_title" in
			*"Saved lossless crop: 01-crop_crop.jpg"*) ;;
			*) echo "UI smoke test: lossless crop did not return to the viewer ($lossless_title)" >&2; exit 1 ;;
		esac
		stop_viewer
	fi
fi

if [ "$visual_assertions" -eq 1 ]; then
	transparency_image="$temporary/transparent.png"
	transparency_config="$temporary/transparency-config"
	mkdir -p "$transparency_config/jpegview-linux"
	convert -size 256x256 xc:none "$transparency_image"
	printf '%s\n' 'transparency_pattern=white' \
		>"$transparency_config/jpegview-linux/settings.conf"
	env -u WAYLAND_DISPLAY -u XDG_SESSION_TYPE DISPLAY=":$display_number" \
		HOME="$temporary/home" XDG_CONFIG_HOME="$transparency_config" "$BINARY" \
		"$transparency_image" >"$temporary/transparency-viewer.log" 2>&1 &
	viewer_pid=$!
	window_id=''
	for _ in $(seq 1 50); do
		window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null | head -1 || true)
		if [ -n "$window_id" ]; then
			transparency_title=$(DISPLAY=":$display_number" window_title_without_position)
			case "$transparency_title" in
				transparent.png\ *) break ;;
				*) window_id='' ;;
			esac
		fi
		sleep 0.1
	done
	if [ -z "$window_id" ]; then
		echo "UI smoke test: transparent PNG viewer did not appear" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	sleep 0.3
	transparency_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^WIDTH=//p')
	transparency_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/transparency-white.png"
	white_sample=$(convert "$temporary/transparency-white.png" -format \
		"%[fx:p{$((transparency_width / 2)),$((transparency_height / 2))}.r > 0.98 && p{$((transparency_width / 2)),$((transparency_height / 2))}.g > 0.98 && p{$((transparency_width / 2)),$((transparency_height / 2))}.b > 0.98]" info:)
	if [ "$white_sample" != "1" ]; then
		echo "UI smoke test: transparent PNG did not composite over the configured white background ($white_sample)" >&2
		exit 1
	fi
	stop_viewer
	grep -q '^transparency_pattern=white$' \
		"$transparency_config/jpegview-linux/settings.conf"
fi

echo "UI smoke tests passed"

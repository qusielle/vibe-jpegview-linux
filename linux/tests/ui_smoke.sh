#!/bin/sh
set -eu

CDPATH=
export CDPATH
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
BINARY=${1:-./build/jpegview-linux}
ARCHIVE_FIXTURE_WRITER=${2:-}
RAR_FIXTURE_WRITER=${3:-}
GPS_EXIF_FIXTURE_WRITER=${4:-}
perf_trace_path=${JPEGVIEW_TEST_PERF_TRACE:-}
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
perf_trace_pending=0
perf_trace_active=0
if [ -n "$perf_trace_path" ]; then perf_trace_pending=1; fi
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

send_repeated_keypresses() {
	repeat_count=$1
	repeated_key=$2
	target_window=$3
	set --
	repeat_index=0
	while [ "$repeat_index" -lt "$repeat_count" ]; do
		set -- "$@" "$repeated_key"
		repeat_index=$((repeat_index + 1))
	done
	DISPLAY=":$display_number" xdotool key --clearmodifiers --window "$target_window" "$@"
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

write_gradient_ppm() {
	filename=$1
	width=$2
	height=$3
	{
		printf 'P3\n%s %s\n255\n' "$width" "$height"
		y=0
		while [ "$y" -lt "$height" ]; do
			x=0
			while [ "$x" -lt "$width" ]; do
				red=$((x * 255 / (width - 1)))
				green=$((y * 255 / (height - 1)))
				blue=$(((x + y) * 255 / (width + height - 2)))
				printf '%s %s %s\n' "$red" "$green" "$blue"
				x=$((x + 1))
			done
			y=$((y + 1))
		done
	} > "$filename"
}

gps_map_image=''
if [ -x "$GPS_EXIF_FIXTURE_WRITER" ] && command -v convert >/dev/null 2>&1; then
	write_solid_ppm "$temporary/gps-source.ppm" 35 80 120
	convert "$temporary/gps-source.ppm" "$temporary/gps-source.jpg"
	"$GPS_EXIF_FIXTURE_WRITER" "$temporary/gps-source.jpg" "$temporary/gps-map.jpg"
	gps_map_image="$temporary/gps-map.jpg"
else
	echo "UI smoke test: SKIP GPS map link interaction (missing fixture writer or ImageMagick convert)"
fi

write_ppm "$temporary/images/01-red.ppm" 255 0 0
write_ppm "$temporary/images/02-green.ppm" 0 255 0
write_ppm "$temporary/images/03-blue.ppm" 0 0 255
write_ppm "$temporary/images/04-yellow.ppm" 255 255 0
write_ppm "$temporary/images/05-cyan.ppm" 0 255 255
mkdir -p "$temporary/images/16-ordinary.zip"
write_ppm "$temporary/images/16-ordinary.zip/inside-folder.ppm" 40 80 120
touch -t 201601010000.00 "$temporary/images/16-ordinary.zip"
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
krita_project=''
if command -v zip >/dev/null 2>&1 && command -v convert >/dev/null 2>&1; then
	mkdir -p "$temporary/krita-source"
	write_solid_ppm "$temporary/krita-source/mergedimage.ppm" 20 80 140
	convert "$temporary/krita-source/mergedimage.ppm" \
		"$temporary/krita-source/mergedimage.png"
	krita_project="$temporary/images/17-flattened.kra"
	(
		cd "$temporary/krita-source"
		zip -q "$krita_project" mergedimage.png
	)
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
	if [ -n "${viewer_slow_map_target:-}" ]; then
		DISPLAY=":$display_number" HOME="$viewer_home" XDG_CONFIG_HOME="$viewer_config" \
			XDG_STATE_HOME="$XDG_STATE_HOME" PATH="$temporary/bin:$PATH" \
			LD_PRELOAD="$temporary/slow_map.so" \
			JPEGVIEW_TEST_SLOW_MAP="$viewer_slow_map_target" \
			JPEGVIEW_TEST_SLOW_MAP_REPEAT=1 \
			JPEGVIEW_TEST_SLOW_MAP_STARTED="$viewer_slow_map_started" \
			JPEGVIEW_TEST_SLOW_MAP_RELEASE="$viewer_slow_map_release" \
			JPEGVIEW_TEST_SLOW_MAP_ACTIVE="$viewer_slow_map_active" \
			JPEGVIEW_TEST_SLOW_MAP_ARMED="$viewer_slow_map_armed" \
			"$BINARY" "$viewer_input" >"$temporary/viewer.log" 2>&1 &
	elif [ "$perf_trace_pending" -eq 1 ]; then
		DISPLAY=":$display_number" HOME="$viewer_home" XDG_CONFIG_HOME="$viewer_config" \
			XDG_STATE_HOME="$XDG_STATE_HOME" PATH="$temporary/bin:$PATH" \
			JPEGVIEW_TEST_URL_LOG="$temporary/opened-url" \
			JPEGVIEW_PERF_TRACE="$perf_trace_path" \
			"$BINARY" "$viewer_input" >"$temporary/viewer.log" 2>&1 &
		perf_trace_pending=0
		perf_trace_active=1
	else
		DISPLAY=":$display_number" HOME="$viewer_home" XDG_CONFIG_HOME="$viewer_config" \
			XDG_STATE_HOME="$XDG_STATE_HOME" PATH="$temporary/bin:$PATH" \
			JPEGVIEW_TEST_URL_LOG="$temporary/opened-url" \
			"$BINARY" "$viewer_input" >"$temporary/viewer.log" 2>&1 &
	fi
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

resize_viewer_window() {
	resize_target_width=$1
	resize_target_height=$2
	DISPLAY=":$display_number" xdotool windowsize "$window_id" \
		"$resize_target_width" "$resize_target_height"
	for _ in $(seq 1 20); do
		resize_actual_geometry=$(DISPLAY=":$display_number" \
			xdotool getwindowgeometry --shell "$window_id")
		resize_actual_width=$(printf '%s\n' "$resize_actual_geometry" |
			sed -n 's/^WIDTH=//p')
		resize_actual_height=$(printf '%s\n' "$resize_actual_geometry" |
			sed -n 's/^HEIGHT=//p')
		if [ "$resize_actual_width" -eq "$resize_target_width" ] &&
			[ "$resize_actual_height" -eq "$resize_target_height" ]; then
			return 0
		fi
		sleep 0.05
	done
	return 1
}

stop_viewer() {
	DISPLAY=":$display_number" xdotool key q || true
	wait "$viewer_pid" || true
	viewer_pid=''
	if [ "$perf_trace_active" -eq 1 ]; then
		if ! awk -F, '
			NR == 1 { if ($6 != "work_class") exit 1; next }
			$2 == "source_read" && $4 == "worker_thread" &&
				$6 == "active_image_spread" { worker_source_read = 1 }
			$2 == "source_read" && $4 == "event_thread" { event_source_read = 1 }
			$2 == "processing" && $4 == "worker_thread" &&
				$6 == "active_image_spread" { worker_processing = 1 }
			$2 == "decode" && $4 == "worker_thread" &&
				$6 == "focused_preview" { preview_decode = 1 }
			$2 == "texture_upload" && $4 == "event_thread" &&
				$6 == "focused_preview" { preview_upload = 1 }
			END { exit !(worker_source_read && !event_source_read && worker_processing &&
				preview_decode && preview_upload) }
		' "$perf_trace_path"; then
			echo "UI smoke test: trace omitted worker source reads/processing or focused-preview decode/upload attribution, or recorded a source read on the event thread" >&2
			cat "$temporary/viewer.log" >&2
			exit 1
		fi
		perf_trace_active=0
	fi
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

assert_idle_frame_count_stable() {
	if [ "$perf_trace_active" -ne 1 ]; then return 0; fi
	previous=-1
	stable_samples=0
	current=0
	for _ in $(seq 1 50); do
		current=$(awk -F, '$2 == "frame_build" { count++ } END { print count + 0 }' \
			"$perf_trace_path")
		if [ "$current" -eq "$previous" ]; then
			stable_samples=$((stable_samples + 1))
		else
			stable_samples=0
		fi
		if [ "$stable_samples" -ge 4 ]; then break; fi
		previous=$current
		sleep 0.1
	done
	if [ "$current" -eq 0 ] || [ "$stable_samples" -lt 4 ]; then
		echo "UI smoke test: initial presentation did not settle before the idle redraw check" >&2
		exit 1
	fi
	sleep 0.3
	final_count=$(awk -F, '$2 == "frame_build" { count++ } END { print count + 0 }' \
		"$perf_trace_path")
	if [ "$final_count" -ne "$current" ]; then
		echo "UI smoke test: idle image continued building frames ($current to $final_count)" >&2
		exit 1
	fi
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
assert_idle_frame_count_stable
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
# Up/Down rotate the open book as one spread. Keep the mode checked and the
# partner visible while the rotated partner frame is prepared asynchronously.
DISPLAY=":$display_number" xdotool key Down
assert_title_prefix "[2-3/4] " "rotating a double-page spread fell back to one image"
if [ "$visual_assertions" -eq 1 ]; then
	rotated_spread_rendered=0
	for _ in $(seq 1 40); do
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/rotated-spread.png"
		rotated_top=$(convert "$temporary/rotated-spread.png" -format '%[hex:p{640,200}]' info:)
		rotated_bottom=$(convert "$temporary/rotated-spread.png" -format '%[hex:p{640,600}]' info:)
		case "$rotated_top:$rotated_bottom" in
			*1EDC3C*:*DC281E*) rotated_spread_rendered=1; break ;;
		esac
		sleep 0.05
	done
	if [ "$rotated_spread_rendered" -ne 1 ]; then
		echo "UI smoke test: rotating the spread did not keep both pages visible as a vertical book" >&2
		exit 1
	fi
fi
DISPLAY=":$display_number" xdotool key d
assert_title_prefix "01-first.ppm" "turning double-page mode off lost the current image"
DISPLAY=":$display_number" xdotool key d
assert_title_prefix "[2-3/4] " "turning double-page mode back on did not restore the rotated spread"
DISPLAY=":$display_number" xdotool key Up
assert_title_prefix "[2-3/4] " "counter-rotating a spread fell back to one image"
if [ "$visual_assertions" -eq 1 ]; then
	spread_restored=0
	for _ in $(seq 1 40); do
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/restored-spread.png"
		restored_left=$(convert "$temporary/restored-spread.png" -format '%[hex:p{320,400}]' info:)
		restored_right=$(convert "$temporary/restored-spread.png" -format '%[hex:p{960,400}]' info:)
		case "$restored_left:$restored_right" in
			*1EDC3C*:*DC281E*) spread_restored=1; break ;;
		esac
		sleep 0.05
	done
	if [ "$spread_restored" -ne 1 ]; then
		echo "UI smoke test: inverse rotation did not restore the side-by-side spread" >&2
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
if [ -n "$perf_trace_path" ]; then
	# Opening Browse for the active file must attribute preview decode to its
	# worker and the resulting SDL texture upload to the renderer thread.
	DISPLAY=":$display_number" xdotool key ctrl+o
	preview_trace_seen=0
	for _ in $(seq 1 40); do
		if awk -F, '
			$2 == "decode" && $4 == "worker_thread" && $6 == "focused_preview" { preview_decode = 1 }
			$2 == "texture_upload" && $4 == "event_thread" && $6 == "focused_preview" { preview_upload = 1 }
			END { exit !(preview_decode && preview_upload) }
		' "$perf_trace_path" 2>/dev/null; then
			preview_trace_seen=1
			break
		fi
		sleep 0.05
	done
	if [ "$preview_trace_seen" -ne 1 ]; then
		echo "UI smoke test: Browse preview decode/upload trace rows did not arrive before the bounded deadline" >&2
		cat "$temporary/viewer.log" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool key Escape
	sleep 0.1
fi
stop_viewer

if [ -n "$gps_map_image" ]; then
	gps_config="$temporary/gps-map-config"
	gps_previous_state_home=$XDG_STATE_HOME
	XDG_STATE_HOME="$temporary/gps-map-state"
	export XDG_STATE_HOME
	mkdir -p "$gps_config/jpegview-linux"
	cat >"$gps_config/jpegview-linux/settings.conf" <<'EOF'
scale_mode=fit
sort_mode=file_name
double_page_mode_enabled=0
thumbnail_panel_visible=0
show_filename=0
info_visible=1
gps_map_provider_url=https://maps.example.test/?lat={lat}&lon={lng}
EOF
	: > "$temporary/opened-url"
	VIEWER_TEST_HOME="$temporary/gps-home" \
		VIEWER_TEST_CONFIG_HOME="$gps_config" launch_viewer "$gps_map_image"
	gps_map_opened=0
	expected_gps_map_url='https://maps.example.test/?lat=-12.58222&lon=-98.11833'
	for _ in $(seq 1 60); do
		DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 20 70 click 1
		if [ -f "$temporary/opened-url" ] &&
			[ "$(cat "$temporary/opened-url")" = "$expected_gps_map_url" ]; then
			gps_map_opened=1
			break
		fi
		sleep 0.1
	done
	if [ "$gps_map_opened" -ne 1 ]; then
		echo "UI smoke test: clicking the EXIF GPS location did not open the configured map URL" >&2
		cat "$temporary/viewer.log" >&2
		cat "$temporary/opened-url" >&2
		exit 1
	fi
	stop_viewer
	grep -Fq 'gps_map_provider_url=https://maps.example.test/?lat={lat}&lon={lng}' \
		"$gps_config/jpegview-linux/settings.conf"
	unset VIEWER_TEST_HOME VIEWER_TEST_CONFIG_HOME
	XDG_STATE_HOME=$gps_previous_state_home
	export XDG_STATE_HOME
fi

# Go-to input is one-based, replaces its prefilled current value when typed,
# and keeps the selected image unchanged when the requested index is invalid.
launch_viewer "$temporary/images"
assert_title_prefix "01-red.ppm" "go-to fixture did not start on the first image"
DISPLAY=":$display_number" xdotool key --window "$window_id" ctrl+g
DISPLAY=":$display_number" xdotool key --window "$window_id" ctrl+a
DISPLAY=":$display_number" xdotool type --window "$window_id" --clearmodifiers 3
DISPLAY=":$display_number" xdotool key --window "$window_id" Return
assert_title_prefix "03-blue.ppm" "Ctrl+G did not select the requested one-based image number"
DISPLAY=":$display_number" xdotool key --window "$window_id" ctrl+g
DISPLAY=":$display_number" xdotool key --window "$window_id" ctrl+a
DISPLAY=":$display_number" xdotool type --window "$window_id" --clearmodifiers 0
DISPLAY=":$display_number" xdotool key --window "$window_id" Return
DISPLAY=":$display_number" xdotool key --window "$window_id" Escape
assert_title_prefix "03-blue.ppm" "invalid go-to input changed the selected image"
stop_viewer

# The full Transform image menu opens the free-rotation editor, Escape cancels
# without changing the selected document, and Apply commits an adjusted angle.
rotation_previous_state=$XDG_STATE_HOME
rotation_previous_config_set=${VIEWER_TEST_CONFIG_HOME+x}
rotation_previous_config=${VIEWER_TEST_CONFIG_HOME-}
XDG_STATE_HOME="$temporary/free-rotation-state"
VIEWER_TEST_CONFIG_HOME="$temporary/free-rotation-config"
export XDG_STATE_HOME VIEWER_TEST_CONFIG_HOME
rotation_image="$temporary/free-rotation-checker.ppm"
printf 'P6\n2 2\n255\n\377\000\000\000\377\000\000\000\377\377\377\000' > "$rotation_image"
launch_viewer "$rotation_image"
rotation_title_before=$(DISPLAY=":$display_number" window_title_without_position)
if [ "$visual_assertions" -eq 1 ]; then
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/free-rotation-original.png"
fi
DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" > "$temporary/free-rotation-geometry"
rotation_window_width=$(awk -F= '$1 == "WIDTH" { print $2 }' "$temporary/free-rotation-geometry")
rotation_window_height=$(awk -F= '$1 == "HEIGHT" { print $2 }' "$temporary/free-rotation-geometry")
rotation_dialog_border_x=$((rotation_window_width / 2 - 260))
rotation_dialog_border_y=$((rotation_window_height / 2 - 155))
rotation_slider_x=$((rotation_window_width / 2 + 60))
rotation_slider_y=$((rotation_window_height / 2 - 66))
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 400
DISPLAY=":$display_number" xdotool keydown Shift_L
DISPLAY=":$display_number" xdotool click 3
DISPLAY=":$display_number" xdotool keyup Shift_L
DISPLAY=":$display_number" xdotool key --delay 30 t t t
if [ "$visual_assertions" -eq 1 ]; then
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/free-rotation-menu.png"
	DISPLAY=":$display_number" xdotool key Return
	rotation_editor_opened=0
	for _ in $(seq 1 40); do
		sleep 0.05
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/free-rotation-editor.png"
		rotation_dialog_border=$(convert "$temporary/free-rotation-editor.png" \
			-format "%[pixel:p{$rotation_dialog_border_x,$rotation_dialog_border_y}]" info:-)
		if [ "$rotation_dialog_border" = 'srgb(200,210,225)' ]; then
			rotation_editor_opened=1
			break
		fi
	done
	if [ "$rotation_editor_opened" -ne 1 ]; then
		echo "UI smoke test: Transform image did not render the free-rotation editor" >&2
		exit 1
	fi
else
	DISPLAY=":$display_number" xdotool key Return
fi
DISPLAY=":$display_number" xdotool key Escape
assert_title_prefix "$rotation_title_before" "canceling free rotation changed the current image title"
if [ "$visual_assertions" -eq 1 ]; then
	rotation_editor_closed=0
	for _ in $(seq 1 40); do
		sleep 0.05
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/free-rotation-canceled.png"
		rotation_dialog_border=$(convert "$temporary/free-rotation-canceled.png" \
			-format "%[pixel:p{$rotation_dialog_border_x,$rotation_dialog_border_y}]" info:-)
		if [ "$rotation_dialog_border" != 'srgb(200,210,225)' ]; then
			rotation_editor_closed=1
			break
		fi
	done
	if [ "$rotation_editor_closed" -ne 1 ]; then
		echo "UI smoke test: Escape did not close the free-rotation editor" >&2
		exit 1
	fi
fi
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 400
DISPLAY=":$display_number" xdotool keydown Shift_L
DISPLAY=":$display_number" xdotool click 3
DISPLAY=":$display_number" xdotool keyup Shift_L
DISPLAY=":$display_number" xdotool key --delay 30 t t t
sleep 0.05
DISPLAY=":$display_number" xdotool key Return
if [ "$visual_assertions" -eq 1 ]; then
	rotation_editor_reopened=0
	for _ in $(seq 1 40); do
		sleep 0.05
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/free-rotation-reopened.png"
		rotation_dialog_border=$(convert "$temporary/free-rotation-reopened.png" \
			-format "%[pixel:p{$rotation_dialog_border_x,$rotation_dialog_border_y}]" info:-)
		if [ "$rotation_dialog_border" = 'srgb(200,210,225)' ]; then
			rotation_editor_reopened=1
			break
		fi
	done
	if [ "$rotation_editor_reopened" -ne 1 ]; then
		echo "UI smoke test: free-rotation editor could not be reopened after cancel" >&2
		exit 1
	fi
fi
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
	"$rotation_slider_x" "$rotation_slider_y" click 1
DISPLAY=":$display_number" xdotool key Return
if [ "$visual_assertions" -eq 1 ]; then
	rotation_editor_applied=0
	for _ in $(seq 1 60); do
		sleep 0.05
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/free-rotation-applied.png"
		rotation_dialog_border=$(convert "$temporary/free-rotation-applied.png" \
			-format "%[pixel:p{$rotation_dialog_border_x,$rotation_dialog_border_y}]" info:-)
		if [ "$rotation_dialog_border" != 'srgb(200,210,225)' ]; then
			rotation_editor_applied=1
			break
		fi
	done
	if [ "$rotation_editor_applied" -ne 1 ]; then
		echo "UI smoke test: free-rotation Apply did not close after committing the transformed pixels" >&2
		cat "$temporary/viewer.log" >&2
		exit 1
	fi
	rotation_crop_width=$(identify -format '%w' "$temporary/free-rotation-original.png")
	rotation_crop_height=$(identify -format '%h' "$temporary/free-rotation-original.png")
	rotation_crop_width=$((rotation_crop_width * 3 / 4))
	rotation_crop_height=$((rotation_crop_height * 3 / 4))
	rotation_crop_x=$((( $(identify -format '%w' "$temporary/free-rotation-original.png") - rotation_crop_width ) / 2))
	rotation_crop_y=$((( $(identify -format '%h' "$temporary/free-rotation-original.png") - rotation_crop_height ) / 2))
	convert "$temporary/free-rotation-original.png" -crop \
		"${rotation_crop_width}x${rotation_crop_height}+${rotation_crop_x}+${rotation_crop_y}" \
		+repage "$temporary/free-rotation-original-image-area.png"
	convert "$temporary/free-rotation-applied.png" -crop \
		"${rotation_crop_width}x${rotation_crop_height}+${rotation_crop_x}+${rotation_crop_y}" \
		+repage "$temporary/free-rotation-applied-image-area.png"
	rotation_image_difference=$(compare -metric AE \
		"$temporary/free-rotation-original-image-area.png" \
		"$temporary/free-rotation-applied-image-area.png" null: 2>&1 || true)
	if [ "$rotation_image_difference" = "0" ]; then
		echo "UI smoke test: free-rotation Apply closed without changing the displayed pixels" >&2
		exit 1
	fi
fi
stop_viewer

# Perspective correction is available from the full transform menu. Cancel
# preserves the image; changing an edge and applying updates the displayed
# document through the asynchronous operation path.
perspective_image="$temporary/perspective-gradient.ppm"
write_gradient_ppm "$perspective_image" 64 48
launch_viewer "$perspective_image"
if ! resize_viewer_window 640 240; then
	cat "$temporary/viewer.log" >&2
	echo "UI smoke test: viewer did not resize to the compact perspective-editor test size" >&2
	exit 1
fi
DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" \
	> "$temporary/perspective-geometry"
perspective_title_before=$(DISPLAY=":$display_number" window_title_without_position)
if [ "$visual_assertions" -eq 1 ]; then
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 10 10
	DISPLAY=":$display_number" import -window "$window_id" \
		"$temporary/perspective-original.png"
fi
DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" \
	> "$temporary/perspective-geometry"
perspective_window_width=$(awk -F= '$1 == "WIDTH" { print $2 }' \
	"$temporary/perspective-geometry")
perspective_window_height=$(awk -F= '$1 == "HEIGHT" { print $2 }' \
	"$temporary/perspective-geometry")
perspective_dialog_width=$((perspective_window_width - 32))
perspective_dialog_height=$((perspective_window_height - 16))
if [ "$perspective_dialog_width" -gt 620 ]; then perspective_dialog_width=620; fi
if [ "$perspective_dialog_height" -gt 390 ]; then perspective_dialog_height=390; fi
perspective_dialog_border_x=$(((perspective_window_width - perspective_dialog_width) / 2))
perspective_dialog_border_y=$(((perspective_window_height - perspective_dialog_height) / 2))
open_perspective_menu() {
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 40 40
	DISPLAY=":$display_number" xdotool keydown Shift_L
	DISPLAY=":$display_number" xdotool click 3
	DISPLAY=":$display_number" xdotool keyup Shift_L
	# In the advanced menu V cycles from the navigation-panel command to the
	# perspective entry; Return activates the selected command.
	DISPLAY=":$display_number" xdotool key --delay 30 v v
	DISPLAY=":$display_number" xdotool key Return
}
perspective_editor_opened=0
open_perspective_menu
if [ "$visual_assertions" -eq 1 ]; then
	for _ in $(seq 1 40); do
		sleep 0.05
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/perspective-editor.png"
		perspective_border=$(convert "$temporary/perspective-editor.png" \
			-format "%[pixel:p{$perspective_dialog_border_x,$perspective_dialog_border_y}]" info:-)
		if [ "$perspective_border" = 'srgb(200,210,225)' ]; then
			perspective_editor_opened=1
			break
		fi
	done
	if [ "$perspective_editor_opened" -ne 1 ]; then
		echo "UI smoke test: Perspective correction did not open from the full transform menu" >&2
		cat "$temporary/viewer.log" >&2
		exit 1
	fi
fi
DISPLAY=":$display_number" xdotool key Escape
assert_title_prefix "$perspective_title_before" \
	"canceling perspective correction changed the current image title"
if [ "$visual_assertions" -eq 1 ]; then
	perspective_editor_closed=0
	for _ in $(seq 1 40); do
		sleep 0.05
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/perspective-canceled.png"
		perspective_border=$(convert "$temporary/perspective-canceled.png" \
			-format "%[pixel:p{$perspective_dialog_border_x,$perspective_dialog_border_y}]" info:-)
		if [ "$perspective_border" != 'srgb(200,210,225)' ]; then
			perspective_editor_closed=1
			break
		fi
	done
	if [ "$perspective_editor_closed" -ne 1 ]; then
		echo "UI smoke test: Escape did not close the perspective-correction editor" >&2
		exit 1
	fi
fi
open_perspective_menu
if [ "$visual_assertions" -eq 1 ]; then
	perspective_editor_reopened=0
	for _ in $(seq 1 40); do
		sleep 0.05
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/perspective-reopened.png"
		perspective_border=$(convert "$temporary/perspective-reopened.png" \
			-format "%[pixel:p{$perspective_dialog_border_x,$perspective_dialog_border_y}]" info:-)
		if [ "$perspective_border" = 'srgb(200,210,225)' ]; then
			perspective_editor_reopened=1
			break
		fi
	done
	if [ "$perspective_editor_reopened" -ne 1 ]; then
		echo "UI smoke test: perspective-correction editor could not be reopened after cancel" >&2
		cat "$temporary/viewer.log" >&2
		exit 1
	fi
fi
if ! resize_viewer_window 160 120; then
	cat "$temporary/viewer.log" >&2
	echo "UI smoke test: perspective editor did not remain open through a 160x120 resize" >&2
	exit 1
fi
perspective_dialog_border_x=0
perspective_dialog_border_y=0
perspective_dialog_width=160
perspective_dialog_height=120
perspective_cancel_x=119
perspective_button_y=106
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
	"$perspective_cancel_x" "$perspective_button_y" click 1
if [ "$visual_assertions" -eq 1 ]; then
	perspective_mouse_cancel_closed=0
	for _ in $(seq 1 40); do
		sleep 0.05
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/perspective-mouse-canceled.png"
		perspective_border=$(convert "$temporary/perspective-mouse-canceled.png" \
			-format "%[pixel:p{$perspective_dialog_border_x,$perspective_dialog_border_y}]" info:-)
		if [ "$perspective_border" != 'srgb(200,210,225)' ]; then
			perspective_mouse_cancel_closed=1
			break
		fi
	done
	if [ "$perspective_mouse_cancel_closed" -ne 1 ]; then
		echo "UI smoke test: compact perspective Cancel button was not mouse-accessible" >&2
		exit 1
	fi
fi
assert_title_prefix "$perspective_title_before" \
	"clicking compact perspective Cancel changed the current image title"
if ! resize_viewer_window 640 240; then
	cat "$temporary/viewer.log" >&2
	echo "UI smoke test: viewer did not restore the perspective test window size" >&2
	exit 1
fi
perspective_dialog_border_x=16
perspective_dialog_border_y=8
perspective_dialog_width=608
perspective_dialog_height=224
perspective_button_y=$((perspective_dialog_border_y + perspective_dialog_height - 43 + 15))
open_perspective_menu
DISPLAY=":$display_number" xdotool key --repeat 12 --delay 20 Right
perspective_apply_x=$((perspective_dialog_border_x + perspective_dialog_width - 198 + 43))
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
	"$perspective_apply_x" "$perspective_button_y" click 1
if [ "$visual_assertions" -eq 1 ]; then
	perspective_editor_applied=0
	for _ in $(seq 1 80); do
		sleep 0.05
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/perspective-applied.png"
		perspective_border=$(convert "$temporary/perspective-applied.png" \
			-format "%[pixel:p{$perspective_dialog_border_x,$perspective_dialog_border_y}]" info:-)
		if [ "$perspective_border" != 'srgb(200,210,225)' ]; then
			perspective_editor_applied=1
			break
		fi
	done
	if [ "$perspective_editor_applied" -ne 1 ]; then
		echo "UI smoke test: Perspective correction Apply did not close after committing" >&2
		cat "$temporary/viewer.log" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 10 10
	DISPLAY=":$display_number" import -window "$window_id" \
		"$temporary/perspective-applied-final.png"
	perspective_image_difference=$(compare -metric AE \
		"$temporary/perspective-original.png" \
		"$temporary/perspective-applied-final.png" null: 2>&1 || true)
	if [ "$perspective_image_difference" = "0" ]; then
		echo "UI smoke test: Perspective correction Apply closed without changing the displayed pixels" >&2
		exit 1
	fi
fi
stop_viewer

# Cancel a lazy-JPEG rotation while its source decode is held, then verify the
# late decode restores presentation readiness. A zero-degree Apply also leaves
# the document unchanged and playback can start afterwards.
if command -v cc >/dev/null 2>&1 && command -v convert >/dev/null 2>&1; then
	cc -shared -fPIC "$SCRIPT_DIR/delay_mmap.c" -o "$temporary/slow_map.so" -ldl -pthread
	rotation_lazy_directory="$temporary/free-rotation-lazy"
	rotation_lazy_config="$temporary/free-rotation-lazy-config"
	mkdir -p "$rotation_lazy_directory" "$rotation_lazy_config/jpegview-linux"
	convert -size 1800x1200 xc:black -quality 90 "$rotation_lazy_directory/01-lazy.jpg"
	convert -size 1800x1200 xc:white -quality 90 "$rotation_lazy_directory/02-next.jpg"
	printf 'scale_mode=fit\ncache_size_mb=64\nthumbnail_panel_visible=0\nwrap_around_folder=0\n' \
		> "$rotation_lazy_config/jpegview-linux/settings.conf"
	viewer_slow_map_target="$rotation_lazy_directory/01-lazy.jpg"
	viewer_slow_map_started="$temporary/free-rotation-decode.started"
	viewer_slow_map_release="$temporary/free-rotation-decode.release"
	viewer_slow_map_active="$temporary/free-rotation-decode.active"
	viewer_slow_map_armed="$temporary/free-rotation-decode.armed"
	rm -f "$viewer_slow_map_started" "$viewer_slow_map_release" \
		"$viewer_slow_map_active" "$viewer_slow_map_armed"
	VIEWER_TEST_HOME="$temporary/free-rotation-lazy-home" \
		VIEWER_TEST_CONFIG_HOME="$rotation_lazy_config" \
		launch_viewer "$rotation_lazy_directory/01-lazy.jpg"
	assert_title_prefix "01-lazy.jpg" "lazy-JPEG rotation fixture did not load"
	rotation_lazy_ready=0
	for _ in $(seq 1 40); do
		rotation_lazy_title=$(DISPLAY=":$display_number" window_title_without_position)
		case "$rotation_lazy_title" in
			*Loading*|*Preparing*) sleep 0.05 ;;
			*) rotation_lazy_ready=1; break ;;
		esac
	done
	if [ "$rotation_lazy_ready" -ne 1 ]; then
		echo "UI smoke test: lazy-JPEG rotation fixture did not finish its initial presentation" >&2
		cat "$temporary/viewer.log" >&2
		exit 1
	fi
	rotation_lazy_digest=$(sha256sum "$rotation_lazy_directory/01-lazy.jpg" | awk '{print $1}')
	: > "$viewer_slow_map_armed"
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 400
	DISPLAY=":$display_number" xdotool keydown Shift_L
	DISPLAY=":$display_number" xdotool click 3
	DISPLAY=":$display_number" xdotool keyup Shift_L
	DISPLAY=":$display_number" xdotool key --delay 30 t t t
	DISPLAY=":$display_number" xdotool key Return
	rotation_decode_blocked=0
	for _ in $(seq 1 100); do
		if [ -f "$viewer_slow_map_started" ]; then rotation_decode_blocked=1; break; fi
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.05
	done
	if [ "$rotation_decode_blocked" -ne 1 ]; then
		: > "$viewer_slow_map_release"
		rm -f "$viewer_slow_map_armed"
		echo "UI smoke test: free-rotation source decode did not reach its controlled barrier" >&2
		cat "$temporary/viewer.log" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool key Escape
	assert_title_prefix "01-lazy.jpg" "canceling lazy free rotation changed the selected image"
	: > "$viewer_slow_map_release"
	rm -f "$viewer_slow_map_armed"
	for _ in $(seq 1 100); do
		if [ ! -e "$viewer_slow_map_active" ]; then break; fi
		sleep 0.05
	done
	sleep 0.75
	viewer_slow_map_target=''
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 400
	DISPLAY=":$display_number" xdotool keydown Shift_L
	DISPLAY=":$display_number" xdotool click 3
	DISPLAY=":$display_number" xdotool keyup Shift_L
	DISPLAY=":$display_number" xdotool key --delay 30 t t t
	sleep 0.05
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.5
	DISPLAY=":$display_number" xdotool key Return
	assert_title_prefix "01-lazy.jpg" "zero-degree Apply changed the selected image"
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 400
	DISPLAY=":$display_number" xdotool keydown Shift_L
	DISPLAY=":$display_number" xdotool click 3
	DISPLAY=":$display_number" xdotool keyup Shift_L
	DISPLAY=":$display_number" xdotool key --delay 30 t t t
	sleep 0.05
	DISPLAY=":$display_number" xdotool key Return
	sleep 0.5
	DISPLAY=":$display_number" xdotool key Escape
	assert_title_prefix "01-lazy.jpg" "canceling a decoded lazy-JPEG rotation changed the selection"
	rotation_lazy_digest_after=$(sha256sum "$rotation_lazy_directory/01-lazy.jpg" | awk '{print $1}')
	if [ "$rotation_lazy_digest_after" != "$rotation_lazy_digest" ]; then
		echo "UI smoke test: canceled or zero-degree free rotation changed the JPEG source" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool key alt+r
	assert_title_prefix "[2/2] " "playback remained unready after canceled lazy rotation decode"
	stop_viewer
	if [ "$visual_assertions" -eq 1 ]; then
		# A detected source replacement invalidates the modal session as well
		# as the pending Apply, rather than retaining an old owner's editor.
		viewer_slow_map_target="$rotation_lazy_directory/01-lazy.jpg"
		rm -f "$viewer_slow_map_started" "$viewer_slow_map_release" \
			"$viewer_slow_map_active" "$viewer_slow_map_armed"
		VIEWER_TEST_HOME="$temporary/free-rotation-refresh-home" \
			VIEWER_TEST_CONFIG_HOME="$rotation_lazy_config" \
			launch_viewer "$rotation_lazy_directory/01-lazy.jpg"
		assert_title_prefix "01-lazy.jpg" "source-refresh rotation fixture did not load"
		rotation_refresh_ready=0
		for _ in $(seq 1 40); do
			rotation_refresh_title=$(DISPLAY=":$display_number" window_title_without_position)
			case "$rotation_refresh_title" in
				*Loading*|*Preparing*) sleep 0.05 ;;
				*) rotation_refresh_ready=1; break ;;
			esac
		done
		if [ "$rotation_refresh_ready" -ne 1 ]; then
			echo "UI smoke test: source-refresh fixture did not finish its initial presentation" >&2
			exit 1
		fi
		rotation_refresh_geometry=$(DISPLAY=":$display_number" \
			xdotool getwindowgeometry --shell "$window_id")
		rotation_refresh_width=$(printf '%s\n' "$rotation_refresh_geometry" |
			sed -n 's/^WIDTH=//p')
		rotation_refresh_height=$(printf '%s\n' "$rotation_refresh_geometry" |
			sed -n 's/^HEIGHT=//p')
		rotation_refresh_border_x=$((rotation_refresh_width / 2 - 260))
		rotation_refresh_border_y=$((rotation_refresh_height / 2 - 155))
		: > "$viewer_slow_map_armed"
		DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 400
		DISPLAY=":$display_number" xdotool keydown Shift_L
		DISPLAY=":$display_number" xdotool click 3
		DISPLAY=":$display_number" xdotool keyup Shift_L
		DISPLAY=":$display_number" xdotool key --delay 30 t t t Return
		rotation_refresh_blocked=0
		for _ in $(seq 1 100); do
			if [ -f "$viewer_slow_map_started" ]; then rotation_refresh_blocked=1; break; fi
			if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
			sleep 0.05
		done
		if [ "$rotation_refresh_blocked" -ne 1 ]; then
			: > "$viewer_slow_map_release"
			rm -f "$viewer_slow_map_armed"
			echo "UI smoke test: source-refresh rotation did not reach its controlled barrier" >&2
			exit 1
		fi
		DISPLAY=":$display_number" xdotool key Right Return
		touch "$rotation_lazy_directory/01-lazy.jpg"
		: > "$viewer_slow_map_release"
		rm -f "$viewer_slow_map_armed"
		rotation_refresh_closed=0
		for _ in $(seq 1 100); do
			sleep 0.05
			DISPLAY=":$display_number" import -window "$window_id" \
				"$temporary/free-rotation-refreshed.png"
			rotation_refresh_border=$(convert "$temporary/free-rotation-refreshed.png" \
				-format "%[pixel:p{$rotation_refresh_border_x,$rotation_refresh_border_y}]" info:-)
			if [ "$rotation_refresh_border" != 'srgb(200,210,225)' ]; then
				rotation_refresh_closed=1
				break
			fi
		done
		if [ "$rotation_refresh_closed" -ne 1 ]; then
			echo "UI smoke test: source refresh retained the previous owner's rotation editor" >&2
			cat "$temporary/viewer.log" >&2
			exit 1
		fi
		assert_title_prefix "01-lazy.jpg" "source refresh changed the selected image"
		DISPLAY=":$display_number" xdotool key alt+r
		assert_title_prefix "[2/2] " "source refresh left rotation playback suppression active"
		stop_viewer
	fi
	viewer_slow_map_target=''
	unset viewer_slow_map_started viewer_slow_map_release viewer_slow_map_active \
		viewer_slow_map_armed
fi

# Playback deadlines are suppressed during the modal editor even if a cached
# display completion restores image readiness while the editor is open.
if command -v convert >/dev/null 2>&1 && [ "$visual_assertions" -eq 1 ]; then
	rotation_animation_directory="$temporary/free-rotation-animation"
	rotation_animation_config="$temporary/free-rotation-animation-config"
	mkdir -p "$rotation_animation_directory" "$rotation_animation_config/jpegview-linux"
	convert -delay 300 -size 800x600 xc:red -delay 300 -size 800x600 xc:blue \
		-loop 0 "$rotation_animation_directory/01-animation.gif"
	printf 'scale_mode=fit\ncache_size_mb=0\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
		> "$rotation_animation_config/jpegview-linux/settings.conf"
	VIEWER_TEST_HOME="$temporary/free-rotation-animation-home" \
		VIEWER_TEST_CONFIG_HOME="$rotation_animation_config" \
		launch_viewer "$rotation_animation_directory/01-animation.gif"
	assert_title_prefix "01-animation.gif" "free-rotation animation fixture did not load"
	rotation_animation_ready=0
	for _ in $(seq 1 40); do
		rotation_animation_title=$(DISPLAY=":$display_number" window_title_without_position)
		case "$rotation_animation_title" in
			*Loading*|*Preparing*) sleep 0.05 ;;
			*) rotation_animation_ready=1; break ;;
		esac
	done
	if [ "$rotation_animation_ready" -ne 1 ]; then
		echo "UI smoke test: animated image did not finish its initial presentation" >&2
		cat "$temporary/viewer.log" >&2
		exit 1
	fi
	rotation_animation_geometry=$(DISPLAY=":$display_number" \
		xdotool getwindowgeometry --shell "$window_id")
	rotation_animation_width=$(printf '%s\n' "$rotation_animation_geometry" |
		sed -n 's/^WIDTH=//p')
	rotation_animation_height=$(printf '%s\n' "$rotation_animation_geometry" |
		sed -n 's/^HEIGHT=//p')
	rotation_animation_sample_x=$((rotation_animation_width / 2 - 310))
	rotation_animation_sample_y=$((rotation_animation_height / 2))
	rotation_animation_border_x=$((rotation_animation_width / 2 - 260))
	rotation_animation_border_y=$((rotation_animation_height / 2 - 155))
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		"$rotation_animation_sample_x" "$rotation_animation_sample_y"
	DISPLAY=":$display_number" import -window "$window_id" \
		"$temporary/free-rotation-animation-before.png"
	rotation_animation_before_color=$(convert \
		"$temporary/free-rotation-animation-before.png" \
		-format "%[hex:p{$rotation_animation_sample_x,$rotation_animation_sample_y}]" info:)
	case "$rotation_animation_before_color" in
		FF0000|0000FF) ;;
		*) echo "UI smoke test: animation fixture did not show a solid starting frame ($rotation_animation_before_color)" >&2; exit 1 ;;
	esac
	rotation_animation_original_color=$rotation_animation_before_color
	DISPLAY=":$display_number" xdotool key p
	sleep 0.45
	DISPLAY=":$display_number" import -window "$window_id" \
		"$temporary/free-rotation-animation-frozen-before.png"
	rotation_animation_frozen_before=$(convert \
		"$temporary/free-rotation-animation-frozen-before.png" \
		-format "%[hex:p{$rotation_animation_sample_x,$rotation_animation_sample_y}]" info:)
	sleep 0.45
	DISPLAY=":$display_number" import -window "$window_id" \
		"$temporary/free-rotation-animation-frozen-after.png"
	rotation_animation_frozen_after=$(convert \
		"$temporary/free-rotation-animation-frozen-after.png" \
		-format "%[hex:p{$rotation_animation_sample_x,$rotation_animation_sample_y}]" info:)
	if [ "$rotation_animation_frozen_before" != "$rotation_animation_frozen_after" ]; then
		echo "UI smoke test: P did not freeze the displayed animation frame" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool key bracketright
	rotation_animation_stepped_color=''
	for _ in $(seq 1 40); do
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/free-rotation-animation-next-frame.png"
		rotation_animation_stepped_color=$(convert \
			"$temporary/free-rotation-animation-next-frame.png" \
			-format "%[hex:p{$rotation_animation_sample_x,$rotation_animation_sample_y}]" info:)
		if [ "$rotation_animation_stepped_color" != "$rotation_animation_frozen_after" ]; then
			break
		fi
		sleep 0.05
	done
	if [ "$rotation_animation_stepped_color" = "$rotation_animation_frozen_after" ]; then
		echo "UI smoke test: ] did not step to the next animation frame" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool key bracketleft
	rotation_animation_reversed_color=''
	for _ in $(seq 1 40); do
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/free-rotation-animation-previous-frame.png"
		rotation_animation_reversed_color=$(convert \
			"$temporary/free-rotation-animation-previous-frame.png" \
			-format "%[hex:p{$rotation_animation_sample_x,$rotation_animation_sample_y}]" info:)
		if [ "$rotation_animation_reversed_color" = "$rotation_animation_original_color" ]; then
			break
		fi
		sleep 0.05
	done
	if [ "$rotation_animation_reversed_color" != "$rotation_animation_original_color" ]; then
		echo "UI smoke test: [ did not reverse to the previous animation frame" >&2
		exit 1
	fi
	assert_title_prefix "01-animation.gif" "frame stepping changed the selected file"
	DISPLAY=":$display_number" xdotool key p
	rotation_animation_resumed_color=''
	for _ in $(seq 1 70); do
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/free-rotation-animation-control-resume.png"
		rotation_animation_resumed_color=$(convert \
			"$temporary/free-rotation-animation-control-resume.png" \
			-format "%[hex:p{$rotation_animation_sample_x,$rotation_animation_sample_y}]" info:)
		if [ "$rotation_animation_resumed_color" != "$rotation_animation_original_color" ]; then
			break
		fi
		sleep 0.05
	done
	if [ "$rotation_animation_resumed_color" = "$rotation_animation_original_color" ]; then
		echo "UI smoke test: P did not resume animation after manual frame stepping" >&2
		exit 1
	fi
	rotation_animation_before_color=$rotation_animation_resumed_color
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 400
	DISPLAY=":$display_number" xdotool keydown Shift_L
	DISPLAY=":$display_number" xdotool click 3
	DISPLAY=":$display_number" xdotool keyup Shift_L
	sleep 0.05
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 560 192 click 1
	rotation_animation_editor_opened=0
	for _ in $(seq 1 40); do
		sleep 0.05
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/free-rotation-animation-editor.png"
		rotation_animation_border=$(convert \
			"$temporary/free-rotation-animation-editor.png" \
			-format "%[pixel:p{$rotation_animation_border_x,$rotation_animation_border_y}]" info:-)
		if [ "$rotation_animation_border" = 'srgb(200,210,225)' ]; then
			rotation_animation_editor_opened=1
			break
		fi
	done
	if [ "$rotation_animation_editor_opened" -ne 1 ]; then
		echo "UI smoke test: free rotation did not open for an animated image" >&2
		cat "$temporary/viewer.log" >&2
		exit 1
	fi
	sleep 3.5
	DISPLAY=":$display_number" xdotool key Escape
	sleep 0.15
	DISPLAY=":$display_number" import -window "$window_id" \
		"$temporary/free-rotation-animation-after-modal.png"
	rotation_animation_after_color=$(convert \
		"$temporary/free-rotation-animation-after-modal.png" \
		-format "%[hex:p{$rotation_animation_sample_x,$rotation_animation_sample_y}]" info:)
	if [ "$rotation_animation_after_color" != "$rotation_animation_before_color" ]; then
		echo "UI smoke test: animation advanced while free rotation was open ($rotation_animation_before_color:$rotation_animation_after_color)" >&2
		exit 1
	fi
	rotation_animation_resumed=0
	for _ in $(seq 1 70); do
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/free-rotation-animation-resumed.png"
		rotation_animation_resumed_color=$(convert \
			"$temporary/free-rotation-animation-resumed.png" \
			-format "%[hex:p{$rotation_animation_sample_x,$rotation_animation_sample_y}]" info:)
		if [ "$rotation_animation_resumed_color" != "$rotation_animation_after_color" ]; then
			rotation_animation_resumed=1
			break
		fi
		sleep 0.05
	done
	if [ "$rotation_animation_resumed" -ne 1 ]; then
		echo "UI smoke test: animation did not resume from its retained frame after rotation" >&2
		exit 1
	fi
	stop_viewer
fi

XDG_STATE_HOME=$rotation_previous_state
export XDG_STATE_HOME
if [ -n "$rotation_previous_config_set" ]; then
	VIEWER_TEST_CONFIG_HOME=$rotation_previous_config
	export VIEWER_TEST_CONFIG_HOME
else
	unset VIEWER_TEST_CONFIG_HOME
fi

# A Krita project is a regular image input and presents its embedded flattened
# image through the normal decoder path.
if [ -n "$krita_project" ]; then
	launch_viewer "$krita_project"
	assert_title_prefix "17-flattened.kra" "Krita input did not open as a single image"
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/krita-capture.png"
		krita_geometry=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id")
		krita_width=$(printf '%s\n' "$krita_geometry" | sed -n 's/^WIDTH=//p')
		krita_height=$(printf '%s\n' "$krita_geometry" | sed -n 's/^HEIGHT=//p')
		krita_center=$(convert "$temporary/krita-capture.png" \
			-format "%[hex:p{$((krita_width / 2)),$((krita_height / 2))}]" info:)
		case "$krita_center" in
			*14508C*) ;;
			*) echo "UI smoke test: Krita project did not render its flattened image ($krita_center)" >&2; exit 1 ;;
		esac
	fi
	stop_viewer
fi

# The pixel color sampler is off by default; when enabled, its DOC readout
# samples a decoded pixel in RGBA order and copies its value when clicked.
pixel_sampler_directory="$temporary/pixel-sampler"
mkdir -p "$pixel_sampler_directory"
pixel_sampler_previous_state=$XDG_STATE_HOME
pixel_sampler_previous_config_set=${VIEWER_TEST_CONFIG_HOME+x}
pixel_sampler_previous_config=${VIEWER_TEST_CONFIG_HOME-}
XDG_STATE_HOME="$temporary/pixel-sampler-state"
VIEWER_TEST_CONFIG_HOME="$temporary/pixel-sampler-default-config"
export VIEWER_TEST_CONFIG_HOME
export XDG_STATE_HOME
printf 'P3\n2 2\n255\n18 52 86 18 52 86 18 52 86 18 52 86\n' \
	> "$pixel_sampler_directory/01-sampler.ppm"
launch_viewer "$pixel_sampler_directory"
assert_title_prefix "01-sampler.ppm" "pixel-sampler fixture did not load"
sampler_geometry=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id")
sampler_window_width=$(printf '%s\n' "$sampler_geometry" | sed -n 's/^WIDTH=//p')
sampler_window_height=$(printf '%s\n' "$sampler_geometry" | sed -n 's/^HEIGHT=//p')
sampler_x=$((sampler_window_width / 2))
sampler_y=$((sampler_window_height / 2))
clear_clipboard_text
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" "$sampler_x" "$sampler_y"
sleep 0.15
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
	$((sampler_x + 62)) $((sampler_y + 20))
DISPLAY=":$display_number" xdotool click --window "$window_id" 1
disabled_sample=$(DISPLAY=":$display_number" xclip -selection clipboard -o 2>/dev/null || true)
if [ "$disabled_sample" = '#123456FF' ]; then
	echo "UI smoke test: default-disabled pixel color sampler copied a readout" >&2
	exit 1
fi
stop_viewer

pixel_sampler_enabled_config="$temporary/pixel-sampler-enabled-config/jpegview-linux"
mkdir -p "$pixel_sampler_enabled_config"
printf 'pixel_color_sampler_enabled=1\n' > "$pixel_sampler_enabled_config/settings.conf"
XDG_STATE_HOME="$temporary/pixel-sampler-enabled-state"
VIEWER_TEST_CONFIG_HOME="$temporary/pixel-sampler-enabled-config"
export XDG_STATE_HOME VIEWER_TEST_CONFIG_HOME
launch_viewer "$pixel_sampler_directory"
assert_title_prefix "01-sampler.ppm" "enabled pixel-sampler fixture did not load"
sampler_geometry=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id")
sampler_window_width=$(printf '%s\n' "$sampler_geometry" | sed -n 's/^WIDTH=//p')
sampler_window_height=$(printf '%s\n' "$sampler_geometry" | sed -n 's/^HEIGHT=//p')
sampler_x=$((sampler_window_width / 2))
sampler_y=$((sampler_window_height / 2))
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" "$sampler_x" "$sampler_y"
sleep 0.15
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
	$((sampler_x + 62)) $((sampler_y + 20))
DISPLAY=":$display_number" xdotool click --window "$window_id" 1
sampled_color=''
for _ in $(seq 1 20); do
	sampled_color=$(DISPLAY=":$display_number" xclip -selection clipboard -o 2>/dev/null || true)
	if [ "$sampled_color" = '#123456FF' ]; then break; fi
	sleep 0.05
done
if [ "$sampled_color" != '#123456FF' ]; then
	echo "UI smoke test: pixel readout did not copy the decoded RGBA document color ($sampled_color)" >&2
	cat "$temporary/viewer.log" >&2
	exit 1
fi
clear_clipboard_text
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 8 8
stop_viewer
XDG_STATE_HOME=$pixel_sampler_previous_state
export XDG_STATE_HOME
if [ -n "$pixel_sampler_previous_config_set" ]; then
	VIEWER_TEST_CONFIG_HOME=$pixel_sampler_previous_config
	export VIEWER_TEST_CONFIG_HOME
else
	unset VIEWER_TEST_CONFIG_HOME
fi

# A fitted JPEG stays on its reduced display path until pointer motion asks the
# sampler for source pixels. This exercises the sampler-only decode channel,
# rather than the already-materialized PPM path above.
if command -v convert >/dev/null 2>&1; then
	lazy_sampler_jpeg="$temporary/pixel-sampler-lazy.jpg"
	lazy_sampler_config="$temporary/pixel-sampler-lazy-config/jpegview-linux"
	mkdir -p "$lazy_sampler_config"
	convert -size 1800x1200 xc:black -quality 90 "$lazy_sampler_jpeg"
	printf 'scale_mode=fit\ncache_size_mb=128\npixel_color_sampler_enabled=1\n' \
		> "$lazy_sampler_config/settings.conf"
	pixel_sampler_previous_state=$XDG_STATE_HOME
	pixel_sampler_previous_config_set=${VIEWER_TEST_CONFIG_HOME+x}
	pixel_sampler_previous_config=${VIEWER_TEST_CONFIG_HOME-}
	XDG_STATE_HOME="$temporary/pixel-sampler-lazy-state"
	VIEWER_TEST_CONFIG_HOME="$temporary/pixel-sampler-lazy-config"
	export XDG_STATE_HOME VIEWER_TEST_CONFIG_HOME
	launch_viewer "$lazy_sampler_jpeg"
	assert_title_prefix "pixel-sampler-lazy.jpg" "lazy-JPEG sampler fixture did not load"
	sampler_geometry=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id")
	sampler_window_width=$(printf '%s\n' "$sampler_geometry" | sed -n 's/^WIDTH=//p')
	sampler_window_height=$(printf '%s\n' "$sampler_geometry" | sed -n 's/^HEIGHT=//p')
	sampler_x=$((sampler_window_width / 2))
	sampler_y=$((sampler_window_height / 2))
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
		"$sampler_x" "$sampler_y"
	sampled_color=''
	for _ in $(seq 1 40); do
		sleep 0.05
		DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
			"$sampler_x" "$sampler_y"
		sleep 0.04
		DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
			$((sampler_x + 62)) $((sampler_y + 20))
		DISPLAY=":$display_number" xdotool click --window "$window_id" 1
		sampled_color=$(DISPLAY=":$display_number" xclip -selection clipboard -o 2>/dev/null || true)
		if [ "$sampled_color" = '#000000FF' ]; then break; fi
	done
	if [ "$sampled_color" != '#000000FF' ]; then
		echo "UI smoke test: lazy-JPEG hover did not decode and copy its source color ($sampled_color)" >&2
		cat "$temporary/viewer.log" >&2
		exit 1
	fi
	clear_clipboard_text
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 8 8
	stop_viewer
	XDG_STATE_HOME=$pixel_sampler_previous_state
	export XDG_STATE_HOME
	if [ -n "$pixel_sampler_previous_config_set" ]; then
		VIEWER_TEST_CONFIG_HOME=$pixel_sampler_previous_config
		export VIEWER_TEST_CONFIG_HOME
	else
		unset VIEWER_TEST_CONFIG_HOME
	fi
else
	echo "UI smoke test: SKIP lazy-JPEG sampler case (convert is unavailable)" >&2
fi

# Loaded-list sorting is asynchronous, but the selected source must remain the
# same after the new order is applied.
async_sort_directory="$temporary/async-sort-ui"
async_sort_config="$temporary/async-sort-ui-config/jpegview-linux"
mkdir -p "$async_sort_directory" "$async_sort_config"
write_ppm "$async_sort_directory/a-first.ppm" 220 40 30
write_ppm "$async_sort_directory/b-middle.ppm" 30 220 40
write_ppm "$async_sort_directory/c-selected.ppm" 40 30 220
touch -t 202001010000.00 "$async_sort_directory/c-selected.ppm"
touch -t 202101010000.00 "$async_sort_directory/b-middle.ppm"
touch -t 202201010000.00 "$async_sort_directory/a-first.ppm"
printf 'scale_mode=fit\nsort_mode=modification_date\nsort_ascending=1\ndouble_page_mode_enabled=0\nthumbnail_panel_visible=0\n' \
	> "$async_sort_config/settings.conf"
async_sort_previous_state=$XDG_STATE_HOME
XDG_STATE_HOME="$temporary/async-sort-ui-state"
VIEWER_TEST_CONFIG_HOME="$temporary/async-sort-ui-config"
export XDG_STATE_HOME VIEWER_TEST_CONFIG_HOME
launch_viewer "$async_sort_directory"
assert_title_prefix "c-selected.ppm" "metadata-sort fixture did not select the oldest image"
DISPLAY=":$display_number" xdotool key n
assert_title_prefix "c-selected.ppm" "asynchronous name sorting changed the selected source"
assert_title_prefix "[3/3] " "asynchronous name sorting did not apply the selected source's new index"
DISPLAY=":$display_number" xdotool key Left
assert_title_prefix "b-middle.ppm" "navigation did not follow the applied filename order"
assert_title_prefix "[2/3] " "navigation title did not follow the applied filename order"
stop_viewer
XDG_STATE_HOME=$async_sort_previous_state
unset VIEWER_TEST_CONFIG_HOME
export XDG_STATE_HOME

# A slideshow that reaches the end of a non-wrapping folder must stop its
# expired deadline instead of repeatedly invalidating and rebuilding frames.
boundary_directory="$temporary/slideshow-boundary"
boundary_config="$temporary/slideshow-boundary-config/jpegview-linux"
boundary_trace="$temporary/slideshow-boundary.csv"
boundary_log="$temporary/slideshow-boundary-viewer.log"
mkdir -p "$boundary_directory" "$boundary_config"
write_ppm "$boundary_directory/01-first.ppm" 220 40 30
write_ppm "$boundary_directory/02-last.ppm" 30 220 40
printf 'scale_mode=fit\nsort_mode=file_name\nfolder_wrap_around=0\ndouble_page_mode_enabled=0\nthumbnail_panel_visible=0\n' \
	> "$boundary_config/settings.conf"
DISPLAY=":$display_number" HOME="$temporary/slideshow-boundary-home" \
	XDG_CONFIG_HOME="$temporary/slideshow-boundary-config" \
	XDG_STATE_HOME="$temporary/slideshow-boundary-state" \
	JPEGVIEW_PERF_TRACE="$boundary_trace" \
	"$BINARY" --slideshow 0.1 "$boundary_directory" >"$boundary_log" 2>&1 &
viewer_pid=$!
window_id=''
for _ in $(seq 1 50); do
	window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
		--class jpegview-linux 2>/dev/null | head -1 || true)
	if [ -n "$window_id" ]; then break; fi
	sleep 0.1
done
if [ -z "$window_id" ]; then
	echo "UI smoke test: non-wrapping slideshow fixture did not create its window" >&2
	cat "$boundary_log" >&2
	exit 1
fi
DISPLAY=":$display_number" xdotool windowactivate "$window_id"
assert_title_prefix "[2/2] " "non-wrapping slideshow did not advance to the final image"
sleep 0.35
boundary_frames_before=$(awk -F, '$2 == "frame_build" { count++ } END { print count + 0 }' \
	"$boundary_trace" 2>/dev/null || true)
sleep 0.3
boundary_frames_after=$(awk -F, '$2 == "frame_build" { count++ } END { print count + 0 }' \
	"$boundary_trace" 2>/dev/null || true)
if ! kill -0 "$viewer_pid" 2>/dev/null || [ "$boundary_frames_before" -eq 0 ] || \
	[ "$boundary_frames_after" -ne "$boundary_frames_before" ]; then
	echo "UI smoke test: slideshow kept rebuilding frames after a non-wrapping folder boundary ($boundary_frames_before to $boundary_frames_after)" >&2
	cat "$boundary_log" >&2
	cat "$boundary_trace" >&2
	exit 1
fi
DISPLAY=":$display_number" xdotool key q || true
wait "$viewer_pid" || true
viewer_pid=''

# Wrapping a one-image folder is a successful navigation with no new content.
# It must restart the slideshow deadline without rebuilding that unchanged frame.
wrap_directory="$temporary/slideshow-wrap"
wrap_config="$temporary/slideshow-wrap-config/jpegview-linux"
wrap_trace="$temporary/slideshow-wrap.csv"
wrap_log="$temporary/slideshow-wrap-viewer.log"
mkdir -p "$wrap_directory" "$wrap_config"
write_ppm "$wrap_directory/only-image.ppm" 90 110 210
printf 'scale_mode=fit\nsort_mode=file_name\nfolder_wrap_around=1\ndouble_page_mode_enabled=0\nthumbnail_panel_visible=0\n' \
	> "$wrap_config/settings.conf"
DISPLAY=":$display_number" HOME="$temporary/slideshow-wrap-home" \
	XDG_CONFIG_HOME="$temporary/slideshow-wrap-config" \
	XDG_STATE_HOME="$temporary/slideshow-wrap-state" \
	JPEGVIEW_PERF_TRACE="$wrap_trace" \
	"$BINARY" --slideshow 0.1 "$wrap_directory" >"$wrap_log" 2>&1 &
viewer_pid=$!
window_id=''
for _ in $(seq 1 50); do
	window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
		--class jpegview-linux 2>/dev/null | head -1 || true)
	if [ -n "$window_id" ]; then break; fi
	sleep 0.1
done
if [ -z "$window_id" ]; then
	echo "UI smoke test: wrapping slideshow fixture did not create its window" >&2
	cat "$wrap_log" >&2
	exit 1
fi
DISPLAY=":$display_number" xdotool windowactivate "$window_id"
assert_title_prefix "[1/1] " "one-image wrapping slideshow did not present its image"
sleep 0.35
wrap_frames_before=$(awk -F, '$2 == "frame_build" { count++ } END { print count + 0 }' \
	"$wrap_trace" 2>/dev/null || true)
sleep 0.3
wrap_frames_after=$(awk -F, '$2 == "frame_build" { count++ } END { print count + 0 }' \
	"$wrap_trace" 2>/dev/null || true)
if ! kill -0 "$viewer_pid" 2>/dev/null || [ "$wrap_frames_before" -eq 0 ] || \
	[ "$wrap_frames_after" -ne "$wrap_frames_before" ]; then
	echo "UI smoke test: one-image wrapping slideshow rebuilt its unchanged frame ($wrap_frames_before to $wrap_frames_after)" >&2
	cat "$wrap_log" >&2
	cat "$wrap_trace" >&2
	exit 1
fi
DISPLAY=":$display_number" xdotool key q || true
wait "$viewer_pid" || true
viewer_pid=''

# A 4 MiB retained budget must still show both fitted pages after the selected
# anchor has been requested at actual size and only its larger texture remains.
spread_budget_directory="$temporary/spread-budget-fixtures"
mkdir -p "$spread_budget_directory" \
	"$temporary/spread-budget-config/jpegview-linux"
spread_budget_previous_state=$XDG_STATE_HOME
XDG_STATE_HOME="$temporary/spread-budget-state"
export XDG_STATE_HOME
write_solid_png() {
	python3 - "$1" "$2" "$3" "$4" <<'PY'
import struct
import sys
import zlib

path = sys.argv[1]
color = bytes(int(value) for value in sys.argv[2:5])
width, height = 768, 1024
row = b"\0" + color * width
def chunk(kind, payload):
	return (struct.pack(">I", len(payload)) + kind + payload +
		struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))

data = (b"\x89PNG\r\n\x1a\n" +
	chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
	chunk(b"IDAT", zlib.compress(row * height)) + chunk(b"IEND", b""))
with open(path, "wb") as output:
	output.write(data)
PY
}
write_solid_png "$spread_budget_directory/01-cover.png" 35 75 220
write_solid_png "$spread_budget_directory/02-anchor.png" 30 220 60
write_solid_png "$spread_budget_directory/03-partner.png" 220 40 30
printf 'scale_mode=fit\ncache_size_mb=4\ndouble_page_mode_enabled=0\nthumbnail_panel_visible=0\nmanga_reading_order_enabled=0\n' \
	> "$temporary/spread-budget-config/jpegview-linux/settings.conf"
spread_budget_trace="$temporary/spread-budget.csv"
spread_budget_log="$temporary/spread-budget-viewer.log"
perf_trace_active=0
DISPLAY=":$display_number" HOME="$temporary/spread-budget-home" \
	XDG_CONFIG_HOME="$temporary/spread-budget-config" \
	XDG_STATE_HOME="$XDG_STATE_HOME" \
	JPEGVIEW_PERF_TRACE="$spread_budget_trace" \
	"$BINARY" "$spread_budget_directory" >"$spread_budget_log" 2>&1 &
viewer_pid=$!
window_id=''
for _ in $(seq 1 50); do
	window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
		--class jpegview-linux 2>/dev/null | head -1 || true)
	if [ -n "$window_id" ]; then break; fi
	sleep 0.1
done
if [ -z "$window_id" ]; then
	echo "UI smoke test: 4 MiB spread fixture did not create its window" >&2
	cat "$spread_budget_log" >&2
	exit 1
fi
DISPLAY=":$display_number" xdotool windowactivate "$window_id"
assert_title_prefix "01-cover.png" "4 MiB spread fixture did not start on its standalone cover"
DISPLAY=":$display_number" xdotool key Right
assert_title_prefix "[2/3] " "4 MiB fixture did not select the single-page anchor"
count_spread_budget_uploads() {
	awk -F, '$2 == "texture_upload" && $4 == "event_thread" && \
		$6 == "active_image_spread" { count++ } END { print count + 0 }' \
		"$spread_budget_trace" 2>/dev/null || true
}
anchor_fit_ready=0
for _ in $(seq 1 100); do
	if [ "$(count_spread_budget_uploads)" -gt 0 ]; then
		anchor_fit_ready=1
		break
	fi
	sleep 0.05
done
if [ "$anchor_fit_ready" -ne 1 ]; then
	echo "UI smoke test: fitted anchor did not upload before actual-size navigation" >&2
	cat "$spread_budget_log" >&2
	cat "$spread_budget_trace" >&2
	exit 1
fi
baseline_uploads=$(count_spread_budget_uploads)
DISPLAY=":$display_number" xdotool key space
actual_anchor_ready=0
for _ in $(seq 1 100); do
	actual_uploads=$(count_spread_budget_uploads)
	if [ "$actual_uploads" -gt "$baseline_uploads" ]; then
		actual_anchor_ready=1
		break
	fi
	sleep 0.05
done
if [ "$actual_anchor_ready" -ne 1 ]; then
	echo "UI smoke test: actual-size anchor did not produce a prepared texture" >&2
	cat "$spread_budget_log" >&2
	cat "$spread_budget_trace" >&2
	exit 1
fi
DISPLAY=":$display_number" xdotool key space
DISPLAY=":$display_number" xdotool key d
assert_title_prefix "[2-3/3] " \
	"actual-size anchor prevented a fitted double-page pair that fits the retained budget"
if [ "$visual_assertions" -eq 1 ]; then
	spread_budget_rendered=0
	for _ in $(seq 1 50); do
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/spread-budget.png"
		spread_budget_left=$(convert "$temporary/spread-budget.png" \
			-format '%[hex:p{320,400}]' info:)
		spread_budget_right=$(convert "$temporary/spread-budget.png" \
			-format '%[hex:p{960,400}]' info:)
		case "$spread_budget_left:$spread_budget_right" in
			*1EDC3C*:*DC281E*) spread_budget_rendered=1; break ;;
		esac
		sleep 0.1
	done
	if [ "$spread_budget_rendered" -ne 1 ]; then
		echo "UI smoke test: 4 MiB active decoded pixels blocked the fitted spread ($spread_budget_left:$spread_budget_right)" >&2
		cat "$spread_budget_log" >&2
		exit 1
	fi
else
	echo "UI smoke test: SKIP (renderer pixel tools missing for the 4 MiB spread assertion)"
fi
stop_viewer
XDG_STATE_HOME=$spread_budget_previous_state
export XDG_STATE_HOME

if [ -n "$perf_trace_path" ]; then
	# Exercise per-row work classes with a generated 2,000-entry collection. All
	# prepared pixels must remain resident while renderer textures stay bounded
	# by the visible/overscan window. Revisiting an evicted texture must not read
	# or resample its source again.
	thumbnail_trace_directory="$temporary/thumbnail-trace-images"
	thumbnail_trace_config="$temporary/thumbnail-trace-config/jpegview-linux"
	thumbnail_trace="$perf_trace_path.thumbnails"
	mkdir -p "$thumbnail_trace_directory" "$thumbnail_trace_config"
	for index in $(seq 0 1999); do
		filename=$(printf '%04d' "$index")
		write_ppm "$thumbnail_trace_directory/$filename.ppm" \
			$((index % 256)) $(((index * 3) % 256)) $(((index * 7) % 256))
	done
	printf 'thumbnail_panel_visible=1\nthumbnail_panel_width=164\ncache_size_mb=0\ndouble_page_mode_enabled=0\n' \
		> "$thumbnail_trace_config/settings.conf"
	DISPLAY=":$display_number" HOME="$temporary/thumbnail-trace-home" \
		XDG_CONFIG_HOME="$temporary/thumbnail-trace-config" \
		XDG_STATE_HOME="$temporary/thumbnail-trace-state" \
		JPEGVIEW_PERF_TRACE="$thumbnail_trace" \
		"$BINARY" "$thumbnail_trace_directory" \
		>"$temporary/thumbnail-trace-viewer.log" 2>&1 &
	viewer_pid=$!
	window_id=''
	for _ in $(seq 1 50); do
		window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null | head -1 || true)
		if [ -n "$window_id" ]; then break; fi
		sleep 0.1
	done
	if [ -z "$window_id" ]; then
		echo "UI smoke test: thumbnail attribution fixture did not open" >&2
		cat "$temporary/thumbnail-trace-viewer.log" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	thumbnail_trace_seen=0
	for _ in $(seq 1 600); do
		if awk -F, '
			$2 == "source_read" && $4 == "worker_thread" && $6 == "visible_thumbnail" { visible_read = 1 }
			$2 == "source_read" && $4 == "worker_thread" && $6 == "distant_speculation" { distant_read = 1 }
			$2 == "resampling" && $4 == "worker_thread" && $6 == "visible_thumbnail" { visible_resample = 1 }
			$2 == "resampling" && $4 == "worker_thread" && $6 == "distant_speculation" { distant_resample = 1 }
			$2 == "texture_upload" && $4 == "event_thread" && $6 == "visible_thumbnail" { visible_upload = 1 }
			$2 == "cache_snapshot" && $13 == "\"thumbnail_pixels\"" && $8 == 2000 { all_pixels_retained = 1 }
			$2 == "cache_snapshot" && $13 == "\"thumbnail_textures\"" {
				texture_snapshot = 1
				if ($8 > 40) texture_window_exceeded = 1
			}
			END { exit !(visible_read && distant_read && visible_resample && distant_resample && visible_upload && all_pixels_retained && texture_snapshot && !texture_window_exceeded) }
		' "$thumbnail_trace" 2>/dev/null; then
			thumbnail_trace_seen=1
			break
		fi
		sleep 0.05
	done
	if [ "$thumbnail_trace_seen" -ne 1 ]; then
		echo "UI smoke test: 2,000-entry thumbnail trace did not retain all pixels within a bounded texture window" >&2
		cat "$temporary/thumbnail-trace-viewer.log" >&2
		if [ -f "$thumbnail_trace" ]; then cat "$thumbnail_trace" >&2; fi
		exit 1
	fi
	thumbnail_reads_before_revisit=$(awk -F, '
		$2 == "source_read" && $4 == "worker_thread" && ($6 == "visible_thumbnail" || $6 == "distant_speculation") { count++ }
		END { print count + 0 }
	' "$thumbnail_trace")
	thumbnail_resamples_before_revisit=$(awk -F, '
		$2 == "resampling" && $4 == "worker_thread" && ($6 == "visible_thumbnail" || $6 == "distant_speculation") { count++ }
		END { print count + 0 }
	' "$thumbnail_trace")
	thumbnail_uploads_before_revisit=$(awk -F, '
		$2 == "texture_upload" && $4 == "event_thread" && ($6 == "visible_thumbnail" || $6 == "distant_speculation") { count++ }
		END { print count + 0 }
	' "$thumbnail_trace")
	DISPLAY=":$display_number" xdotool key End
	sleep 0.5
	DISPLAY=":$display_number" xdotool key Home
	sleep 1
	thumbnail_reads_after_revisit=$(awk -F, '
		$2 == "source_read" && $4 == "worker_thread" && ($6 == "visible_thumbnail" || $6 == "distant_speculation") { count++ }
		END { print count + 0 }
	' "$thumbnail_trace")
	thumbnail_resamples_after_revisit=$(awk -F, '
		$2 == "resampling" && $4 == "worker_thread" && ($6 == "visible_thumbnail" || $6 == "distant_speculation") { count++ }
		END { print count + 0 }
	' "$thumbnail_trace")
	thumbnail_uploads_after_revisit=$(awk -F, '
		$2 == "texture_upload" && $4 == "event_thread" && ($6 == "visible_thumbnail" || $6 == "distant_speculation") { count++ }
		END { print count + 0 }
	' "$thumbnail_trace")
	if [ "$thumbnail_reads_after_revisit" -ne "$thumbnail_reads_before_revisit" ] || \
		[ "$thumbnail_resamples_after_revisit" -ne "$thumbnail_resamples_before_revisit" ] || \
		[ "$thumbnail_uploads_after_revisit" -le "$thumbnail_uploads_before_revisit" ]; then
		echo "UI smoke test: thumbnail revisit decoded again or failed to upload retained pixels" >&2
		cat "$temporary/thumbnail-trace-viewer.log" >&2
		cat "$thumbnail_trace" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool key q || true
	wait "$viewer_pid" || true
	viewer_pid=''

	# With retained caching disabled, load and rotate an oversized source. Decode,
	# processing, and resampling must stay on workers; only texture upload belongs
	# to the renderer thread. The rotation queued during loading must still apply.
	mkdir -p "$temporary/perf-sync-config/jpegview-linux"
	printf 'scale_mode=fit\ncache_size_mb=0\ndouble_page_mode_enabled=0\n' \
		> "$temporary/perf-sync-config/jpegview-linux/settings.conf"
	perf_sync_image="$temporary/perf-sync-source.ppm"
	perf_sync_trace="$perf_trace_path.sync"
	{
		printf 'P6\n1600 1200\n255\n'
		head -c "$((1600 * 1200 * 3))" /dev/zero
	} > "$perf_sync_image"
	DISPLAY=":$display_number" HOME="$temporary/perf-sync-home" \
		XDG_CONFIG_HOME="$temporary/perf-sync-config" \
		XDG_STATE_HOME="$temporary/perf-sync-state" \
		JPEGVIEW_PERF_TRACE="$perf_sync_trace" \
		"$BINARY" "$perf_sync_image" >"$temporary/perf-sync-viewer.log" 2>&1 &
	viewer_pid=$!
	window_id=''
	for _ in $(seq 1 50); do
		window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null | head -1 || true)
		if [ -n "$window_id" ]; then break; fi
		sleep 0.1
	done
	if [ -z "$window_id" ]; then
		echo "UI smoke test: foreground performance fixture did not open" >&2
		cat "$temporary/perf-sync-viewer.log" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	DISPLAY=":$display_number" xdotool key Down
	async_foreground_seen=0
	for _ in $(seq 1 100); do
		if awk -F, '
			$2 == "source_read" && $4 == "worker_thread" &&
				$6 == "active_image_spread" { worker_source_read = 1 }
			$2 == "decode" && $4 == "worker_thread" &&
				$6 == "active_image_spread" { worker_decode = 1 }
			$2 == "processing" && $4 == "worker_thread" &&
				$6 == "active_image_spread" {
				worker_processing = 1
				worker_pixel_operation_time = $1
			}
			$2 == "resampling" && $4 == "worker_thread" &&
				$6 == "active_image_spread" { worker_resampling_time = $1 }
			$2 == "texture_upload" && $4 == "event_thread" &&
				$6 == "active_image_spread" {
				if (worker_resampling_time != "" && $1 >= worker_resampling_time) {
					prepared_upload = 1
				}
				if (worker_pixel_operation_time != "" && $1 >= worker_pixel_operation_time &&
					$8 == 1200 && $9 == 1600) {
					transform_upload = 1
				}
			}
			$2 == "source_read" && $4 == "event_thread" { event_source_read = 1 }
			$2 == "decode" && $4 == "event_thread" { event_decode = 1 }
			$2 == "resampling" && $4 == "event_thread" { event_resampling = 1 }
			END {
				exit !(worker_source_read && worker_decode && worker_processing &&
					worker_resampling_time != "" && prepared_upload &&
					worker_pixel_operation_time != "" && transform_upload &&
					!event_source_read && !event_decode && !event_resampling)
			}
		' \
			"$perf_sync_trace" 2>/dev/null; then
			async_foreground_seen=1
			break
		fi
		sleep 0.05
	done
	stop_viewer
	if [ "$async_foreground_seen" -ne 1 ]; then
		echo "UI smoke test: selected foreground work did not stay on workers or its queued transform did not upload" >&2
		cat "$temporary/perf-sync-viewer.log" >&2
		if [ -f "$perf_sync_trace" ]; then cat "$perf_sync_trace" >&2; fi
		exit 1
	fi

	# A source-resolution frame remains sufficient through further enlargement
	# and pan. Neither interaction should start another pixel-processing pass or
	# renderer upload for the selected image.
	zoom_pan_directory="$temporary/zoom-pan-images"
	zoom_pan_config="$temporary/zoom-pan-config/jpegview-linux"
	zoom_pan_trace="$perf_trace_path.zoom-pan"
	mkdir -p "$zoom_pan_directory" "$zoom_pan_config"
	printf 'scale_mode=fit\ncache_size_mb=128\ndouble_page_mode_enabled=0\nshow_histogram=0\n' \
		> "$zoom_pan_config/settings.conf"
	zoom_pan_image="$zoom_pan_directory/zoom-pan.ppm"
	{
		printf 'P6\n2000 1200\n255\n'
		head -c "$((2000 * 1200 * 3))" /dev/zero
	} > "$zoom_pan_image"
	DISPLAY=":$display_number" HOME="$temporary/zoom-pan-home" \
		XDG_CONFIG_HOME="$temporary/zoom-pan-config" \
		XDG_STATE_HOME="$temporary/zoom-pan-state" \
		JPEGVIEW_PERF_TRACE="$zoom_pan_trace" \
		"$BINARY" "$zoom_pan_image" >"$temporary/zoom-pan-viewer.log" 2>&1 &
	viewer_pid=$!
	window_id=''
	for _ in $(seq 1 50); do
		window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null | head -1 || true)
		if [ -n "$window_id" ]; then break; fi
		sleep 0.1
	done
	if [ -z "$window_id" ]; then
		echo "UI smoke test: source-resolution zoom fixture did not open" >&2
		cat "$temporary/zoom-pan-viewer.log" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	initial_uploads=0
	initial_ready=0
	for _ in $(seq 1 100); do
		initial_uploads=$(awk -F, '$2 == "texture_upload" && $4 == "event_thread" && \
			$6 == "active_image_spread" { count++ } END { print count + 0 }' "$zoom_pan_trace" 2>/dev/null || true)
		if [ "$initial_uploads" -gt 0 ]; then initial_ready=1; break; fi
		sleep 0.05
	done
	if [ "$initial_ready" -ne 1 ]; then
		echo "UI smoke test: fitted source-resolution fixture did not upload" >&2
		cat "$temporary/zoom-pan-viewer.log" >&2
		cat "$zoom_pan_trace" >&2
		exit 1
	fi
	baseline_uploads=$initial_uploads
	baseline_processing=$(awk -F, '$2 == "processing" && $4 == "worker_thread" && \
		$6 == "active_image_spread" { count++ } END { print count + 0 }' "$zoom_pan_trace")
	DISPLAY=":$display_number" xdotool key space
	actual_ready=0
	for _ in $(seq 1 100); do
		actual_uploads=$(awk -F, '$2 == "texture_upload" && $4 == "event_thread" && \
			$6 == "active_image_spread" { count++ } END { print count + 0 }' "$zoom_pan_trace")
		actual_processing=$(awk -F, '$2 == "processing" && $4 == "worker_thread" && \
			$6 == "active_image_spread" { count++ } END { print count + 0 }' "$zoom_pan_trace")
		if [ "$actual_uploads" -ge "$((baseline_uploads + 3))" ] && \
			[ "$actual_processing" -gt "$baseline_processing" ]; then
			actual_ready=1
			break
		fi
		sleep 0.05
	done
	if [ "$actual_ready" -ne 1 ]; then
		echo "UI smoke test: actual-size banded source-resolution frame did not finish its three uploads" >&2
		cat "$temporary/zoom-pan-viewer.log" >&2
		cat "$zoom_pan_trace" >&2
		exit 1
	fi
	baseline_uploads=$actual_uploads
	baseline_processing=$actual_processing
	DISPLAY=":$display_number" xdotool key plus
	DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 512 mousedown 1
	DISPLAY=":$display_number" xdotool mousemove --sync --window "$window_id" 690 562 mouseup 1
	DISPLAY=":$display_number" xdotool key --window "$window_id" Return
	sleep 0.4
	final_uploads=$(awk -F, '$2 == "texture_upload" && $4 == "event_thread" && \
		$6 == "active_image_spread" { count++ } END { print count + 0 }' "$zoom_pan_trace")
	final_processing=$(awk -F, '$2 == "processing" && $4 == "worker_thread" && \
		$6 == "active_image_spread" { count++ } END { print count + 0 }' "$zoom_pan_trace")
	DISPLAY=":$display_number" xdotool key q || true
	wait "$viewer_pid" || true
	viewer_pid=''
	texture_destroy_count=$(awk -F, '$2 == "texture_destroy" && $4 == "event_thread" { count++ } \
		END { print count + 0 }' "$zoom_pan_trace")
	texture_residency_violation=$(awk -F, '$2 == "cache_snapshot" && \
		$13 == "\"display_texture_residency\"" && $7 > $8 { failed = 1 } \
		END { print failed + 0 }' "$zoom_pan_trace")
	texture_residency_samples=$(awk -F, '$2 == "cache_snapshot" && \
		$13 == "\"display_texture_residency\"" { count++ } END { print count + 0 }' "$zoom_pan_trace")
	if [ "$final_uploads" -ne "$baseline_uploads" ] || \
		[ "$final_processing" -ne "$baseline_processing" ] || \
		[ "$texture_destroy_count" -eq 0 ] || \
		[ "$texture_residency_violation" -ne 0 ] || \
		[ "$texture_residency_samples" -eq 0 ]; then
		echo "UI smoke test: source-resolution enlargement or pan repeated display preparation/upload ($baseline_processing/$final_processing processing; $baseline_uploads/$final_uploads uploads)" >&2
		cat "$temporary/zoom-pan-viewer.log" >&2
		cat "$zoom_pan_trace" >&2
		exit 1
	fi

	# Continuous held navigation keeps the interaction policy active. Once four
	# obsolete renderer textures queue, maintenance must destroy one per tick so
	# active-working textures cannot accumulate for the duration of the hold.
	held_navigation_directory="$temporary/held-navigation-images"
	held_navigation_config="$temporary/held-navigation-config/jpegview-linux"
	held_navigation_trace="$perf_trace_path.held-navigation"
	mkdir -p "$held_navigation_directory" "$held_navigation_config"
	for index in $(seq 0 19); do
		filename=$(printf '%02d' "$index")
		write_solid_ppm "$held_navigation_directory/$filename.ppm" \
			$((index * 11 % 256)) $((index * 17 % 256)) $((index * 23 % 256))
	done
	printf 'scale_mode=fit\ncache_size_mb=0\ndouble_page_mode_enabled=0\nshow_histogram=0\nthumbnail_panel_visible=0\n' \
		> "$held_navigation_config/settings.conf"
	DISPLAY=":$display_number" HOME="$temporary/held-navigation-home" \
		XDG_CONFIG_HOME="$temporary/held-navigation-config" \
		XDG_STATE_HOME="$temporary/held-navigation-state" \
		JPEGVIEW_PERF_TRACE="$held_navigation_trace" \
		"$BINARY" "$held_navigation_directory" \
		> "$temporary/held-navigation-viewer.log" 2>&1 &
	viewer_pid=$!
	window_id=''
	for _ in $(seq 1 50); do
		window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null | head -1 || true)
		if [ -n "$window_id" ]; then break; fi
		sleep 0.1
	done
	if [ -z "$window_id" ]; then
		echo "UI smoke test: held-navigation retirement fixture did not create its window" >&2
		cat "$temporary/held-navigation-viewer.log" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	initial_navigation_uploads=0
	for _ in $(seq 1 100); do
		initial_navigation_uploads=$(awk -F, '$2 == "texture_upload" && \
			$4 == "event_thread" && $6 == "active_image_spread" { count++ } \
			END { print count + 0 }' "$held_navigation_trace" 2>/dev/null || true)
		if [ "$initial_navigation_uploads" -gt 0 ]; then break; fi
		sleep 0.05
	done
	if [ "$initial_navigation_uploads" -eq 0 ]; then
		echo "UI smoke test: held-navigation retirement fixture did not upload its first image" >&2
		cat "$temporary/held-navigation-viewer.log" >&2
		cat "$held_navigation_trace" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool keydown --window "$window_id" Right
	held_navigation_destroy_count=0
	for _ in $(seq 1 120); do
		held_navigation_destroy_count=$(awk -F, \
			'$2 == "texture_destroy" && $4 == "event_thread" { count++ } \
			END { print count + 0 }' "$held_navigation_trace" 2>/dev/null || true)
		if [ "$held_navigation_destroy_count" -ge 3 ]; then break; fi
		sleep 0.05
	done
	DISPLAY=":$display_number" xdotool keyup --window "$window_id" Right
	DISPLAY=":$display_number" xdotool key q || true
	wait "$viewer_pid" || true
	viewer_pid=''
	held_navigation_max_retirement_queue=$(awk -F, \
		'$2 == "cache_snapshot" && $13 == "\"display_texture_residency\"" { \
			if ($10 > maximum) maximum = $10; samples++ \
		} END { print maximum + 0 ":" samples + 0 }' "$held_navigation_trace")
	held_navigation_max_queue=${held_navigation_max_retirement_queue%%:*}
	held_navigation_queue_samples=${held_navigation_max_retirement_queue#*:}
	if [ "$held_navigation_destroy_count" -lt 3 ] || \
		[ "$held_navigation_queue_samples" -eq 0 ] || \
		[ "$held_navigation_max_queue" -gt 4 ]; then
		echo "UI smoke test: held navigation stalled texture retirement or exceeded its queue bound ($held_navigation_destroy_count destroys, maximum queue $held_navigation_max_queue)" >&2
		cat "$temporary/held-navigation-viewer.log" >&2
		cat "$held_navigation_trace" >&2
		exit 1
	fi
fi

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

# Reopening an already decoded non-JPEG can commit synchronously from the
# renderer cache. The selected-load continuation must not overwrite that
# committed title with a stale loading status.
cached_title_directory="$temporary/cached-title"
cached_title_config="$temporary/cached-title-config"
mkdir -p "$cached_title_directory" "$cached_title_config/jpegview-linux"
write_solid_ppm "$cached_title_directory/01-first.ppm" 220 40 40
write_solid_ppm "$cached_title_directory/02-second.ppm" 40 180 80
printf 'scale_mode=fit\ncache_size_mb=8\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
	> "$cached_title_config/jpegview-linux/settings.conf"
VIEWER_TEST_HOME="$temporary/cached-title-home" \
	VIEWER_TEST_CONFIG_HOME="$cached_title_config" \
	launch_viewer "$cached_title_directory/01-first.ppm"
cached_title_first_ready=0
for _ in $(seq 1 100); do
	cached_title=$(DISPLAY=":$display_number" window_title_without_position)
	case "$cached_title" in
		'01-first.ppm ('*) cached_title_first_ready=1; break ;;
	esac
	sleep 0.025
done
if [ "$cached_title_first_ready" -ne 1 ]; then
	echo "UI smoke test: first cached-title image did not finish loading ($cached_title)" >&2
	exit 1
fi
DISPLAY=":$display_number" xdotool key Right
assert_title_prefix "02-second.ppm" "cached-title fixture did not navigate to its second image"
DISPLAY=":$display_number" xdotool key Left
cached_title_reopened_ready=0
for _ in $(seq 1 100); do
	cached_title=$(DISPLAY=":$display_number" window_title_without_position)
	case "$cached_title" in
		'01-first.ppm ('*) cached_title_reopened_ready=1; break ;;
	esac
	sleep 0.025
done
if [ "$cached_title_reopened_ready" -ne 1 ]; then
	echo "UI smoke test: cached non-JPEG reopen remained in a loading title ($cached_title)" >&2
	exit 1
fi
stop_viewer

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
	cc -shared -fPIC "$SCRIPT_DIR/delay_mmap.c" -o "$temporary/slow_map.so" -ldl -pthread
	# Reloading an unchanged source must reset a locally rotated document while
	# keeping the asynchronous renderer commit live.
	same_reload_directory="$temporary/same-source-reload"
	same_reload_config="$temporary/same-source-reload-config"
	mkdir -p "$same_reload_directory" "$same_reload_config/jpegview-linux"
	convert -size 600x300 xc:red -fill blue -draw 'rectangle 300,0 599,299' \
		"$same_reload_directory/01-reload.png"
	printf 'scale_mode=fit\ncache_size_mb=0\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
		> "$same_reload_config/jpegview-linux/settings.conf"
	VIEWER_TEST_HOME="$temporary/same-source-reload-home" \
		VIEWER_TEST_CONFIG_HOME="$same_reload_config" \
		launch_viewer "$same_reload_directory/01-reload.png"
	assert_title_prefix "01-reload.png" "same-source reload fixture did not load"
	if [ "$visual_assertions" -eq 1 ]; then
		viewer_geometry=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id")
		viewer_width=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^WIDTH=//p')
		viewer_height=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^HEIGHT=//p')
		DISPLAY=":$display_number" xdotool key Down
		reload_rotated=0
		for _ in $(seq 1 60); do
			DISPLAY=":$display_number" import -window "$window_id" "$temporary/reload-rotated.png"
			reload_top=$(convert "$temporary/reload-rotated.png" -format \
				"%[fx:p{$((viewer_width / 2)),$((viewer_height / 2 - 60))}.r>0.75&&p{$((viewer_width / 2)),$((viewer_height / 2 - 60))}.b<0.25]" info:)
			reload_bottom=$(convert "$temporary/reload-rotated.png" -format \
				"%[fx:p{$((viewer_width / 2)),$((viewer_height / 2 + 60))}.b>0.75&&p{$((viewer_width / 2)),$((viewer_height / 2 + 60))}.r<0.25]" info:)
			if [ "$reload_top:$reload_bottom" = "1:1" ]; then reload_rotated=1; break; fi
			sleep 0.05
		done
		if [ "$reload_rotated" -ne 1 ]; then
			echo "UI smoke test: same-source reload fixture did not rotate before reload ($reload_top:$reload_bottom)" >&2
			exit 1
		fi
		DISPLAY=":$display_number" xdotool key ctrl+r
		reload_restored=0
		for _ in $(seq 1 80); do
			DISPLAY=":$display_number" import -window "$window_id" "$temporary/reload-restored.png"
			reload_left=$(convert "$temporary/reload-restored.png" -format \
				"%[fx:p{$((viewer_width / 2 - 60)),$((viewer_height / 2))}.r>0.75&&p{$((viewer_width / 2 - 60)),$((viewer_height / 2))}.b<0.25]" info:)
			reload_right=$(convert "$temporary/reload-restored.png" -format \
				"%[fx:p{$((viewer_width / 2 + 60)),$((viewer_height / 2))}.b>0.75&&p{$((viewer_width / 2 + 60)),$((viewer_height / 2))}.r<0.25]" info:)
			if [ "$reload_left:$reload_right" = "1:1" ]; then reload_restored=1; break; fi
			sleep 0.05
		done
		if [ "$reload_restored" -ne 1 ]; then
			echo "UI smoke test: same-source reload did not restore the unmodified source frame ($reload_left:$reload_right)" >&2
			cat "$temporary/viewer.log" >&2
			exit 1
		fi
	fi
	stop_viewer

	# Resize must publish a replacement document and renderer texture from the
	# asynchronous pixel worker without changing the original source file.
	resize_directory="$temporary/resize-worker"
	resize_config="$temporary/resize-worker-config"
	mkdir -p "$resize_directory" "$resize_config/jpegview-linux"
	convert -size 600x300 xc:red -fill blue -draw 'rectangle 300,0 599,299' \
		"$resize_directory/01-resize.png"
	printf 'scale_mode=fit\ncache_size_mb=0\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
		> "$resize_config/jpegview-linux/settings.conf"
	VIEWER_TEST_HOME="$temporary/resize-worker-home" \
		VIEWER_TEST_CONFIG_HOME="$resize_config" \
		launch_viewer "$resize_directory/01-resize.png"
	assert_title_prefix "01-resize.png (600x300," "resize worker fixture did not load"
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	DISPLAY=":$display_number" xdotool key --window "$window_id" ctrl+shift+r
	DISPLAY=":$display_number" xdotool key --window "$window_id" ctrl+a
	DISPLAY=":$display_number" xdotool type --window "$window_id" --delay 30 '50'
	DISPLAY=":$display_number" xdotool key --window "$window_id" Return
	assert_title_prefix "01-resize.png (300x150," \
		"asynchronous resize did not commit the 50-percent document and texture"
	if [ "$(identify -format '%wx%h' "$resize_directory/01-resize.png")" != "600x300" ]; then
		echo "UI smoke test: in-memory resize unexpectedly changed the source file" >&2
		exit 1
	fi
	if [ "$visual_assertions" -eq 1 ]; then
		viewer_geometry=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id")
		viewer_width=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^WIDTH=//p')
		viewer_height=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^HEIGHT=//p')
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/resized-image.png"
		resize_center_x=$((viewer_width / 2))
		resize_center_y=$((viewer_height / 2))
		resize_left=$(convert "$temporary/resized-image.png" -format \
			"%[fx:p{$((resize_center_x - 50)),$resize_center_y}.r>0.75&&p{$((resize_center_x - 50)),$resize_center_y}.b<0.25]" info:)
		resize_right=$(convert "$temporary/resized-image.png" -format \
			"%[fx:p{$((resize_center_x + 50)),$resize_center_y}.b>0.75&&p{$((resize_center_x + 50)),$resize_center_y}.r<0.25]" info:)
		if [ "$resize_left:$resize_right" != "1:1" ]; then
			echo "UI smoke test: resized renderer texture did not preserve the two image halves ($resize_left:$resize_right)" >&2
			exit 1
		fi
	fi
	stop_viewer

	# Saving over the selected source must materialize its lazy document pixels,
	# preserve the current presentation, and rebind later operations to the new
	# filesystem identity. The title reports source dimensions, so verify the
	# subsequent rotation through the rendered pixels below.
	in_place_directory="$temporary/in-place-save"
	in_place_config="$temporary/in-place-save-config"
	mkdir -p "$in_place_directory" "$in_place_config/jpegview-linux"
	convert -size 600x300 xc:red -fill blue -draw 'rectangle 300,0 599,299' \
		"$in_place_directory/01-inplace.png"
	ln -s 01-inplace.png "$in_place_directory/alias.png"
	printf 'scale_mode=fit\ncache_size_mb=0\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
		> "$in_place_config/jpegview-linux/settings.conf"
	VIEWER_TEST_HOME="$temporary/in-place-save-home" \
		VIEWER_TEST_CONFIG_HOME="$in_place_config" \
		launch_viewer "$in_place_directory/01-inplace.png"
	assert_title_prefix "01-inplace.png (600x300," \
		"in-place save fixture did not load its lazy source"
	in_place_original_inode=$(stat -c '%i' "$in_place_directory/01-inplace.png")
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	DISPLAY=":$display_number" xdotool key --window "$window_id" ctrl+s
	# Save through a symlink alias to the selected source. The default name is
	# `01-inplace_proc.jpg` (19 code points); clear it exactly so the last key
	# cannot navigate to the parent.
	send_repeated_keypresses 19 BackSpace "$window_id"
	DISPLAY=":$display_number" xdotool type --window "$window_id" --delay 25 'alias.png'
	DISPLAY=":$display_number" xdotool key --window "$window_id" Return
	sleep 0.1
	DISPLAY=":$display_number" xdotool key --window "$window_id" Return
	in_place_saved_inode=$in_place_original_inode
	for _ in $(seq 1 80); do
		in_place_saved_inode=$(stat -c '%i' "$in_place_directory/01-inplace.png")
		if [ "$in_place_saved_inode" != "$in_place_original_inode" ]; then break; fi
		sleep 0.05
	done
	if [ "$in_place_saved_inode" = "$in_place_original_inode" ]; then
		echo "UI smoke test: in-place save did not publish a new image inode" >&2
		cat "$temporary/viewer.log" >&2
		exit 1
	fi
	if [ "$(identify -format '%wx%h' "$in_place_directory/01-inplace.png")" != "600x300" ]; then
		echo "UI smoke test: in-place save changed the selected source dimensions" >&2
		exit 1
	fi
	if [ "$visual_assertions" -eq 1 ]; then
		viewer_geometry=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id")
		viewer_width=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^WIDTH=//p')
		viewer_height=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^HEIGHT=//p')
		in_place_x=$((viewer_width / 2))
		in_place_y=$((viewer_height / 2))
		in_place_visible=0
		for _ in $(seq 1 60); do
			DISPLAY=":$display_number" import -window "$window_id" "$temporary/in-place-after-save.png"
			in_place_left=$(convert "$temporary/in-place-after-save.png" -format \
				"%[fx:p{$((in_place_x - 50)),$in_place_y}.r>0.75&&p{$((in_place_x - 50)),$in_place_y}.b<0.25]" info:)
			in_place_right=$(convert "$temporary/in-place-after-save.png" -format \
				"%[fx:p{$((in_place_x + 50)),$in_place_y}.b>0.75&&p{$((in_place_x + 50)),$in_place_y}.r<0.25]" info:)
			if [ "$in_place_left:$in_place_right" = "1:1" ]; then
				in_place_visible=1
				break
			fi
			sleep 0.05
		done
		if [ "$in_place_visible" -ne 1 ]; then
			echo "UI smoke test: in-place save blanked or changed the current presentation ($in_place_left:$in_place_right)" >&2
			exit 1
		fi
	fi
	DISPLAY=":$display_number" xdotool key --window "$window_id" Down
	if [ "$visual_assertions" -eq 1 ]; then
		in_place_rotated=0
		for _ in $(seq 1 60); do
			DISPLAY=":$display_number" import -window "$window_id" "$temporary/in-place-rotated.png"
			in_place_top=$(convert "$temporary/in-place-rotated.png" -format \
				"%[fx:p{$in_place_x,$((in_place_y - 60))}.r>0.75&&p{$in_place_x,$((in_place_y - 60))}.b<0.25]" info:)
			in_place_bottom=$(convert "$temporary/in-place-rotated.png" -format \
				"%[fx:p{$in_place_x,$((in_place_y + 60))}.b>0.75&&p{$in_place_x,$((in_place_y + 60))}.r<0.25]" info:)
			if [ "$in_place_top:$in_place_bottom" = "1:1" ]; then
				in_place_rotated=1
				break
			fi
			sleep 0.05
		done
		if [ "$in_place_rotated" -ne 1 ]; then
			echo "UI smoke test: transform after in-place save did not update the visible pixels ($in_place_top:$in_place_bottom)" >&2
			exit 1
		fi
	fi
	stop_viewer

	# A same-source resolution replacement must retain the last presented texture
	# for every renderer tick while the replacement decode is blocked.
	fallback_directory="$temporary/same-source-texture-fallback"
	fallback_config="$temporary/same-source-texture-fallback-config"
	mkdir -p "$fallback_directory" "$fallback_config/jpegview-linux"
	convert -size 2400x1200 xc:red -fill blue -draw 'rectangle 1200,0 2399,1199' \
		-quality 95 "$fallback_directory/01-fallback.jpg"
	printf 'scale_mode=fit\ncache_size_mb=32\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
		> "$fallback_config/jpegview-linux/settings.conf"
	fallback_started="$temporary/fallback.started"
	fallback_release="$temporary/fallback.release"
	fallback_active="$temporary/fallback.active"
	DISPLAY=":$display_number" HOME="$temporary/fallback-home" \
		XDG_CONFIG_HOME="$fallback_config" XDG_STATE_HOME="$temporary/fallback-state" \
		LD_PRELOAD="$temporary/slow_map.so" \
		JPEGVIEW_TEST_SLOW_MAP="$fallback_directory/01-fallback.jpg" \
		JPEGVIEW_TEST_SLOW_MAP_REPEAT=1 \
		JPEGVIEW_TEST_SLOW_MAP_STARTED="$fallback_started" \
		JPEGVIEW_TEST_SLOW_MAP_ACTIVE="$fallback_active" \
		JPEGVIEW_TEST_SLOW_MAP_RELEASE="$fallback_release" \
		JPEGVIEW_TEST_SLOW_MAP_MAX_ATTEMPTS=1200 \
		"$BINARY" "$fallback_directory/01-fallback.jpg" \
		>"$temporary/fallback-viewer.log" 2>&1 &
	viewer_pid=$!
	window_id=''
	for _ in $(seq 1 50); do
		window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null | head -1 || true)
		if [ -n "$window_id" ]; then break; fi
		sleep 0.05
	done
	if [ -z "$window_id" ]; then
		echo "UI smoke test: fallback viewer window was unavailable" >&2
		exit 1
	fi
	fallback_initial_blocked=0
	for _ in $(seq 1 100); do
		if [ -f "$fallback_started" ] && [ -f "$fallback_active" ]; then
			fallback_initial_blocked=1
			break
		fi
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.05
	done
	if [ "$fallback_initial_blocked" -ne 1 ]; then
		echo "UI smoke test: initial JPEG presentation did not reach the controlled map barrier" >&2
		cat "$temporary/fallback-viewer.log" >&2
		exit 1
	fi
	: > "$fallback_release"
	fallback_initial_rendered=0
	for _ in $(seq 1 100); do
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/fallback-initial.png"
		fallback_left=$(convert "$temporary/fallback-initial.png" -format \
			"%[fx:p{320,400}.r>0.75&&p{320,400}.b<0.25]" info:)
		fallback_right=$(convert "$temporary/fallback-initial.png" -format \
			"%[fx:p{960,400}.b>0.75&&p{960,400}.r<0.25]" info:)
		if [ "$fallback_left:$fallback_right" = "1:1" ] && [ ! -f "$fallback_active" ]; then
			fallback_initial_rendered=1
			break
		fi
		sleep 0.05
	done
	if [ "$fallback_initial_rendered" -ne 1 ]; then
		echo "UI smoke test: controlled JPEG did not produce its initial cached texture ($fallback_left:$fallback_right)" >&2
		cat "$temporary/fallback-viewer.log" >&2
		exit 1
	fi
	# Drain any startup metadata access while the release file still exists.
	sleep 0.2
	rm -f -- "$fallback_started" "$fallback_release"
	fallback_geometry=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id")
	fallback_width=$(printf '%s\n' "$fallback_geometry" | sed -n 's/^WIDTH=//p')
	fallback_height=$(printf '%s\n' "$fallback_geometry" | sed -n 's/^HEIGHT=//p')
	DISPLAY=":$display_number" xdotool windowsize "$window_id" \
		$((fallback_width + 80)) $((fallback_height + 40))
	fallback_replacement_blocked=0
	for _ in $(seq 1 100); do
		if [ -f "$fallback_started" ] && [ -f "$fallback_active" ]; then
			fallback_replacement_blocked=1
			break
		fi
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.05
	done
	if [ "$fallback_replacement_blocked" -ne 1 ]; then
		echo "UI smoke test: same-source resolution replacement did not reach the controlled map barrier" >&2
		cat "$temporary/fallback-viewer.log" >&2
		exit 1
	fi
	sleep 0.25
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/fallback-blocked.png"
	fallback_left=$(convert "$temporary/fallback-blocked.png" -format \
		"%[fx:p{320,400}.r>0.75&&p{320,400}.b<0.25]" info:)
	fallback_right=$(convert "$temporary/fallback-blocked.png" -format \
		"%[fx:p{960,400}.b>0.75&&p{960,400}.r<0.25]" info:)
	if [ "$fallback_left:$fallback_right" != "1:1" ]; then
		echo "UI smoke test: prior same-source texture disappeared while replacement was blocked ($fallback_left:$fallback_right)" >&2
		cat "$temporary/fallback-viewer.log" >&2
		exit 1
	fi
	: > "$fallback_release"
	stop_viewer

	# Materializing one animation frame for Copy must not make later frames reuse
	# that frame's full-resolution pixels.
	if command -v xclip >/dev/null 2>&1 && [ "$visual_assertions" -eq 1 ]; then
		animation_directory="$temporary/animation-materialization"
		animation_config="$temporary/animation-materialization-config"
		mkdir -p "$animation_directory" "$animation_config/jpegview-linux"
		convert -delay 200 -size 160x120 xc:red -delay 200 -size 160x120 xc:blue \
			-loop 0 "$animation_directory/01-animated.gif"
		printf 'scale_mode=fit\ncache_size_mb=0\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
			> "$animation_config/jpegview-linux/settings.conf"
		VIEWER_TEST_HOME="$temporary/animation-materialization-home" \
			VIEWER_TEST_CONFIG_HOME="$animation_config" \
			launch_viewer "$animation_directory/01-animated.gif"
		assert_title_prefix "01-animated.gif" "animated frame materialization fixture did not load"
		viewer_geometry=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id")
		viewer_width=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^WIDTH=//p')
		viewer_height=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^HEIGHT=//p')
		animation_color=''
		for _ in $(seq 1 100); do
			DISPLAY=":$display_number" import -window "$window_id" "$temporary/animation-frame.png"
			animation_color=$(convert "$temporary/animation-frame.png" -format \
				"%[hex:p{$((viewer_width / 2)),$((viewer_height / 2))}]" info:)
			case "$animation_color" in FF0000|0000FF) break ;; esac
			sleep 0.05
		done
		if [ "$animation_color" != FF0000 ] && [ "$animation_color" != 0000FF ]; then
			echo "UI smoke test: animated frame was not visible ($animation_color)" >&2
			exit 1
		fi
		first_animation_color=$animation_color
		DISPLAY=":$display_number" xdotool key ctrl+c
		copied_animation_color=''
		for _ in $(seq 1 30); do
			if DISPLAY=":$display_number" xclip -selection clipboard -t image/png -o \
				> "$temporary/animation-copy-first.png" 2>/dev/null; then
				copied_animation_color=$(convert "$temporary/animation-copy-first.png" \
					-format '%[hex:p{0,0}]' info: 2>/dev/null || true)
				if [ "${#copied_animation_color}" -eq 8 ]; then
					copied_animation_color=${copied_animation_color%??}
				fi
				if [ "$copied_animation_color" = "$first_animation_color" ]; then break; fi
			fi
			sleep 0.05
		done
		if [ "$copied_animation_color" != "$first_animation_color" ]; then
			echo "UI smoke test: copied animation frame did not match its visible frame ($first_animation_color/$copied_animation_color)" >&2
			exit 1
		fi
		second_animation_color=''
		for _ in $(seq 1 100); do
			DISPLAY=":$display_number" import -window "$window_id" "$temporary/animation-frame.png"
			second_animation_color=$(convert "$temporary/animation-frame.png" -format \
				"%[hex:p{$((viewer_width / 2)),$((viewer_height / 2))}]" info:)
			if [ "$second_animation_color" != "$first_animation_color" ]; then break; fi
			sleep 0.05
		done
		if [ "$second_animation_color" = "$first_animation_color" ]; then
			echo "UI smoke test: animated fixture did not advance to another frame" >&2
			exit 1
		fi
		DISPLAY=":$display_number" xdotool key ctrl+c
		copied_animation_color=''
		for _ in $(seq 1 30); do
			if DISPLAY=":$display_number" xclip -selection clipboard -t image/png -o \
				> "$temporary/animation-copy-second.png" 2>/dev/null; then
				copied_animation_color=$(convert "$temporary/animation-copy-second.png" \
					-format '%[hex:p{0,0}]' info: 2>/dev/null || true)
				if [ "${#copied_animation_color}" -eq 8 ]; then
					copied_animation_color=${copied_animation_color%??}
				fi
				if [ "$copied_animation_color" = "$second_animation_color" ]; then break; fi
			fi
			sleep 0.05
		done
		if [ "$copied_animation_color" != "$second_animation_color" ]; then
			echo "UI smoke test: second copied animation frame reused stale pixels ($second_animation_color/$copied_animation_color)" >&2
			exit 1
		fi
		stop_viewer

		# Saving the selected animated source replaces it with the captured frame.
		# Playback stays paused through file publication, then the materialized
		# document remains available for a later edit.
		animation_save_directory="$temporary/animation-save"
		animation_save_config="$temporary/animation-save-config"
		mkdir -p "$animation_save_directory" "$animation_save_config/jpegview-linux"
		convert -size 600x300 xc:red -fill blue -draw 'rectangle 300,0 599,299' \
			"$temporary/animation-save-first.png"
		convert -size 600x300 xc:green -fill yellow -draw 'rectangle 300,0 599,299' \
			"$temporary/animation-save-second.png"
		convert -delay 1 "$temporary/animation-save-first.png" \
			-delay 1 "$temporary/animation-save-second.png" -loop 0 \
			"$animation_save_directory/01-animated-save.gif"
		# Saving changes the timestamp; an asynchronous timestamp sort can overwrite
		# the save-completion title before its assertion. Keep this fixture in filename order.
		printf 'scale_mode=fit\nsort_mode=file_name\ncache_size_mb=0\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
			> "$animation_save_config/jpegview-linux/settings.conf"
		VIEWER_TEST_HOME="$temporary/animation-save-home" \
			VIEWER_TEST_CONFIG_HOME="$animation_save_config" \
			launch_viewer "$animation_save_directory/01-animated-save.gif"
		assert_title_prefix "01-animated-save.gif" "animated in-place save fixture did not load"
		if [ "$(identify "$animation_save_directory/01-animated-save.gif" | wc -l)" -lt 2 ]; then
			echo "UI smoke test: animated in-place save fixture has fewer than two frames" >&2
			exit 1
		fi
		animation_save_name='01-animated-save.gif'
		animation_save_default="${animation_save_name%.*}_proc.jpg"
		DISPLAY=":$display_number" xdotool key --window "$window_id" ctrl+s
		send_repeated_keypresses "${#animation_save_default}" BackSpace "$window_id"
		DISPLAY=":$display_number" xdotool type --window "$window_id" --delay 25 "$animation_save_name"
		DISPLAY=":$display_number" xdotool key --window "$window_id" Return
		sleep 0.1
		DISPLAY=":$display_number" xdotool key --window "$window_id" Return
		animation_save_flattened=0
		for _ in $(seq 1 100); do
			if [ "$(identify "$animation_save_directory/01-animated-save.gif" | wc -l)" -eq 1 ]; then
				animation_save_flattened=1
				break
			fi
			sleep 0.05
		done
		if [ "$animation_save_flattened" -ne 1 ]; then
			echo "UI smoke test: successful animated in-place save did not publish one captured frame" >&2
			cat "$temporary/viewer.log" >&2
			exit 1
		fi
		assert_title_prefix "Saved processed image: 01-animated-save.gif" \
			"animated in-place save did not complete"
		viewer_geometry=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id")
		viewer_width=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^WIDTH=//p')
		viewer_height=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^HEIGHT=//p')
		animation_save_left_x=$((viewer_width * 35 / 100))
		animation_save_right_x=$((viewer_width * 65 / 100))
		animation_save_center_x=$((viewer_width / 2))
		animation_save_center_y=$((viewer_height / 2))
		animation_save_top_y=$((viewer_height / 3))
		animation_save_bottom_y=$((viewer_height * 2 / 3))
		animation_save_pixels=''
		for _ in $(seq 1 60); do
			DISPLAY=":$display_number" import -window "$window_id" \
				"$temporary/animation-save-after.png"
			animation_save_left=$(convert "$temporary/animation-save-after.png" \
				-format "%[hex:p{$animation_save_left_x,$animation_save_center_y}]" info:)
			animation_save_right=$(convert "$temporary/animation-save-after.png" \
				-format "%[hex:p{$animation_save_right_x,$animation_save_center_y}]" info:)
			case "$animation_save_left:$animation_save_right" in
				FF0000:0000FF|008000:FFFF00)
					animation_save_pixels="$animation_save_left:$animation_save_right"
					break
					;;
			esac
			sleep 0.05
		done
		if [ -z "$animation_save_pixels" ]; then
			echo "UI smoke test: animated in-place save lost its captured presentation ($animation_save_left:$animation_save_right)" >&2
			cat "$temporary/viewer.log" >&2
			exit 1
		fi
		sleep 0.15
		DISPLAY=":$display_number" xdotool key --window "$window_id" Down
		animation_save_rotated=0
		for _ in $(seq 1 60); do
			DISPLAY=":$display_number" import -window "$window_id" \
				"$temporary/animation-save-rotated.png"
			animation_save_rotated_top=$(convert "$temporary/animation-save-rotated.png" \
				-format "%[hex:p{$animation_save_center_x,$animation_save_top_y}]" info:)
			animation_save_rotated_bottom=$(convert "$temporary/animation-save-rotated.png" \
				-format "%[hex:p{$animation_save_center_x,$animation_save_bottom_y}]" info:)
			if [ "$animation_save_rotated_top:$animation_save_rotated_bottom" = \
				"$animation_save_pixels" ]; then
				animation_save_rotated=1
				break
			fi
			sleep 0.05
		done
		if [ "$animation_save_rotated" -ne 1 ]; then
			echo "UI smoke test: edit after animated in-place save did not preserve the captured pixels ($animation_save_pixels -> $animation_save_rotated_top:$animation_save_rotated_bottom)" >&2
			cat "$temporary/viewer.log" >&2
			exit 1
		fi
		stop_viewer

		# A failed in-place save must hand a paused animation back to playback.
		animation_failure_directory="$temporary/animation-save-failure"
		animation_failure_config="$temporary/animation-save-failure-config"
		mkdir -p "$animation_failure_directory" "$animation_failure_config/jpegview-linux"
		convert -delay 5 -size 160x120 xc:red -delay 5 -size 160x120 xc:blue \
			-loop 0 "$animation_failure_directory/01-animated-failure.gif"
		ln -s 01-animated-failure.gif "$animation_failure_directory/fail.xyz"
		printf 'scale_mode=fit\ncache_size_mb=0\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
			> "$animation_failure_config/jpegview-linux/settings.conf"
		VIEWER_TEST_HOME="$temporary/animation-failure-home" \
			VIEWER_TEST_CONFIG_HOME="$animation_failure_config" \
			launch_viewer "$animation_failure_directory/01-animated-failure.gif"
		assert_title_prefix "01-animated-failure.gif" \
			"animated save-failure fixture did not load"
		animation_failure_name='fail.xyz'
		animation_failure_default='01-animated-failure_proc.jpg'
		DISPLAY=":$display_number" xdotool key --window "$window_id" ctrl+s
		send_repeated_keypresses "${#animation_failure_default}" BackSpace "$window_id"
		DISPLAY=":$display_number" xdotool type --window "$window_id" --delay 25 \
			"$animation_failure_name"
		DISPLAY=":$display_number" xdotool key --window "$window_id" Return
		sleep 0.1
		DISPLAY=":$display_number" xdotool key --window "$window_id" Return
		sleep 0.5
		DISPLAY=":$display_number" xdotool key --window "$window_id" Escape
		if [ "$(identify "$animation_failure_directory/01-animated-failure.gif" | wc -l)" -ne 2 ]; then
			echo "UI smoke test: unsupported in-place save changed the animated source" >&2
			cat "$temporary/viewer.log" >&2
			exit 1
		fi
		viewer_geometry=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id")
		viewer_width=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^WIDTH=//p')
		viewer_height=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^HEIGHT=//p')
		animation_failure_color=''
		animation_failure_advanced=0
		for _ in $(seq 1 120); do
			DISPLAY=":$display_number" import -window "$window_id" \
				"$temporary/animation-save-failure.png"
			animation_failure_next=$(convert "$temporary/animation-save-failure.png" \
				-format "%[hex:p{$((viewer_width / 2)),$((viewer_height / 2))}]" info:)
			case "$animation_failure_next" in
				FF0000|0000FF)
					if [ -n "$animation_failure_color" ] && \
						[ "$animation_failure_color" != "$animation_failure_next" ]; then
						animation_failure_advanced=1
						break
					fi
					animation_failure_color=$animation_failure_next
					;;
			esac
			sleep 0.05
		done
		if [ "$animation_failure_advanced" -ne 1 ]; then
			echo "UI smoke test: failed in-place save did not resume animation ($animation_failure_color/$animation_failure_next)" >&2
			cat "$temporary/viewer.log" >&2
			exit 1
		fi
		stop_viewer

		# An edit must hold the visible animation frame until the worker publishes
		# the flattened still. Fast animation used to cancel edits mid-operation.
		animation_edit_directory="$temporary/animation-edit"
		animation_edit_config="$temporary/animation-edit-config"
		mkdir -p "$animation_edit_directory" "$animation_edit_config/jpegview-linux"
		convert -size 3000x2000 xc:red -fill blue \
			-draw 'rectangle 1500,0 2999,1999' "$temporary/animation-edit-first.png"
		convert -size 3000x2000 xc:green -fill yellow \
			-draw 'rectangle 1500,0 2999,1999' "$temporary/animation-edit-second.png"
		convert -delay 1 "$temporary/animation-edit-first.png" \
			-delay 1 "$temporary/animation-edit-second.png" -loop 0 \
			"$animation_edit_directory/01-animated-edit.gif"
		printf 'scale_mode=fit\ncache_size_mb=0\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
			> "$animation_edit_config/jpegview-linux/settings.conf"
		VIEWER_TEST_HOME="$temporary/animation-edit-home" \
			VIEWER_TEST_CONFIG_HOME="$animation_edit_config" \
			launch_viewer "$animation_edit_directory/01-animated-edit.gif"
		assert_title_prefix "01-animated-edit.gif" "animated edit fixture did not load"
		viewer_geometry=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id")
		viewer_width=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^WIDTH=//p')
		viewer_height=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^HEIGHT=//p')
		animation_edit_sample_x=$((viewer_width * 43 / 100))
		animation_edit_top_y=$((viewer_height / 3))
		animation_edit_bottom_y=$((viewer_height * 2 / 3))
		animation_edit_ready=0
		for _ in $(seq 1 100); do
			DISPLAY=":$display_number" import -window "$window_id" \
				"$temporary/animation-edit-before.png"
			animation_edit_top=$(convert "$temporary/animation-edit-before.png" \
				-format "%[hex:p{$animation_edit_sample_x,$animation_edit_top_y}]" info:)
			animation_edit_bottom=$(convert "$temporary/animation-edit-before.png" \
				-format "%[hex:p{$animation_edit_sample_x,$animation_edit_bottom_y}]" info:)
			case "$animation_edit_top:$animation_edit_bottom" in
				FF0000:FF0000|008000:008000) animation_edit_ready=1; break ;;
			esac
			sleep 0.05
		done
		if [ "$animation_edit_ready" -ne 1 ]; then
			echo "UI smoke test: animated edit source was not visible before rotation ($animation_edit_top:$animation_edit_bottom)" >&2
			cat "$temporary/viewer.log" >&2
			exit 1
		fi
		DISPLAY=":$display_number" xdotool key --window "$window_id" Down
		animation_edit_applied=0
		for _ in $(seq 1 100); do
			DISPLAY=":$display_number" import -window "$window_id" \
				"$temporary/animation-edit-after.png"
			animation_edit_top=$(convert "$temporary/animation-edit-after.png" \
				-format "%[hex:p{$animation_edit_sample_x,$animation_edit_top_y}]" info:)
			animation_edit_bottom=$(convert "$temporary/animation-edit-after.png" \
				-format "%[hex:p{$animation_edit_sample_x,$animation_edit_bottom_y}]" info:)
			case "$animation_edit_top:$animation_edit_bottom" in
				FF0000:0000FF|008000:FFFF00) animation_edit_applied=1; break ;;
			esac
			sleep 0.05
		done
		if [ "$animation_edit_applied" -ne 1 ]; then
			echo "UI smoke test: rotation did not finish from a stable animation frame ($animation_edit_top:$animation_edit_bottom)" >&2
			cat "$temporary/viewer.log" >&2
			exit 1
		fi
		sleep 0.15
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/animation-edit-stable.png"
		stable_edit_top=$(convert "$temporary/animation-edit-stable.png" \
			-format "%[hex:p{$animation_edit_sample_x,$animation_edit_top_y}]" info:)
		stable_edit_bottom=$(convert "$temporary/animation-edit-stable.png" \
			-format "%[hex:p{$animation_edit_sample_x,$animation_edit_bottom_y}]" info:)
		if [ "$stable_edit_top:$stable_edit_bottom" != \
			"$animation_edit_top:$animation_edit_bottom" ]; then
			echo "UI smoke test: successful animation edit did not remain flattened ($animation_edit_top:$animation_edit_bottom -> $stable_edit_top:$stable_edit_bottom)" >&2
			cat "$temporary/viewer.log" >&2
			exit 1
		fi
		stop_viewer
	else
		echo "UI smoke test: SKIP animation materialization (missing xclip or visual tools)"
	fi

	# With a controlled two-file list, hold the selected JPEG read and verify
	# the loading state paints while navigation replaces the pending selection.
	async_decode_directory="$temporary/async-decode"
	async_decode_config="$temporary/async-decode-config"
	mkdir -p "$async_decode_directory" "$async_decode_config/jpegview-linux"
	write_solid_ppm "$async_decode_directory/01-fast.ppm" 20 220 80
	convert "$temporary/images/01-red.ppm" "$async_decode_directory/00-slow.jpg"
	printf 'scale_mode=fit\nsort_mode=file_name\nsort_ascending=1\ncache_size_mb=0\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
		> "$async_decode_config/jpegview-linux/settings.conf"
	async_decode_started="$temporary/async-decode.started"
	async_decode_release="$temporary/async-decode.release"
	DISPLAY=":$display_number" HOME="$temporary/async-decode-home" \
		XDG_CONFIG_HOME="$async_decode_config" \
		LD_PRELOAD="$temporary/slow_map.so" \
		JPEGVIEW_TEST_SLOW_MAP="$async_decode_directory/00-slow.jpg" \
		JPEGVIEW_TEST_SLOW_MAP_REPEAT=1 \
		JPEGVIEW_TEST_SLOW_MAP_STARTED="$async_decode_started" \
		JPEGVIEW_TEST_SLOW_MAP_RELEASE="$async_decode_release" \
		JPEGVIEW_TEST_SLOW_MAP_MAX_ATTEMPTS=1000 \
		"$BINARY" "$async_decode_directory/00-slow.jpg" \
		"$async_decode_directory/01-fast.ppm" \
		>"$temporary/async-decode.log" 2>&1 &
	viewer_pid=$!
	window_id=''
	for _ in $(seq 1 40); do
		window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null | head -1 || true)
		if [ -n "$window_id" ]; then break; fi
		sleep 0.05
	done
	if [ -z "$window_id" ]; then
		echo "UI smoke test: async-decode viewer window was unavailable while decode was blocked" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	async_decode_blocked=0
	for _ in $(seq 1 100); do
		if [ -f "$async_decode_started" ]; then async_decode_blocked=1; break; fi
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.05
	done
	if [ "$async_decode_blocked" -ne 1 ]; then
		echo "UI smoke test: selected JPEG decode did not reach the controlled mmap barrier" >&2
		cat "$temporary/async-decode.log" >&2
		exit 1
	fi
	async_decode_loading_visible=0
	for _ in $(seq 1 40); do
		async_decode_title=$(DISPLAY=":$display_number" window_title_without_position)
		case "$async_decode_title" in
			*00-slow.jpg*Loading*) async_decode_loading_visible=1 ;;
		esac
		if [ "$async_decode_loading_visible" -eq 1 ]; then break; fi
		sleep 0.05
	done
	DISPLAY=":$display_number" xdotool key --window "$window_id" Right
	async_decode_navigation_ready=0
	for _ in $(seq 1 40); do
		async_decode_title=$(DISPLAY=":$display_number" window_title_without_position)
		case "$async_decode_title" in
			01-fast.ppm\ *) async_decode_navigation_ready=1; break ;;
		esac
		sleep 0.05
	done
	: > "$async_decode_release"
	if [ "$async_decode_loading_visible" -ne 1 ]; then
		echo "UI smoke test: blocked selected JPEG did not retain its loading state ($async_decode_title)" >&2
		cat "$temporary/async-decode.log" >&2
		exit 1
	fi
	if [ "$async_decode_navigation_ready" -ne 1 ]; then
		echo "UI smoke test: navigation waited for selected-source decoding ($async_decode_title)" >&2
		cat "$temporary/async-decode.log" >&2
		exit 1
	fi
	stop_viewer

	# Source materialization may finish after the filename field changes. Keep
	# the accepted output's overwrite decision with that pending save.
	save_consent_directory="$temporary/save-consent"
	save_consent_config="$temporary/save-consent-config"
	mkdir -p "$save_consent_directory" "$save_consent_config/jpegview-linux"
	convert -size 2400x1200 xc:red "$save_consent_directory/source.jpg"
	printf 'original target' > "$save_consent_directory/A.png"
	printf 'scale_mode=fit\ncache_size_mb=32\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
		> "$save_consent_config/jpegview-linux/settings.conf"
	save_consent_started="$temporary/save-consent.started"
	save_consent_active="$temporary/save-consent.active"
	save_consent_release="$temporary/save-consent.release"
	: > "$save_consent_release"
	DISPLAY=":$display_number" HOME="$temporary/save-consent-home" \
		XDG_CONFIG_HOME="$save_consent_config" XDG_STATE_HOME="$temporary/save-consent-state" \
		LD_PRELOAD="$temporary/slow_map.so" \
		JPEGVIEW_TEST_SLOW_MAP="$save_consent_directory/source.jpg" \
		JPEGVIEW_TEST_SLOW_MAP_REPEAT=1 \
		JPEGVIEW_TEST_SLOW_MAP_STARTED="$save_consent_started" \
		JPEGVIEW_TEST_SLOW_MAP_ACTIVE="$save_consent_active" \
		JPEGVIEW_TEST_SLOW_MAP_RELEASE="$save_consent_release" \
		JPEGVIEW_TEST_SLOW_MAP_MAX_ATTEMPTS=1200 \
		"$BINARY" "$save_consent_directory/source.jpg" \
		>"$temporary/save-consent.log" 2>&1 &
	viewer_pid=$!
	window_id=''
	for _ in $(seq 1 100); do
		window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null | head -1 || true)
		if [ -n "$window_id" ]; then break; fi
		sleep 0.05
	done
	if [ -z "$window_id" ]; then
		echo "UI smoke test: save-consent viewer window was unavailable" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	assert_title_prefix "source.jpg" "save-consent source did not load"
	# Allow startup metadata to finish before holding the full-source read.
	sleep 0.2
	rm -f -- "$save_consent_started" "$save_consent_release"
	DISPLAY=":$display_number" xdotool key --window "$window_id" ctrl+s
	send_repeated_keypresses 15 BackSpace "$window_id"
	DISPLAY=":$display_number" xdotool type --window "$window_id" --delay 25 'A.png'
	DISPLAY=":$display_number" xdotool key --window "$window_id" Return
	sleep 0.1
	DISPLAY=":$display_number" xdotool key --window "$window_id" Return
	save_consent_blocked=0
	for _ in $(seq 1 100); do
		if [ -f "$save_consent_active" ]; then save_consent_blocked=1; break; fi
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.05
	done
	if [ "$save_consent_blocked" -ne 1 ]; then
		echo "UI smoke test: accepted save did not reach the source-preparation barrier" >&2
		cat "$temporary/save-consent.log" >&2
		exit 1
	fi
	send_repeated_keypresses 5 BackSpace "$window_id"
	DISPLAY=":$display_number" xdotool type --window "$window_id" --delay 25 'B.png'
	: > "$save_consent_release"
	save_consent_published=0
	for _ in $(seq 1 100); do
		if [ "$(identify -format '%wx%h' "$save_consent_directory/A.png" 2>/dev/null || true)" = \
			"2400x1200" ]; then save_consent_published=1; break; fi
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.05
	done
	if [ "$save_consent_published" -ne 1 ] || [ -e "$save_consent_directory/B.png" ]; then
		echo "UI smoke test: pending save did not publish its captured output only" >&2
		exit 1
	fi
	stop_viewer

	# Rotate a cold JPEG while its header is blocked. The command must survive
	# the header continuation and be applied after the selected source is ready.
	async_rotate_directory="$temporary/async-rotate"
	async_rotate_config="$temporary/async-rotate-config"
	mkdir -p "$async_rotate_directory" "$async_rotate_config/jpegview-linux"
	convert -size 240x320 xc:red -fill '#00ff00' \
		-draw 'rectangle 0,160 239,319' -sampling-factor 1x1 -quality 100 \
		"$async_rotate_directory/01-rotate.jpg"
	printf 'scale_mode=fit\ncache_size_mb=1\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
		> "$async_rotate_config/jpegview-linux/settings.conf"
	async_rotate_started="$temporary/async-rotate.started"
	async_rotate_release="$temporary/async-rotate.release"
	DISPLAY=":$display_number" HOME="$temporary/async-rotate-home" \
		XDG_CONFIG_HOME="$async_rotate_config" \
		LD_PRELOAD="$temporary/slow_map.so" \
		JPEGVIEW_TEST_SLOW_MAP="$async_rotate_directory/01-rotate.jpg" \
		JPEGVIEW_TEST_SLOW_MAP_REPEAT=1 \
		JPEGVIEW_TEST_SLOW_MAP_STARTED="$async_rotate_started" \
		JPEGVIEW_TEST_SLOW_MAP_RELEASE="$async_rotate_release" \
		JPEGVIEW_TEST_SLOW_MAP_MAX_ATTEMPTS=1000 \
		"$BINARY" "$async_rotate_directory/01-rotate.jpg" \
		>"$temporary/async-rotate.log" 2>&1 &
	viewer_pid=$!
	window_id=''
	for _ in $(seq 1 40); do
		window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null | head -1 || true)
		if [ -n "$window_id" ]; then break; fi
		sleep 0.05
	done
	if [ -z "$window_id" ]; then
		echo "UI smoke test: async-rotate viewer window was unavailable" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	async_rotate_blocked=0
	for _ in $(seq 1 100); do
		if [ -f "$async_rotate_started" ]; then async_rotate_blocked=1; break; fi
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.05
	done
	if [ "$async_rotate_blocked" -ne 1 ]; then
		echo "UI smoke test: selected JPEG header did not reach the controlled mmap barrier" >&2
		cat "$temporary/async-rotate.log" >&2
		exit 1
	fi
	async_rotate_loading_visible=0
	for _ in $(seq 1 40); do
		async_rotate_title=$(DISPLAY=":$display_number" window_title_without_position)
		case "$async_rotate_title" in
			01-rotate.jpg\ *Loading*) async_rotate_loading_visible=1; break ;;
		esac
		sleep 0.05
	done
	DISPLAY=":$display_number" xdotool key --window "$window_id" Up
	sleep 0.05
	: > "$async_rotate_release"
	if [ "$async_rotate_loading_visible" -ne 1 ]; then
		echo "UI smoke test: blocked JPEG did not show a loading state ($async_rotate_title)" >&2
		cat "$temporary/async-rotate.log" >&2
		exit 1
	fi
	if [ "$visual_assertions" -eq 1 ]; then
		async_rotate_applied=0
		viewer_geometry=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id")
		viewer_width=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^WIDTH=//p')
		viewer_height=$(printf '%s\n' "$viewer_geometry" | sed -n 's/^HEIGHT=//p')
		async_rotate_left_x=$((viewer_width / 2 - 40))
		async_rotate_right_x=$((viewer_width / 2 + 40))
		async_rotate_center_y=$((viewer_height / 2))
		for _ in $(seq 1 100); do
			DISPLAY=":$display_number" import -window "$window_id" "$temporary/async-rotate.png"
			async_rotate_left=$(convert "$temporary/async-rotate.png" \
				-format "%[hex:p{$async_rotate_left_x,$async_rotate_center_y}]" info:)
			async_rotate_right=$(convert "$temporary/async-rotate.png" \
				-format "%[hex:p{$async_rotate_right_x,$async_rotate_center_y}]" info:)
			if [ "$async_rotate_left" != "$async_rotate_right" ]; then
				async_rotate_applied=1
				break
			fi
			if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
			sleep 0.05
		done
		if [ "$async_rotate_applied" -ne 1 ]; then
			echo "UI smoke test: rotate queued during JPEG loading was not applied ($async_rotate_left:$async_rotate_right)" >&2
			cat "$temporary/async-rotate.log" >&2
			exit 1
		fi
	fi
	stop_viewer

	# Hold the decoder's memory map until the harness has observed the visible
	# loading window, then release it through an explicit file signal.
	convert "$temporary/images/01-red.ppm" "$temporary/startup-delay.jpg"
	startup_map_started="$temporary/startup-map.started"
	startup_map_release="$temporary/startup-map.release"
	DISPLAY=":$display_number" HOME="$temporary/home" XDG_CONFIG_HOME="$temporary/startup-config" \
		LD_PRELOAD="$temporary/slow_map.so" \
		JPEGVIEW_TEST_SLOW_MAP="$temporary/startup-delay.jpg" \
		JPEGVIEW_TEST_SLOW_MAP_REPEAT=1 \
		JPEGVIEW_TEST_SLOW_MAP_STARTED="$startup_map_started" \
		JPEGVIEW_TEST_SLOW_MAP_RELEASE="$startup_map_release" \
		"$BINARY" "$temporary/startup-delay.jpg" >"$temporary/startup-viewer.log" 2>&1 &
	viewer_pid=$!
	map_started=0
	for _ in $(seq 1 100); do
		if [ -f "$startup_map_started" ]; then map_started=1; break; fi
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.1
	done
	if [ "$map_started" -ne 1 ]; then
		echo "UI smoke test: decoder did not reach the controlled memory-map barrier" >&2
		cat "$temporary/startup-viewer.log" >&2
		exit 1
	fi
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
	: > "$startup_map_release"
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

	# Cold JPEG dimensions arrive after the initial load. Ordinary navigation
	# keeps the current Actual Size mode despite a saved Recents zoom; an explicit
	# open from Recents restores that saved mode when the JPEG header completes.
	manual_zoom_image="$temporary/cold-jpeg-manual/01-manual.jpg"
	manual_zoom_before="$temporary/cold-jpeg-manual/00-before.jpg"
	mkdir -p "$(dirname -- "$manual_zoom_image")"
	convert -size 400x240 xc:red -fill blue -draw 'rectangle 200,0 399,239' \
		-quality 100 "$manual_zoom_image"
	convert -size 400x240 xc:green -quality 100 "$manual_zoom_before"
	manual_zoom_path_hex=$(printf '%s' "$manual_zoom_image" | od -An -tx1 | tr -d ' \n')
	manual_state="$temporary/cold-jpeg-manual-state"
	mkdir -p "$manual_state/jpegview-linux"
	{
		printf '# JPEGView Linux recent files, version 3\n'
		printf 'R %s\n' "$manual_zoom_path_hex"
		printf 'V %s 0 0 0 2 1\n' "$manual_zoom_path_hex"
	} > "$manual_state/jpegview-linux/recent-files.db"
	manual_config="$temporary/cold-jpeg-manual-config"
	mkdir -p "$manual_config/jpegview-linux"
	printf 'scale_mode=manual\ndouble_page_mode_enabled=0\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
		> "$manual_config/jpegview-linux/settings.conf"
	DISPLAY=":$display_number" HOME="$temporary/cold-jpeg-manual-home" \
		XDG_CONFIG_HOME="$manual_config" XDG_STATE_HOME="$manual_state" \
		"$BINARY" "$manual_zoom_before" >"$temporary/cold-jpeg-manual.log" 2>&1 &
	viewer_pid=$!
	manual_zoom_window_id=''
	manual_zoom_title=''
	for _ in $(seq 1 100); do
		for candidate_window in $(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null || true); do
			candidate_title=$(DISPLAY=":$display_number" xdotool getwindowname \
				"$candidate_window" 2>/dev/null || true)
			case "$candidate_title" in
				*"00-before.jpg (400x240"*)
					manual_zoom_window_id=$candidate_window
					manual_zoom_title=$candidate_title
					break
				;;
			esac
		done
		[ -n "$manual_zoom_window_id" ] && break
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.05
	done
	if [ -z "$manual_zoom_window_id" ]; then
		echo "UI smoke test: Actual Size baseline image did not load ($manual_zoom_title)" >&2
		cat "$temporary/cold-jpeg-manual.log" >&2
		exit 1
	fi
	window_id=$manual_zoom_window_id
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	assert_title_prefix "[2/2] " \
		"ordinary-navigation fixture did not finish loading both directory entries"
	DISPLAY=":$display_number" xdotool key --window "$window_id" Right
	assert_title_prefix "01-manual.jpg (400x240" \
		"ordinary navigation did not load the saved Recents viewport fixture"
	if [ "$visual_assertions" -eq 1 ]; then
		ordinary_zoom_rendered=0
		ordinary_left_pixel=0
		ordinary_right_pixel=0
		for _ in $(seq 1 40); do
			DISPLAY=":$display_number" import -window "$window_id" \
				"$temporary/cold-jpeg-manual-ordinary.png"
			ordinary_width=$(identify -format '%w' "$temporary/cold-jpeg-manual-ordinary.png")
			ordinary_height=$(identify -format '%h' "$temporary/cold-jpeg-manual-ordinary.png")
			ordinary_sample_y=$((ordinary_height / 2))
			ordinary_left_x=$((ordinary_width / 2 - 230))
			ordinary_right_x=$((ordinary_width / 2 + 230))
			ordinary_left_pixel=$(convert "$temporary/cold-jpeg-manual-ordinary.png" -format \
				"%[fx:p{$ordinary_left_x,$ordinary_sample_y}.r>0.75&&p{$ordinary_left_x,$ordinary_sample_y}.b<0.25]" info:)
			ordinary_right_pixel=$(convert "$temporary/cold-jpeg-manual-ordinary.png" -format \
				"%[fx:p{$ordinary_right_x,$ordinary_sample_y}.b>0.75&&p{$ordinary_right_x,$ordinary_sample_y}.r<0.25]" info:)
			if [ "$ordinary_left_pixel" = 0 ] && [ "$ordinary_right_pixel" = 0 ]; then
				ordinary_zoom_rendered=1
				break
			fi
			sleep 0.05
		done
		if [ "$ordinary_zoom_rendered" -ne 1 ]; then
			echo "UI smoke test: ordinary navigation applied the image's saved 2x Recents viewport instead of current Actual Size ($ordinary_left_pixel/$ordinary_right_pixel)" >&2
			cat "$temporary/cold-jpeg-manual.log" >&2
			exit 1
		fi
	fi
	stop_viewer
	# The first process correctly saved the ordinary Actual Size view when it
	# closed. Restore a distinct 2x recent snapshot for the explicit-open case.
	{
		printf '# JPEGView Linux recent files, version 3\n'
		printf 'R %s\n' "$manual_zoom_path_hex"
		printf 'V %s 0 0 0 2 1\n' "$manual_zoom_path_hex"
	} > "$manual_state/jpegview-linux/recent-files.db"
	manual_zoom_outside="$temporary/cold-jpeg-manual-outside/00-outside.jpg"
	mkdir -p "$(dirname -- "$manual_zoom_outside")"
	convert -size 400x240 xc:green -quality 100 "$manual_zoom_outside"
	DISPLAY=":$display_number" HOME="$temporary/cold-jpeg-manual-home" \
		XDG_CONFIG_HOME="$manual_config" XDG_STATE_HOME="$manual_state" \
		"$BINARY" "$manual_zoom_outside" >"$temporary/cold-jpeg-manual-recent.log" 2>&1 &
	viewer_pid=$!
	manual_zoom_window_id=''
	manual_zoom_title=''
	for _ in $(seq 1 100); do
		for candidate_window in $(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null || true); do
			candidate_title=$(DISPLAY=":$display_number" xdotool getwindowname \
				"$candidate_window" 2>/dev/null || true)
			case "$candidate_title" in
				*"00-outside.jpg (400x240"*)
					manual_zoom_window_id=$candidate_window
					manual_zoom_title=$candidate_title
					break
				;;
			esac
		done
		[ -n "$manual_zoom_window_id" ] && break
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.05
	done
	if [ -z "$manual_zoom_window_id" ]; then
		echo "UI smoke test: separate image for explicit Recents restoration did not load ($manual_zoom_title)" >&2
		cat "$temporary/cold-jpeg-manual-recent.log" >&2
		exit 1
	fi
	window_id=$manual_zoom_window_id
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	DISPLAY=":$display_number" xdotool key --window "$window_id" ctrl+o
	DISPLAY=":$display_number" xdotool key --window "$window_id" ctrl+Tab
	DISPLAY=":$display_number" xdotool key --window "$window_id" Home
	DISPLAY=":$display_number" xdotool key --window "$window_id" Down
	DISPLAY=":$display_number" xdotool key --window "$window_id" Return
	assert_title_prefix "01-manual.jpg (400x240" \
		"Recents did not explicitly reopen the saved cold JPEG viewport"
	if [ "$visual_assertions" -eq 1 ]; then
		manual_zoom_rendered=0
		manual_left_pixel=0
		manual_right_pixel=0
		for _ in $(seq 1 40); do
			DISPLAY=":$display_number" import -window "$window_id" \
				"$temporary/cold-jpeg-manual.png"
			manual_capture_width=$(identify -format '%w' "$temporary/cold-jpeg-manual.png")
			manual_capture_height=$(identify -format '%h' "$temporary/cold-jpeg-manual.png")
			manual_sample_y=$((manual_capture_height / 2))
			manual_left_x=$((manual_capture_width / 2 - 230))
			manual_right_x=$((manual_capture_width / 2 + 230))
			manual_left_pixel=$(convert "$temporary/cold-jpeg-manual.png" -format \
				"%[fx:p{$manual_left_x,$manual_sample_y}.r>0.75&&p{$manual_left_x,$manual_sample_y}.b<0.25]" info:)
			manual_right_pixel=$(convert "$temporary/cold-jpeg-manual.png" -format \
				"%[fx:p{$manual_right_x,$manual_sample_y}.b>0.75&&p{$manual_right_x,$manual_sample_y}.r<0.25]" info:)
			if [ "$manual_left_pixel" = 1 ] && [ "$manual_right_pixel" = 1 ]; then
				manual_zoom_rendered=1
				break
			fi
			sleep 0.05
		done
		if [ "$manual_zoom_rendered" -ne 1 ]; then
			manual_zoom_final_title=$(DISPLAY=":$display_number" xdotool getwindowname \
				"$window_id" 2>/dev/null || true)
			case "$manual_zoom_final_title" in
				*"01-manual.jpg (400x240"*)
					echo "UI smoke test: explicit Recents open did not render its saved 2x manual zoom after cold JPEG dimensions arrived ($manual_left_pixel/$manual_right_pixel; $manual_zoom_final_title)" >&2
				;;
			*Loading*)
				echo "UI smoke test: cold JPEG remained in its loading state past the bounded render deadline ($manual_zoom_final_title)" >&2
			;;
			*)
				echo "UI smoke test: cold JPEG never reached its loaded title ($manual_zoom_final_title)" >&2
			;;
			esac
			exit 1
		fi
	fi
	stop_viewer

	# Hold a cold JPEG header read while viewport and transform commands arrive.
	# Releasing the read must replay Fit and zoom against incoming dimensions,
	# then apply the queued 90-degree rotation before presenting the image.
	cold_intent_image="$temporary/cold-jpeg-intents/01-intents.jpg"
	mkdir -p "$(dirname -- "$cold_intent_image")"
	convert -size 1600x400 xc:red -fill blue -draw 'rectangle 800,0 1599,399' \
		-quality 100 "$cold_intent_image"
	write_ppm "$(dirname -- "$cold_intent_image")/02-scan-entry.ppm" 32 64 128
	cold_intent_path_hex=$(printf '%s' "$cold_intent_image" | od -An -tx1 | tr -d ' \n')
	cold_intent_state="$temporary/cold-jpeg-intents-state"
	mkdir -p "$cold_intent_state/jpegview-linux"
	{
		printf '# JPEGView Linux recent files, version 3\n'
		printf 'R %s\n' "$cold_intent_path_hex"
		printf 'V %s 1 0 1 1 1\n' "$cold_intent_path_hex"
	} > "$cold_intent_state/jpegview-linux/recent-files.db"
	cold_header_started="$temporary/cold-header.started"
	cold_header_release="$temporary/cold-header.release"
	cold_header_active="$temporary/cold-header.active"
	DISPLAY=":$display_number" HOME="$temporary/cold-intent-home" \
		XDG_CONFIG_HOME="$temporary/cold-intent-config" XDG_STATE_HOME="$cold_intent_state" \
		LD_PRELOAD="$temporary/slow_map.so" \
		JPEGVIEW_TEST_SLOW_MAP="$cold_intent_image" \
		JPEGVIEW_TEST_SLOW_MAP_REPEAT=1 \
		JPEGVIEW_TEST_SLOW_MAP_STARTED="$cold_header_started" \
		JPEGVIEW_TEST_SLOW_MAP_ACTIVE="$cold_header_active" \
		JPEGVIEW_TEST_SLOW_MAP_RELEASE="$cold_header_release" \
		JPEGVIEW_TEST_SLOW_MAP_MAX_ATTEMPTS=2400 \
		"$BINARY" "$cold_intent_image" >"$temporary/cold-jpeg-intents.log" 2>&1 &
	viewer_pid=$!
	cold_intent_window_id=''
	for _ in $(seq 1 200); do
		if [ -f "$cold_header_started" ]; then break; fi
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.025
	done
	if [ ! -f "$cold_header_started" ] || [ ! -f "$cold_header_active" ]; then
		echo "UI smoke test: cold JPEG dimension read did not reach its controlled barrier" >&2
		cat "$temporary/cold-jpeg-intents.log" >&2
		exit 1
	fi
	for _ in $(seq 1 100); do
		cold_intent_window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null | head -1 || true)
		[ -n "$cold_intent_window_id" ] && break
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.025
	done
	if [ -z "$cold_intent_window_id" ]; then
		echo "UI smoke test: viewer did not present while the JPEG header read was blocked" >&2
		cat "$temporary/cold-jpeg-intents.log" >&2
		exit 1
	fi
	window_id=$cold_intent_window_id
	DISPLAY=":$display_number" xdotool windowactivate --sync "$window_id"
	DISPLAY=":$display_number" xdotool windowfocus --sync "$window_id"
	cold_focused_window=''
	for _ in $(seq 1 40); do
		cold_focused_window=$(DISPLAY=":$display_number" xdotool getwindowfocus 2>/dev/null || true)
		[ "$cold_focused_window" = "$window_id" ] && break
		sleep 0.025
	done
	if [ "$cold_focused_window" != "$window_id" ]; then
		echo "UI smoke test: cold-image viewer did not receive keyboard focus ($cold_focused_window != $window_id)" >&2
		exit 1
	fi
	cold_pending_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id")
	case "$cold_pending_title" in
		*Loading*) ;;
		*) echo "UI smoke test: controlled cold JPEG was not visibly pending ($cold_pending_title)" >&2; exit 1 ;;
	esac
	[ -f "$cold_header_active" ] || {
		echo "UI smoke test: cold JPEG header barrier ended before pending commands" >&2
		exit 1
	}
	clear_clipboard_text
	DISPLAY=":$display_number" xdotool key --clearmodifiers --window "$window_id" ctrl+c
	# Queue a transform before viewport commands. The wide-short fixture makes
	# replay order visible because rotating first changes the vertical pan range.
	DISPLAY=":$display_number" xdotool key --clearmodifiers --window "$window_id" Down
	# Fill the remaining 254 slots with 251 Fits, Actual Size, two pans, and the
	# pending whole-image copy. On
	# the wide-short fixture, ordered replay rotates before the vertical pans;
	# grouped replay clamps them against the original short height. The next pan
	# is rejected at capacity and must not cancel the two accepted pans.
	# xdotool treats each argument as a distinct press/release pair; SDL does
	# not count OS autorepeat keydowns for these non-navigation commands.
	send_repeated_keypresses 251 Return "$window_id"
	DISPLAY=":$display_number" xdotool key --clearmodifiers --window "$window_id" space
	DISPLAY=":$display_number" xdotool key --clearmodifiers --window "$window_id" shift+Up
	DISPLAY=":$display_number" xdotool key --clearmodifiers --window "$window_id" shift+Up
	DISPLAY=":$display_number" xdotool key --clearmodifiers --window "$window_id" shift+Down
	[ -f "$cold_header_active" ] || {
		echo "UI smoke test: cold JPEG header barrier ended before the queued rotation" >&2
		exit 1
	}
	intent_limit_reported=0
	for _ in $(seq 1 100); do
		cold_pending_title=$(DISPLAY=":$display_number" xdotool getwindowname \
			"$window_id" 2>/dev/null || true)
		case "$cold_pending_title" in
			*"Loading image header (pending input limit reached)"*)
				intent_limit_reported=1
				break
			;;
		esac
		if [ ! -f "$cold_header_active" ] || ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.025
	done
	if [ "$intent_limit_reported" -ne 1 ]; then
		echo "UI smoke test: pending intent overflow was not reported while retaining header status ($cold_pending_title)" >&2
		exit 1
	fi
	# Name sorting refreshes the generic title while the same header remains
	# pending; the overflow status must remain visible through that refresh.
	DISPLAY=":$display_number" xdotool key --clearmodifiers --window "$window_id" n
	limit_title_retained=0
	for _ in $(seq 1 100); do
		cold_pending_title=$(DISPLAY=":$display_number" xdotool getwindowname \
			"$window_id" 2>/dev/null || true)
		case "$cold_pending_title" in
			*"Loading image header (pending input limit reached)"*)
				limit_title_retained=1
				break
			;;
		esac
		if [ ! -f "$cold_header_active" ] || ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.025
	done
	if [ "$limit_title_retained" -ne 1 ]; then
		echo "UI smoke test: generic title refresh cleared the pending input limit status ($cold_pending_title)" >&2
		exit 1
	fi
	: > "$cold_header_release"
	cold_intent_completed=0
	cold_intent_title=''
	for _ in $(seq 1 200); do
		cold_intent_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id" 2>/dev/null || true)
		case "$cold_intent_title" in
			*"01-intents.jpg (1600x400,"*) cold_intent_completed=1; break ;;
		esac
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.025
	done
	if [ "$cold_intent_completed" -ne 1 ]; then
		echo "UI smoke test: queued cold-image commands did not complete ($cold_intent_title)" >&2
		cat "$temporary/cold-jpeg-intents.log" >&2
		exit 1
	fi
	cold_copy_ready=0
	for _ in $(seq 1 100); do
		if DISPLAY=":$display_number" xclip -selection clipboard -t image/png -o \
			> "$temporary/cold-pending-copy.png" 2>/dev/null; then
			cold_copy_width=$(identify -format '%w' "$temporary/cold-pending-copy.png" 2>/dev/null || true)
			cold_copy_height=$(identify -format '%h' "$temporary/cold-pending-copy.png" 2>/dev/null || true)
			case "$cold_copy_width:$cold_copy_height" in
				0:*|*:0|:*) ;;
				*) cold_copy_ready=1; break ;;
			esac
		fi
		sleep 0.025
	done
	if [ "$cold_copy_ready" -ne 1 ]; then
		echo "UI smoke test: whole-image copy queued during a cold load did not reach the clipboard ($cold_copy_width x $cold_copy_height)" >&2
		cat "$temporary/cold-jpeg-intents.log" >&2
		exit 1
	fi
	if [ "$visual_assertions" -eq 1 ]; then
		cold_intent_rendered=0
		cold_pan_pixel=0
		for _ in $(seq 1 100); do
			DISPLAY=":$display_number" import -window "$window_id" \
				"$temporary/cold-jpeg-intents.png"
			cold_capture_width=$(identify -format '%w' "$temporary/cold-jpeg-intents.png")
			cold_capture_height=$(identify -format '%h' "$temporary/cold-jpeg-intents.png")
			cold_sample_x=$((cold_capture_width / 2))
			cold_pan_y=$((cold_capture_height / 2 + 70))
			cold_pan_pixel=$(convert "$temporary/cold-jpeg-intents.png" -format \
				"%[fx:p{$cold_sample_x,$cold_pan_y}.r>0.75&&p{$cold_sample_x,$cold_pan_y}.b<0.25]" info:)
			if [ "$cold_pan_pixel" = 1 ]; then
				cold_intent_rendered=1
				break
			fi
			sleep 0.025
		done
		if [ "$cold_intent_rendered" -ne 1 ]; then
			echo "UI smoke test: interleaved rotate/Actual Size/pan replay or bounded rejection failed ($cold_pan_pixel)" >&2
			cat "$temporary/cold-jpeg-intents.log" >&2
			exit 1
		fi
	fi
	stop_viewer

	# Clipboard image loading is asynchronous too; its viewport commands must be
	# queued against the temporary source and replayed after the decode commits.
	if command -v xclip >/dev/null 2>&1 && [ "$visual_assertions" -eq 1 ]; then
		clipboard_config="$temporary/clipboard-intents-config"
		clipboard_data="$temporary/clipboard-intents-data"
		mkdir -p "$clipboard_config/jpegview-linux" "$clipboard_data"
		printf 'scale_mode=fit\nspacebar_navigates_images=0\ncache_size_mb=0\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
			> "$clipboard_config/jpegview-linux/settings.conf"
		clipboard_image="$temporary/clipboard-intents.png"
		convert -size 1600x400 xc:black -fill white -draw 'rectangle 775,0 824,399' \
			"$clipboard_image"
		DISPLAY=":$display_number" xclip -selection clipboard -t image/png -i \
			"$clipboard_image"
		clipboard_map_started="$temporary/clipboard-map.started"
		clipboard_map_active="$temporary/clipboard-map.active"
		clipboard_map_release="$temporary/clipboard-map.release"
		VIEWER_TEST_HOME="$temporary/clipboard-intents-home" \
			VIEWER_TEST_CONFIG_HOME="$clipboard_config" XDG_DATA_DIRS="$clipboard_data" \
			LD_PRELOAD="$temporary/slow_map.so" \
			JPEGVIEW_TEST_SLOW_MAP_PREFIX=/tmp/jpegview-paste- \
			JPEGVIEW_TEST_SLOW_MAP_REPEAT=1 \
			JPEGVIEW_TEST_SLOW_MAP_STARTED="$clipboard_map_started" \
			JPEGVIEW_TEST_SLOW_MAP_ACTIVE="$clipboard_map_active" \
			JPEGVIEW_TEST_SLOW_MAP_RELEASE="$clipboard_map_release" \
			JPEGVIEW_TEST_SLOW_MAP_MAX_ATTEMPTS=12000 \
			launch_viewer "$temporary/images/01-red.ppm"
		DISPLAY=":$display_number" xdotool key --window "$window_id" ctrl+v
		clipboard_map_blocked=0
		for _ in $(seq 1 200); do
			if [ -f "$clipboard_map_started" ] && [ -f "$clipboard_map_active" ]; then
				clipboard_map_blocked=1
				break
			fi
			if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
			sleep 0.025
		done
		if [ "$clipboard_map_blocked" -ne 1 ]; then
			echo "UI smoke test: pasted PNG decode did not reach the controlled map barrier" >&2
			cat "$temporary/viewer.log" >&2
			exit 1
		fi
		DISPLAY=":$display_number" xdotool key --window "$window_id" Return
		DISPLAY=":$display_number" xdotool key --window "$window_id" space
		send_repeated_keypresses 8 shift+Right "$window_id"
		: > "$clipboard_map_release"
		clipboard_pan_rendered=0
		clipboard_title=''
		clipboard_center_pixel=''
		for _ in $(seq 1 160); do
			clipboard_title=$(DISPLAY=":$display_number" window_title_without_position)
			case "$clipboard_title" in
				clipboard.png\ *)
					DISPLAY=":$display_number" import -window "$window_id" \
						"$temporary/clipboard-intents-rendered.png"
					clipboard_center_pixel=$(convert "$temporary/clipboard-intents-rendered.png" \
						-format "%[fx:p{640,400}.r<0.15&&p{640,400}.g<0.15&&p{640,400}.b<0.15]" info:)
					if [ "$clipboard_center_pixel" = 1 ]; then
						clipboard_pan_rendered=1
						break
					fi
				;;
			esac
			if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
			sleep 0.025
		done
		if [ "$clipboard_pan_rendered" -ne 1 ]; then
			echo "UI smoke test: fit, actual-size, and pan intents were lost during clipboard loading ($clipboard_title, center=$clipboard_center_pixel)" >&2
			cat "$temporary/viewer.log" >&2
			exit 1
		fi
		stop_viewer
	else
		echo "UI smoke test: SKIP pending clipboard viewport intents (missing xclip or visual tools)"
	fi

	# Reverse to the committed owner while another image's JPEG header is
	# blocked. Startup uses the configured Fit mode despite A's saved Recents 2x
	# view, and the stale read must drain without carrying its 3x view into A.
	owner_return_directory="$temporary/cold-jpeg-owner-return"
	mkdir -p "$owner_return_directory"
	owner_return_image="$owner_return_directory/01-committed.jpg"
	owner_pending_image="$owner_return_directory/02-pending.jpg"
	convert -size 1600x400 xc:black -fill white -draw 'rectangle 775,0 824,399' \
		-quality 100 "$owner_return_image"
	convert -size 160x120 xc:yellow -quality 100 "$owner_pending_image"
	owner_return_hex=$(printf '%s' "$owner_return_image" | od -An -tx1 | tr -d ' \n')
	owner_pending_hex=$(printf '%s' "$owner_pending_image" | od -An -tx1 | tr -d ' \n')
	owner_return_state="$temporary/cold-jpeg-owner-return-state"
	mkdir -p "$owner_return_state/jpegview-linux"
	owner_return_config="$temporary/owner-return-config"
	mkdir -p "$owner_return_config/jpegview-linux"
	printf 'scale_mode=fit\ndouble_page_mode_enabled=0\nthumbnail_panel_visible=0\nshow_histogram=0\n' \
		> "$owner_return_config/jpegview-linux/settings.conf"
	{
		printf '# JPEGView Linux recent files, version 3\n'
		printf 'R %s\n' "$owner_return_hex"
		printf 'V %s 0 0 0 2 1\n' "$owner_return_hex"
		printf 'V %s 0 0 0 3 1\n' "$owner_pending_hex"
	} > "$owner_return_state/jpegview-linux/recent-files.db"
	owner_header_started="$temporary/owner-header.started"
	owner_header_release="$temporary/owner-header.release"
	owner_header_active="$temporary/owner-header.active"
	DISPLAY=":$display_number" HOME="$temporary/owner-return-home" \
		XDG_CONFIG_HOME="$owner_return_config" \
		XDG_STATE_HOME="$owner_return_state" \
		LD_PRELOAD="$temporary/slow_map.so" \
		JPEGVIEW_TEST_SLOW_MAP="$owner_pending_image" \
		JPEGVIEW_TEST_SLOW_MAP_REPEAT=1 \
		JPEGVIEW_TEST_SLOW_MAP_STARTED="$owner_header_started" \
		JPEGVIEW_TEST_SLOW_MAP_ACTIVE="$owner_header_active" \
		JPEGVIEW_TEST_SLOW_MAP_RELEASE="$owner_header_release" \
		JPEGVIEW_TEST_SLOW_MAP_MAX_ATTEMPTS=12000 \
		"$BINARY" "$owner_return_image" >"$temporary/cold-jpeg-owner-return.log" 2>&1 &
	viewer_pid=$!
	window_id=''
	for _ in $(seq 1 100); do
		window_id=$(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null | head -1 || true)
		[ -n "$window_id" ] && break
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.025
	done
	if [ -z "$window_id" ]; then
		echo "UI smoke test: owner-reversal viewer window did not appear" >&2
		cat "$temporary/cold-jpeg-owner-return.log" >&2
		exit 1
	fi
	owner_initial_loaded=0
	for _ in $(seq 1 240); do
		owner_title=$(DISPLAY=":$display_number" xdotool getwindowname "$window_id" 2>/dev/null || true)
		case "$owner_title" in
			*"[1/2]"*"01-committed.jpg (1600x400"*)
				owner_initial_loaded=1
				break
			;;
		esac
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.025
	done
	if [ "$owner_initial_loaded" -ne 1 ]; then
		echo "UI smoke test: committed image did not load with its saved 2x view ($owner_title)" >&2
		cat "$temporary/cold-jpeg-owner-return.log" >&2
		exit 1
	fi
	owner_window_title() {
		if command -v xprop >/dev/null 2>&1; then
			DISPLAY=":$display_number" xprop -id "$window_id" WM_NAME 2>/dev/null |
				sed -n 's/^WM_NAME(STRING) = "\(.*\)"$/\1/p'
		else
			DISPLAY=":$display_number" xdotool getwindowname "$window_id" 2>/dev/null || true
		fi
	}
	if [ "$visual_assertions" -eq 1 ]; then
		owner_initial_white_run=0
		for _ in $(seq 1 100); do
			DISPLAY=":$display_number" import -window "$window_id" \
				"$temporary/cold-jpeg-owner-return-before.png"
			owner_capture_width=$(identify -format '%w' \
				"$temporary/cold-jpeg-owner-return-before.png")
			owner_capture_height=$(identify -format '%h' \
				"$temporary/cold-jpeg-owner-return-before.png")
			owner_sample_y=$((owner_capture_height / 2))
			owner_initial_white_run=$(convert "$temporary/cold-jpeg-owner-return-before.png" \
				-crop "${owner_capture_width}x1+0+${owner_sample_y}" +repage \
				-threshold 70% txt:- | awk '
					NR == 1 { next }
					/#FFFFFF/ { current++; if (current > maximum) maximum = current; next }
					{ current = 0 }
					END { print maximum + 0 }
				')
			if [ "$owner_initial_white_run" -ge 20 ] && [ "$owner_initial_white_run" -le 60 ]; then
				break
			fi
			sleep 0.025
		done
		if [ "$owner_initial_white_run" -lt 20 ] || [ "$owner_initial_white_run" -gt 60 ]; then
			echo "UI smoke test: startup did not keep the configured Fit mode over A's saved Recents 2x view (white stripe width $owner_initial_white_run pixels)" >&2
			exit 1
		fi
	fi
	DISPLAY=":$display_number" xdotool key --clearmodifiers --window "$window_id" Right
	owner_pending_seen=0
	for _ in $(seq 1 160); do
		owner_title=$(owner_window_title)
		case "$owner_title" in
			*"02-pending.jpg [2/2]"*"Loading image header"*)
				if [ -f "$owner_header_active" ]; then owner_pending_seen=1; break; fi
			;;
		esac
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.025
	done
	if [ "$owner_pending_seen" -ne 1 ]; then
		echo "UI smoke test: B did not reach the controlled cold-header state ($owner_title)" >&2
		cat "$temporary/cold-jpeg-owner-return.log" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool key --clearmodifiers --window "$window_id" Left
	owner_return_ready=0
	for _ in $(seq 1 160); do
		owner_title=$(owner_window_title)
		case "$owner_title" in
			*"01-committed.jpg (1600x400,"*)
				if [ -f "$owner_header_active" ]; then owner_return_ready=1; break; fi
			;;
		esac
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.025
	done
	if [ "$owner_return_ready" -ne 1 ]; then
		echo "UI smoke test: returning to A waited for or lost its blocked B replacement ($owner_title)" >&2
		cat "$temporary/cold-jpeg-owner-return.log" >&2
		exit 1
	fi
	: > "$owner_header_release"
	owner_settled_polls=0
	owner_stale_drained=0
	for _ in $(seq 1 240); do
		owner_title=$(owner_window_title)
		case "$owner_title" in
			*"01-committed.jpg (1600x400,"*)
				if [ ! -f "$owner_header_active" ]; then
					owner_settled_polls=$((owner_settled_polls + 1))
				else
					owner_settled_polls=0
				fi
			;;
			*) owner_settled_polls=0 ;;
		esac
		if [ "$owner_settled_polls" -ge 10 ]; then
			owner_stale_drained=1
			break
		fi
		if ! kill -0 "$viewer_pid" 2>/dev/null; then break; fi
		sleep 0.025
	done
	if [ "$owner_stale_drained" -ne 1 ]; then
		echo "UI smoke test: canceled B did not drain while A remained selected ($owner_title)" >&2
		cat "$temporary/cold-jpeg-owner-return.log" >&2
		exit 1
	fi
	if [ "$visual_assertions" -eq 1 ]; then
		DISPLAY=":$display_number" import -window "$window_id" \
			"$temporary/cold-jpeg-owner-return-after.png"
		if ! compare -metric AE "$temporary/cold-jpeg-owner-return-before.png" \
			"$temporary/cold-jpeg-owner-return-after.png" null: \
			2>"$temporary/cold-jpeg-owner-return-difference.txt"; then
			owner_return_difference=$(cat "$temporary/cold-jpeg-owner-return-difference.txt")
			echo "UI smoke test: returning to A changed its current Fit viewport ($owner_return_difference differing pixels)" >&2
			exit 1
		fi
	fi
	stop_viewer

	# A failed cold header probe must fall through to decode failure, preserve a
	# same-folder recent row, and return the nonzero startup status.
	malformed_jpeg="$temporary/malformed-startup/01-malformed.jpg"
	mkdir -p "$(dirname -- "$malformed_jpeg")"
	printf 'not a JPEG image\000\377' > "$malformed_jpeg"
	malformed_existing_recent="$temporary/malformed-startup/00-existing.jpg"
	malformed_existing_recent_hex=$(printf '%s' "$malformed_existing_recent" | od -An -tx1 | tr -d ' \n')
	malformed_state="$temporary/malformed-startup-state"
	mkdir -p "$malformed_state/jpegview-linux"
	{
		printf '# JPEGView Linux recent files, version 3\n'
		printf 'R %s\n' "$malformed_existing_recent_hex"
	} > "$malformed_state/jpegview-linux/recent-files.db"
	malformed_jpeg_hex=$(printf '%s' "$malformed_jpeg" | od -An -tx1 | tr -d ' \n')
	if DISPLAY=":$display_number" timeout --foreground 5s \
		env HOME="$temporary/malformed-startup-home" \
		XDG_CONFIG_HOME="$temporary/malformed-startup-config" \
		XDG_STATE_HOME="$malformed_state" \
		"$BINARY" "$malformed_jpeg" >"$temporary/malformed-startup.log" 2>&1; then
		malformed_status=0
	else
		malformed_status=$?
	fi
	if [ "$malformed_status" -ne 1 ]; then
		echo "UI smoke test: malformed cold JPEG startup returned $malformed_status instead of 1" >&2
		cat "$temporary/malformed-startup.log" >&2
		exit 1
	fi
	malformed_recent_rows=$(awk '$1 == "R" { print $2 }' \
		"$malformed_state/jpegview-linux/recent-files.db")
	if [ "$malformed_recent_rows" != "$malformed_existing_recent_hex" ] || \
		printf '%s\n' "$malformed_recent_rows" | grep -Fqx -- "$malformed_jpeg_hex"; then
		echo "UI smoke test: failed cold JPEG changed the same-folder recent image" >&2
		cat "$malformed_state/jpegview-linux/recent-files.db" >&2
		exit 1
	fi
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

# Advanced configuration appears immediately before Help in the compact menu.
# The dialog stages values by category and persists only after Apply.
XDG_STATE_HOME="$temporary/advanced-config-state"
VIEWER_TEST_CONFIG_HOME="$temporary/advanced-config-config"
export XDG_STATE_HOME VIEWER_TEST_CONFIG_HOME
launch_viewer "$temporary/images/01-red.ppm"
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 400 click 3
DISPLAY=":$display_number" xdotool key End
DISPLAY=":$display_number" xdotool key Up Up Up
DISPLAY=":$display_number" xdotool key Return
sleep 0.15
DISPLAY=":$display_number" xdotool key Down Down Return
DISPLAY=":$display_number" xdotool key Down Return
DISPLAY=":$display_number" xdotool key Tab
DISPLAY=":$display_number" xdotool key Return
DISPLAY=":$display_number" xdotool key Down Down Return
DISPLAY=":$display_number" xdotool key ctrl+a
DISPLAY=":$display_number" xdotool type --delay 30 --clearmodifiers '%f - [%p] - %a'
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
grep -q '^fit_relative_zoom_mode=1$' "$advanced_config_settings"
grep -Fqx 'window_title_pattern=%f - [%p] - %a' "$advanced_config_settings"
assert_title_prefix "01-red.ppm" "applying advanced configuration did not return to the viewer"
assert_title_prefix "01-red.ppm - [1/" "advanced configuration did not apply the reordered title pattern"
if [ "$visual_assertions" -eq 1 ]; then
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/fit-relative-before.png"
	zoom_capture_width=$(identify -format '%w' "$temporary/fit-relative-before.png")
	zoom_capture_height=$(identify -format '%h' "$temporary/fit-relative-before.png")
	zoom_readout_base=$(convert "$temporary/fit-relative-before.png" -format \
		"%[hex:p{$((zoom_capture_width - 7)),$((zoom_capture_height - 9))}]" info:)
	DISPLAY=":$display_number" xdotool key ctrl+Up
	zoom_readout_visible=0
	for _ in $(seq 1 20); do
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/fit-relative-zoom.png"
		zoom_readout_border=$(convert "$temporary/fit-relative-zoom.png" -format \
			"%[hex:p{$((zoom_capture_width - 7)),$((zoom_capture_height - 9))}]" info:)
		case "$zoom_readout_border" in
			787878*) zoom_readout_visible=1; break ;;
		esac
		sleep 0.05
	done
	if [ "$zoom_readout_visible" -ne 1 ]; then
		echo "UI smoke test: fit-relative zoom action did not draw its readout (pixel $zoom_readout_border)" >&2
		exit 1
	fi
	zoom_readout_expired=0
	for _ in $(seq 1 50); do
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/fit-relative-expired.png"
		zoom_readout_after=$(convert "$temporary/fit-relative-expired.png" -format \
			"%[hex:p{$((zoom_capture_width - 7)),$((zoom_capture_height - 9))}]" info:)
		if [ "$zoom_readout_after" = "$zoom_readout_base" ]; then
			zoom_readout_expired=1
			break
		fi
		sleep 0.05
	done
	if [ "$zoom_readout_expired" -ne 1 ]; then
		echo "UI smoke test: fit-relative zoom readout did not expire and restore the prior pixel" >&2
		exit 1
	fi
fi
DISPLAY=":$display_number" xdotool key Left
sleep 0.1
assert_title_prefix "01-red.ppm" "disabled folder wrap moved past the first image"
DISPLAY=":$display_number" xdotool key Right
sleep 0.1
assert_title_prefix "02-green.ppm" "folder navigation did not continue from an interior image"
assert_title_prefix "02-green.ppm - [2/" "window title pattern did not update its position after navigation"

# Reopening and escaping discards an un-applied draft.
DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 640 400 click 3
DISPLAY=":$display_number" xdotool key End
DISPLAY=":$display_number" xdotool key Up Up Up
DISPLAY=":$display_number" xdotool key Return
sleep 0.1
DISPLAY=":$display_number" xdotool key Tab
DISPLAY=":$display_number" xdotool key Return
DISPLAY=":$display_number" xdotool key Escape
sleep 0.1
grep -q '^transparency_pattern=white$' "$advanced_config_settings"
grep -q '^folder_wrap_around=0$' "$advanced_config_settings"
grep -q '^fit_relative_zoom_mode=1$' "$advanced_config_settings"
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

if [ "$visual_assertions" -eq 1 ]; then
	# Archive-aware labels are prepared before painting: archive files stay gold,
	# while an ordinary directory with an archive suffix stays a normal folder.
	if [ -f "$temporary/images/06-archive.zip" ]; then
		DISPLAY=":$display_number" xdotool key ctrl+o
		DISPLAY=":$display_number" xdotool type --delay 20 '06-archive.zip'
		sleep 0.3
		DISPLAY=":$display_number" import -window "$window_id" "$temporary/archive-row.png"
		archive_row_color=$(convert "$temporary/archive-row.png" +repage \
			-format %c histogram:info:- | grep -c 'srgb(255,205,125)' || true)
		if [ "$archive_row_color" -eq 0 ]; then
			echo "UI smoke test: prepared Browse archive-format label was not rendered in archive color" >&2
			exit 1
		fi
		DISPLAY=":$display_number" xdotool key Escape
	fi
	DISPLAY=":$display_number" xdotool key ctrl+o
	DISPLAY=":$display_number" xdotool type --delay 20 '16-ordinary.zip'
	sleep 0.3
	DISPLAY=":$display_number" import -window "$window_id" "$temporary/ordinary-zip-directory-row.png"
	ordinary_directory_color=$(convert "$temporary/ordinary-zip-directory-row.png" +repage \
		-format %c histogram:info:- | grep -c 'srgb(185,205,235)' || true)
	if [ "$ordinary_directory_color" -eq 0 ]; then
		echo "UI smoke test: an ordinary .zip-named directory was rendered as an archive" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool key Escape
fi

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
	if [ "$visual_assertions" -eq 1 ]; then
		recent_archive_parent_color=0
		for _ in $(seq 1 30); do
			DISPLAY=":$display_number" import -window "$window_id" \
				"$temporary/recent-archive-parent.png"
			recent_archive_parent_color=$(convert "$temporary/recent-archive-parent.png" +repage \
				-format %c histogram:info:- | grep -c 'srgb(210,170,105)' || true)
			if [ "$recent_archive_parent_color" -gt 0 ]; then break; fi
			sleep 0.1
		done
		if [ "$recent_archive_parent_color" -eq 0 ]; then
			echo "UI smoke test: Recents did not render the prepared archive parent label" >&2
			exit 1
		fi
	fi
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

# A large ordinary folder is enumerated off the event thread. Escape must
# close the dialog while that replaceable listing is still in progress.
slow_browse_directory="$temporary/images/00-slow-browse"
mkdir -p "$slow_browse_directory"
for index in $(seq 0 14999); do
	filename=$(printf '%05d' "$index")
	: > "$slow_browse_directory/$filename.jpg"
done
DISPLAY=":$display_number" xdotool key ctrl+o
sleep 0.5
DISPLAY=":$display_number" xdotool type --delay 1 '00-slow-browse'
DISPLAY=":$display_number" xdotool key Return
DISPLAY=":$display_number" xdotool key Escape
DISPLAY=":$display_number" xdotool key Right
assert_title_prefix "04-yellow.ppm" "Escape did not close Browse during a large-directory listing"
DISPLAY=":$display_number" xdotool key Left
assert_title_prefix "03-blue.ppm" "large-directory Escape test did not restore the starting image"
rm -rf -- "$slow_browse_directory"

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

# The known 01 -> 02 -> 01 sequence leaves a valid next image in the active
# directory. Exercise held navigation here, before later dialog interactions
# can leave a standalone image or an endpoint selected.
title_before_hold=$(DISPLAY=":$display_number" window_title_without_position)
DISPLAY=":$display_number" xdotool keydown Right
# Xvfb's default initial keyboard-repeat delay is near 0.7 seconds. Leave a
# wider margin so the assertion observes repeat events under load.
sleep 1.3
DISPLAY=":$display_number" xdotool keyup Right
sleep 0.2
title_after_hold=$(DISPLAY=":$display_number" window_title_without_position)
if [ "$title_before_hold" = "$title_after_hold" ]; then
	echo "UI smoke test: held Right key did not repeat navigation (before '$title_before_hold'; after '$title_after_hold')" >&2
	exit 1
fi
sleep 0.3
title_after_release_settled=$(DISPLAY=":$display_number" window_title_without_position)
if [ "$title_after_hold" != "$title_after_release_settled" ]; then
	echo "UI smoke test: navigation continued after Right was released" >&2
	exit 1
fi

title_before=$(DISPLAY=":$display_number" window_title_without_position)
DISPLAY=":$display_number" xdotool mousemove 640 400
DISPLAY=":$display_number" xdotool click 4
sleep 0.4
title_after_wheel=$(DISPLAY=":$display_number" window_title_without_position)
if [ "$title_before" = "$title_after_wheel" ]; then
	echo "UI smoke test: plain wheel did not navigate" >&2
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
	# “Show Advanced Options” gets the `A` mnemonic after shared commands keep
	# their full-menu assignments. Its five-pixel glyph ink ends at x=686;
	# x=687 is blank cell space and must not get an underline endpoint. The menu
	# is opened at (640,20), and this underline is at y=36.
	mnemonic_ink_pixel=$(convert "$temporary/context-compact.png" -crop 1x1+686+36 +repage \
		-colorspace Gray -threshold 50% -format "%[fx:mean]" info:)
	mnemonic_trailing_pixel=$(convert "$temporary/context-compact.png" -crop 1x1+687+36 +repage \
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
	preview_image_height=$((preview_rows * 26 - 64))
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
	preview_footer_y=$((reopened_dialog_y + 112 + preview_rows * 26 - 19))
	preview_footer_left=$((preview_left + 8))
	preview_footer_half=$((preview_image_width / 2))
	preview_footer_left_ink=$(convert "$temporary/recent-dialog-preview.png" \
		-crop "${preview_footer_half}x14+${preview_footer_left}+${preview_footer_y}" +repage txt:- |
		awk '/srgb\(165,175,185\)/ { found = 1; exit } END { if (found) print "present" }')
	preview_footer_right_x=$((preview_footer_left + preview_footer_half))
	preview_footer_right_width=$((preview_image_width - preview_footer_half))
	preview_footer_right_ink=$(convert "$temporary/recent-dialog-preview.png" \
		-crop "${preview_footer_right_width}x14+${preview_footer_right_x}+${preview_footer_y}" +repage txt:- |
		awk '/srgb\(165,175,185\)/ { found = 1; exit } END { if (found) print "present" }')
	preview_old_details_y=$((reopened_dialog_y + 112 + preview_rows * 26 - 37))
	preview_old_details_ink=$(convert "$temporary/recent-dialog-preview.png" \
		-crop "${preview_image_width}x14+${preview_left}+${preview_old_details_y}" +repage txt:- |
		awk '/srgb\(165,175,185\)/ { found = 1; exit } END { if (found) print "present" }')
	if [ -z "$preview_footer_left_ink" ] || [ -z "$preview_footer_right_ink" ] || \
		[ -n "$preview_old_details_ink" ]; then
		echo "UI smoke test: preview filename and image details were not combined on one footer row" >&2
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
	crop_viewer_ready=0
	crop_window_title=''
	for _ in $(seq 1 100); do
		for candidate_window in $(DISPLAY=":$display_number" xdotool search --onlyvisible \
			--class jpegview-linux 2>/dev/null || true); do
			candidate_title=$(DISPLAY=":$display_number" xdotool getwindowname "$candidate_window" 2>/dev/null || true)
			crop_window_title=$candidate_title
			case "$candidate_title" in
				*"01-crop.jpg (160x128,"*)
					window_id=$candidate_window
					crop_viewer_ready=1
					break
				;;
			esac
		done
		[ "$crop_viewer_ready" -eq 1 ] && break
		sleep 0.05
	done
	if [ "$crop_viewer_ready" -ne 1 ]; then
		echo "UI smoke test: crop viewer did not finish loading its initial JPEG ($crop_window_title)" >&2
		exit 1
	fi
	DISPLAY=":$display_number" xdotool windowactivate "$window_id"
	crop_window_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^WIDTH=//p')
	crop_window_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" | sed -n 's/^HEIGHT=//p')
	crop_image_left=$((crop_window_width / 2 - 80))
	crop_image_top=$((crop_window_height / 2 - 64))
	crop_settings="$temporary/crop-config/jpegview-linux/settings.conf"
	# A fresh launch must leave normal drags in view mode rather than silently creating a crop.
	if [ "$visual_assertions" -eq 1 ]; then
		crop_image_rendered=0
		for _ in $(seq 1 100); do
			DISPLAY=":$display_number" import -window "$window_id" "$temporary/crop-image-readiness.png"
			crop_image_pixels=$(convert "$temporary/crop-image-readiness.png" -format \
				"%[fx:p{$((crop_image_left + 32)),$((crop_image_top + 32))}.r>0.7&&p{$((crop_image_left + 32)),$((crop_image_top + 32))}.b<0.3],%[fx:p{$((crop_image_left + 128)),$((crop_image_top + 32))}.b>0.7&&p{$((crop_image_left + 128)),$((crop_image_top + 32))}.r<0.3]" info:)
			if [ "$crop_image_pixels" = "1,1" ]; then
				crop_image_rendered=1
				break
			fi
			sleep 0.05
		done
		if [ "$crop_image_rendered" -ne 1 ]; then
			echo "UI smoke test: crop viewer did not render its initial JPEG pixels before the visual assertion ($crop_image_pixels)" >&2
			exit 1
		fi
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
		copied_selection_title=$(DISPLAY=":$display_number" window_title_without_position)
		if [ ! -s "$temporary/copied-selection.png" ] || \
			[ "$copied_selection_dimensions" != "64x48" ]; then
			echo "UI smoke test: Copy Selection did not place its source-size crop on the clipboard ($copied_selection_dimensions; $copied_selection_title)" >&2
			cat "$temporary/crop-viewer.log" >&2
			exit 1
		fi
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

	if command -v xclip >/dev/null 2>&1; then
		selection_copy_config="$temporary/selection-copy-config"
		mkdir -p "$selection_copy_config/jpegview-linux"
		printf 'scale_mode=fit_no_enlarge\nselection_mode_enabled=1\ncopy_selection_on_release=1\nthumbnail_panel_visible=0\n' \
			> "$selection_copy_config/jpegview-linux/settings.conf"
		VIEWER_TEST_HOME="$temporary/home" VIEWER_TEST_CONFIG_HOME="$selection_copy_config" \
			launch_viewer "$temporary/crop-images/01-crop.jpg"
		assert_title_prefix "01-crop.jpg" \
			"copy-on-release crop viewer did not load its initial image"
		selection_copy_width=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" |
			sed -n 's/^WIDTH=//p')
		selection_copy_height=$(DISPLAY=":$display_number" xdotool getwindowgeometry --shell "$window_id" |
			sed -n 's/^HEIGHT=//p')
		selection_copy_left=$((selection_copy_width / 2 - 80))
		selection_copy_top=$((selection_copy_height / 2 - 64))
		clear_clipboard_text
		DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
			$((selection_copy_left + 32)) $((selection_copy_top + 32)) mousedown 1
		sleep 0.1
		DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
			$((selection_copy_left + 127)) $((selection_copy_top + 95)) mouseup 1
		selection_copy_dimensions=''
		for _ in $(seq 1 40); do
			if DISPLAY=":$display_number" xclip -selection clipboard -t image/png -o \
				>"$temporary/automatic-selection-copy.png" 2>/dev/null; then
				selection_copy_dimensions=$(identify -format '%wx%h' \
					"$temporary/automatic-selection-copy.png" 2>/dev/null || true)
				if [ "$selection_copy_dimensions" = "96x64" ]; then break; fi
			fi
			sleep 0.1
		done
		selection_copy_title=$(DISPLAY=":$display_number" window_title_without_position)
		if [ "$selection_copy_dimensions" != "96x64" ] || \
			[ "$selection_copy_title" != "Copied selection to clipboard" ]; then
			echo "UI smoke test: copy-on-release did not copy and complete the source-size selection ($selection_copy_dimensions; $selection_copy_title)" >&2
			cat "$temporary/viewer.log" >&2
			exit 1
		fi
		if [ "$visual_assertions" -eq 1 ]; then
			DISPLAY=":$display_number" import -window "$window_id" \
				"$temporary/automatic-selection-copy-cleared.png"
			selection_copy_border=$(convert "$temporary/automatic-selection-copy-cleared.png" \
				-format "%[pixel:p{$((selection_copy_left + 32)),$((selection_copy_top + 32))}]" info:)
			if [ "$selection_copy_border" = "srgb(255,205,0)" ]; then
				echo "UI smoke test: automatic selection copy left the selection overlay visible" >&2
				exit 1
			fi
		fi
		set_clipboard_text 'selection-copy-shift-sentinel'
		DISPLAY=":$display_number" xdotool keydown Shift_L
		DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
			$((selection_copy_left + 32)) $((selection_copy_top + 32)) mousedown 1
		sleep 0.1
		DISPLAY=":$display_number" xdotool mousemove --window "$window_id" \
			$((selection_copy_left + 127)) $((selection_copy_top + 95))
		DISPLAY=":$display_number" xdotool mouseup 1 keyup Shift_L
		shift_clipboard_text=$(DISPLAY=":$display_number" xclip -selection clipboard -o 2>/dev/null || true)
		if [ "$shift_clipboard_text" != 'selection-copy-shift-sentinel' ]; then
			echo "UI smoke test: Shift-zoom triggered copy-on-release ($shift_clipboard_text)" >&2
			exit 1
		fi
		clear_clipboard_text
		DISPLAY=":$display_number" xdotool mousemove --window "$window_id" 8 8
		stop_viewer
	fi

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

"$SCRIPT_DIR/thumbnail_panning_smoke.sh" "$BINARY"

# Keep this content-detection case last so its Viewer/Recents state cannot
# influence any other smoke case.
if command -v convert >/dev/null 2>&1 &&
	convert -list format | grep -Eq '^[[:space:]]*WEBP\*?[[:space:]]'; then
	mislabeled_webp="$temporary/misnamed-webp.jpg"
	convert -size 640x480 xc:'#d04070' -quality 82 "webp:$mislabeled_webp"
	mislabeled_webp_previous_state=$XDG_STATE_HOME
	XDG_STATE_HOME="$temporary/misnamed-webp-state"
	export XDG_STATE_HOME
	launch_viewer "$mislabeled_webp"
	assert_title_prefix "misnamed-webp.jpg (640x480" \
		"JPEG-named RIFF/WEBP content did not reach a ready image presentation"
	stop_viewer
	XDG_STATE_HOME=$mislabeled_webp_previous_state
	export XDG_STATE_HOME
else
	echo "UI smoke test: SKIP JPEG-named WebP case (ImageMagick WebP encoder is unavailable)"
fi

echo "UI smoke tests passed"

#!/bin/sh
set -eu

APPIMAGE=${1:?usage: appimage_x11_smoke.sh /path/to/application.AppImage}
APPIMAGE=$(cd -- "$(dirname -- "$APPIMAGE")" && pwd)/$(basename -- "$APPIMAGE")

for command in xvfb-run xwininfo; do
	if ! command -v "$command" >/dev/null 2>&1; then
		echo "AppImage X11 smoke test requires $command" >&2
		exit 2
	fi
done

export APPIMAGE_EXTRACT_AND_RUN=1
# The single-quoted body is intentionally evaluated by the nested shell so its
# variables refer to the Xvfb session, not this wrapper.
# shellcheck disable=SC2016
xvfb-run -a -s '-screen 0 1280x900x24 -nolisten tcp' sh -eu -c '
	appimage=$1
	"$appimage" &
	app_pid=$!
	cleanup() {
		kill "$app_pid" 2>/dev/null || true
		wait "$app_pid" 2>/dev/null || true
	}
	trap cleanup EXIT HUP INT TERM

	attempt=0
	while [ "$attempt" -lt 250 ]; do
		if ! kill -0 "$app_pid" 2>/dev/null; then
			wait "$app_pid" || true
			echo "AppImage exited before creating its X11 window" >&2
			exit 1
		fi
		window_tree=$(xwininfo -root -tree 2>/dev/null || true)
		if printf "%s\n" "$window_tree" | grep -q JPEGView; then
			echo "AppImage created its JPEGView X11 window"
			exit 0
		fi
		sleep 0.1
		attempt=$((attempt + 1))
	done
	echo "AppImage did not create its JPEGView X11 window within 25 seconds" >&2
	printf "%s\n" "$window_tree" >&2
	exit 1
' appimage-x11-smoke "$APPIMAGE"

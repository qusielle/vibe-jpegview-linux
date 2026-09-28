#!/bin/sh
set -u

attempt_limit=${JPEGVIEW_RETRY_ATTEMPTS:-3}
delay_seconds=${JPEGVIEW_RETRY_INITIAL_DELAY_SECONDS:-15}
max_delay_seconds=${JPEGVIEW_RETRY_MAX_DELAY_SECONDS:-60}

is_unsigned_integer() {
	case "$1" in
		''|*[!0-9]*) return 1 ;;
		*) return 0 ;;
	esac
}

if ! is_unsigned_integer "$attempt_limit" || [ "$attempt_limit" -lt 1 ] \
	|| ! is_unsigned_integer "$delay_seconds" \
	|| ! is_unsigned_integer "$max_delay_seconds"; then
	echo 'retry-command: invalid retry settings' >&2
	exit 2
fi
if [ "$#" -lt 2 ] || [ "$1" != -- ]; then
	echo 'Usage: retry-command.sh -- command [argument ...]' >&2
	exit 2
fi
shift

if [ "$delay_seconds" -gt "$max_delay_seconds" ]; then
	delay_seconds=$max_delay_seconds
fi

attempt=1
while :; do
	printf 'Retry wrapper: attempt %s/%s\n' "$attempt" "$attempt_limit"
	if "$@"; then
		exit 0
	else
		status=$?
	fi

	if [ "$attempt" -ge "$attempt_limit" ]; then
		printf 'Retry wrapper: command failed after %s attempts (exit %s)\n' \
			"$attempt" "$status" >&2
		exit "$status"
	fi

	printf 'Retry wrapper: exit %s; retrying in %s seconds\n' \
		"$status" "$delay_seconds" >&2
	if ! sleep "$delay_seconds"; then
		echo 'Retry wrapper: interrupted while waiting to retry' >&2
		exit 1
	fi
	attempt=$((attempt + 1))
	delay_seconds=$((delay_seconds * 2))
	if [ "$delay_seconds" -gt "$max_delay_seconds" ]; then
		delay_seconds=$max_delay_seconds
	fi
done

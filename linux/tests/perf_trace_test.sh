#!/bin/sh
set -eu

trace_file=${1:?usage: perf_trace_test.sh TRACE.csv}
if [ ! -s "$trace_file" ]; then
	echo "performance trace test: trace file is missing or empty: $trace_file" >&2
	exit 1
fi

awk -F, '
NR == 1 {
	if ($0 != "monotonic_us,metric,duration_us,execution,thread_id,work_class,value_a,value_b,value_c,value_d,value_e,value_f,detail") {
		print "performance trace test: unexpected CSV header" > "/dev/stderr"
		exit 1
	}
	next
}
{
	if (NF != 13 || $5 !~ /^[0-9]+$/) {
		print "performance trace test: malformed CSV row or missing thread identity" > "/dev/stderr"
		exit 1
	}
	if ($2 == "source_read" && $4 == "worker_thread" &&
		$6 == "nearest_navigation_neighbor") nearest_read = 1
	if ($2 == "source_read" && $4 == "worker_thread" &&
		$6 == "distant_speculation") distant_read = 1
	if ($2 == "source_read" && $3 > 0 && $4 == "worker_thread" &&
		$6 == "focused_preview" && $7 > 0 && $8 > 0 && $13 == "\"stb\"") stb_direct_read = 1
	if ($2 == "source_read" && $3 > 0 && $4 == "worker_thread" &&
		$6 == "focused_preview" && $7 > 0 && $8 > 0 && $13 == "\"giflib\"") giflib_direct_read = 1
	if ($2 == "processing" && $4 == "worker_thread" &&
		$6 == "active_image_spread") worker_processing = 1
	if ($2 == "resampling" && $4 == "worker_thread" &&
		$6 == "active_image_spread") worker_resampling = 1
	if ($2 == "resampling" && $4 == "worker_thread" &&
		$6 == "visible_thumbnail") visible_thumbnail_resampling = 1
	if ($2 == "resampling" && $4 == "worker_thread" &&
		$6 == "distant_speculation") distant_thumbnail_resampling = 1
	if ($2 == "decode" && $4 == "worker_thread" &&
		$6 == "focused_preview") preview_decode = 1
	if ($2 == "cancellation" && $4 == "event_thread" &&
		$6 == "focused_preview" && $8 == 1 &&
		$13 == "\"file_dialog_preview\"") preview_pending_cancellation = 1
	if ($2 == "cancellation" && $4 == "worker_thread" &&
		$6 == "focused_preview" && $8 == 3 &&
		$13 == "\"file_dialog_preview\"") preview_stale_cancellation = 1
	if ($2 == "cancellation" && $4 == "event_thread" &&
		$6 == "active_image_spread") active_cancellation = 1
	if ($2 == "cache_snapshot" && $7 > 0 && $10 > 0 &&
		$13 == "\"upload_overlap\"") upload_overlap = 1
	if ($2 == "cache_snapshot" && $7 > 0 && $10 > 0 &&
		$13 == "\"active_working_charge\"") active_working_charge = 1
}
END {
	if (!nearest_read || !distant_read || !stb_direct_read || !giflib_direct_read || !worker_processing ||
		!worker_resampling || !active_cancellation || !preview_decode ||
		!visible_thumbnail_resampling || !distant_thumbnail_resampling ||
		!preview_pending_cancellation || !preview_stale_cancellation ||
		!upload_overlap || !active_working_charge) {
		print "performance trace test: missing codec, cancellation, processing, resampling, thumbnail, focused-preview, upload-overlap, or active-working rows" > "/dev/stderr"
		exit 1
	}
}
' "$trace_file"

echo "performance trace test: codec reads, source attribution, cancellation, processing, resampling, thumbnails, focused preview, upload overlap, and active working data passed"

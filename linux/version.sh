#!/bin/sh
set -eu

numeric_identifier='(0|[1-9][0-9]*)'
prerelease_identifier="(${numeric_identifier}|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
build_identifier='[0-9A-Za-z-]+'
semver_pattern="^${numeric_identifier}\\.${numeric_identifier}\\.${numeric_identifier}(-${prerelease_identifier}(\\.${prerelease_identifier})*)?(\\+${build_identifier}(\\.${build_identifier})*)?$"

is_semver() {
	printf '%s\n' "$1" | grep -Eq "$semver_pattern"
}

append_metadata() {
	case "$1" in
		*+*) printf '%s.%s\n' "$1" "$2" ;;
		*) printf '%s+%s\n' "$1" "$2" ;;
	esac
}

head_commit=$(git rev-parse --verify HEAD 2>/dev/null || true)
if [ -z "$head_commit" ]; then
	printf '%s\n' '0.0.0+unknown'
	exit 0
fi

best_version=
best_distance=
tag_list=$(git tag --merged "$head_commit" --sort=-version:refname 2>/dev/null || true)
while IFS= read -r tag; do
	[ -n "$tag" ] || continue

	tag_version=$tag
	case "$tag_version" in v*) tag_version=${tag_version#v} ;; esac
	is_semver "$tag_version" || continue

	tag_commit=$(git rev-parse --verify "${tag}^{commit}" 2>/dev/null || true)
	[ -n "$tag_commit" ] || continue
	distance=$(git rev-list --count "${tag_commit}..${head_commit}" 2>/dev/null || true)
	case "$distance" in ''|*[!0-9]*) continue ;; esac

	if [ -z "$best_distance" ] || [ "$distance" -lt "$best_distance" ]; then
		best_version=$tag_version
		best_distance=$distance
	fi
done <<EOF
$tag_list
EOF

if [ -n "$best_version" ]; then
	version=$best_version
	if [ "$best_distance" -gt 0 ]; then
		version=$(append_metadata "$version" "dev${best_distance}")
	fi
else
	short_commit=$(git rev-parse --short "$head_commit")
	version="0.0.0+dev.g${short_commit}"
fi

if [ -n "$(git status --porcelain --untracked-files=normal 2>/dev/null || true)" ]; then
	version=$(append_metadata "$version" dirty)
fi

printf '%s\n' "$version"

#!/bin/sh
set -eu

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

MOCK_BIN="$TEMP_DIR/bin"
mkdir -p "$MOCK_BIN"
MAKE_LOG="$TEMP_DIR/make-arguments"
cat > "$MOCK_BIN/make" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$DOCKER_BUILD_TEST_MAKE_LOG"
EOF
chmod 755 "$MOCK_BIN/make"

run_binary_and_expect_version() {
	source_dir=$1
	expected_version=$2
	environment_version=$3
	shift 3
	rm -f "$MAKE_LOG"
	JPEGVIEW_SOURCE_DIR="$source_dir" \
	OUTPUT_DIR="$TEMP_DIR/output" \
	JPEGVIEW_VERSION="$environment_version" \
	DOCKER_BUILD_TEST_MAKE_LOG="$MAKE_LOG" \
	PATH="$MOCK_BIN:$PATH" \
		sh "$REPO_DIR/linux/docker-build.sh" binary "$@" >/dev/null
	actual_version=$(sed -n 's/^VERSION=//p' "$MAKE_LOG")
	if [ "$actual_version" != "$expected_version" ]; then
		echo "Expected Docker build version '$expected_version', got '$actual_version'" >&2
		exit 1
	fi
}

automatic_version=$(cd "$REPO_DIR" && GIT_OPTIONAL_LOCKS=0 sh ./linux/version.sh)
run_binary_and_expect_version "$REPO_DIR" "$automatic_version" ''
run_binary_and_expect_version "$REPO_DIR" environment-version environment-version
run_binary_and_expect_version "$REPO_DIR" argument-version environment-version argument-version

SOURCE_SNAPSHOT="$TEMP_DIR/source-snapshot"
mkdir -p "$SOURCE_SNAPSHOT/linux"
cp "$REPO_DIR/linux/version.sh" "$SOURCE_SNAPSHOT/linux/version.sh"
run_binary_and_expect_version "$SOURCE_SNAPSHOT" 0.0.0+unknown ''

CONFIG_SNAPSHOT="$TEMP_DIR/config-snapshot"
mkdir -p "$CONFIG_SNAPSHOT/linux"
cat > "$CONFIG_SNAPSHOT/linux/version.sh" <<'EOF'
#!/bin/sh
if [ "${GIT_OPTIONAL_LOCKS:-}" = 0 ] &&
	[ "${GIT_CONFIG_COUNT:-}" = 1 ] &&
	[ "${GIT_CONFIG_KEY_0:-}" = safe.directory ] &&
	[ "${GIT_CONFIG_VALUE_0:-}" = "$PWD" ]; then
	printf '%s\n' process-scoped-safe-directory
else
	exit 1
fi
EOF
run_binary_and_expect_version "$CONFIG_SNAPSHOT" process-scoped-safe-directory ''

echo 'Docker build version tests passed.'

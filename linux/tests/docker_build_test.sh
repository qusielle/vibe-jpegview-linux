#!/bin/sh
set -eu

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

MOCK_BIN="$TEMP_DIR/bin"
TEST_HOME="$TEMP_DIR/home"
mkdir -p "$MOCK_BIN" "$TEST_HOME"
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
	HOME="$TEST_HOME" \
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
mkdir -p "$CONFIG_SNAPSHOT/linux" "$CONFIG_SNAPSHOT/.git"
cat > "$CONFIG_SNAPSHOT/linux/version.sh" <<'EOF'
#!/bin/sh
if [ "${GIT_OPTIONAL_LOCKS:-}" = 0 ]; then
	printf '%s\n' container-safe-directory
else
	exit 1
fi
EOF
run_binary_and_expect_version "$CONFIG_SNAPSHOT" container-safe-directory ''
safe_directories=$(HOME="$TEST_HOME" git config --global --get-all safe.directory)
if ! printf '%s\n' "$safe_directories" | grep -Fqx "$CONFIG_SNAPSHOT"; then
	echo 'Docker build wrapper did not add the mounted source to the container Git safe-directory list' >&2
	exit 1
fi

if [ "$(id -u)" -eq 0 ] && command -v chown >/dev/null 2>&1; then
	FOREIGN_SOURCE="$TEMP_DIR/foreign-owner-source"
	mkdir -p "$FOREIGN_SOURCE/linux"
	cp "$REPO_DIR/linux/version.sh" "$FOREIGN_SOURCE/linux/version.sh"
	git -C "$FOREIGN_SOURCE" init -q
	git -C "$FOREIGN_SOURCE" config user.name 'Docker Version Test'
	git -C "$FOREIGN_SOURCE" config user.email 'docker-version-test@example.invalid'
	git -C "$FOREIGN_SOURCE" add linux/version.sh
	git -C "$FOREIGN_SOURCE" commit -qm initial
	git -C "$FOREIGN_SOURCE" tag -a 1.2.3 -m 'Docker version test tag'
	chown -R 1000:1000 "$FOREIGN_SOURCE/.git"
	if HOME="$TEST_HOME" git -C "$FOREIGN_SOURCE" rev-parse --verify HEAD >/dev/null 2>&1; then
		echo 'Git ownership regression test setup unexpectedly allowed untrusted metadata' >&2
		exit 1
	fi
	run_binary_and_expect_version "$FOREIGN_SOURCE" 1.2.3 ''
fi

echo 'Docker build version tests passed.'

#!/bin/sh
# Prepare a fresh Ubuntu-based Codex/container environment for JPEGView Linux.
# This script is safe to rerun. It changes only package state and the user's
# global Git configuration; build outputs remain under linux/build or out.

set -eu

CDPATH=
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if command -v apt-get >/dev/null 2>&1 && [ "${SET_ENV_SKIP_APT:-0}" != "1" ]; then
	apt_run()
	{
		if [ "$(id -u)" -eq 0 ]; then
			apt-get "$@"
		elif command -v sudo >/dev/null 2>&1; then
			sudo apt-get "$@"
		else
			echo "set_env.sh: apt-get needs root or sudo" >&2
			exit 1
		fi
	}

	echo "Updating package lists..."
	apt_run update

	# Ubuntu 20.04 provides libwebp6; newer Ubuntu releases provide libwebp7.
	WEBP_PACKAGE=libwebp6
	if ! apt-cache show "$WEBP_PACKAGE" >/dev/null 2>&1; then
		WEBP_PACKAGE=libwebp7
	fi
	CODEC_PACKAGES=
	for package in \
		libgif-dev libtiff-dev libwebp-dev libheif-dev libraw-dev libjxr-dev libjxr-tools \
		libavif-dev libjxl-dev liblcms2-dev libheif-plugin-libde265 libheif-plugin-x265 libheif-examples; do
		if apt-cache show "$package" >/dev/null 2>&1; then
			CODEC_PACKAGES="$CODEC_PACKAGES $package"
		fi
	done

	echo "Installing Linux build, packaging, and headless-X11 test tools..."
	apt_run install -y --no-install-recommends \
		ca-certificates \
		curl \
		file \
		g++ \
		git \
		imagemagick \
		libjpeg-dev \
		libjpeg-turbo-progs \
		libpng-dev \
		libsdl2-2.0-0 \
		make \
		patchelf \
		pkg-config \
		squashfs-tools \
		wl-clipboard \
		xclip \
		x11-apps \
		x11-utils \
		xdotool \
		xvfb \
		"$WEBP_PACKAGE" \
		$CODEC_PACKAGES
elif [ "${SET_ENV_SKIP_APT:-0}" = "1" ]; then
	echo "Skipping apt installation because SET_ENV_SKIP_APT=1"
else
	echo "set_env.sh: apt-get is not available; skipping system package setup" >&2
fi

# These defaults make atomic commits possible in a fresh container. Override
# them for a different identity, for example:
#   GIT_USER_NAME='Your Name' GIT_USER_EMAIL='you@example.com' ./set_env.sh
GIT_USER_NAME=${GIT_USER_NAME:-Codex}
GIT_USER_EMAIL=${GIT_USER_EMAIL:-codex@localhost}
git config --global user.name "$GIT_USER_NAME"
git config --global user.email "$GIT_USER_EMAIL"

# Containers often mount the repository with a different numeric owner.
if ! git config --global --get-all safe.directory 2>/dev/null | grep -Fxq "$SCRIPT_DIR"; then
	git config --global --add safe.directory "$SCRIPT_DIR"
fi

mkdir -p "$SCRIPT_DIR/out"

cat <<EOF

JPEGView Linux environment is ready.
Repository: $SCRIPT_DIR
Git identity: $GIT_USER_NAME <$GIT_USER_EMAIL>

Useful commands:
  make -C "$SCRIPT_DIR/linux" clean && make -C "$SCRIPT_DIR/linux"
  "$SCRIPT_DIR/linux/build/jpegview-linux" --help
  "$SCRIPT_DIR/linux/build/jpegview-linux" --decode-check /path/to/image-or-folder
  # Decode every supported file in a fixture directory, including animation frame counts:
  "$SCRIPT_DIR/linux/build/jpegview-linux" --decode-check /path/to/fixture-directory
  sh -n "$SCRIPT_DIR/linux/AppRun" "$SCRIPT_DIR/linux/package-appimage.sh" "$SCRIPT_DIR/linux/docker-build.sh"

Ubuntu 20.04 Docker build (Docker must be available to the host/container):
  cd "$SCRIPT_DIR"
  docker build -f linux/Dockerfile.ubuntu20 -t jpegview-linux-build .
  docker run --rm -v "\$PWD/out:/out" jpegview-linux-build appimage
  docker run --rm -v "\$PWD/out:/out" jpegview-linux-build binary

Local AppImage packaging when appimagetool is installed:
  make -C "$SCRIPT_DIR/linux" appimage VERSION=1.3.46-linux.1

Headless X11 smoke test:
  Xvfb :99 -screen 0 1280x800x24 >/tmp/jpegview-xvfb.log 2>&1 &
  DISPLAY=:99 "$SCRIPT_DIR/linux/build/jpegview-linux" /path/to/image.jpg
EOF

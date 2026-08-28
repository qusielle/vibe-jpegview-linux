# JPEGView Linux frontend

This directory contains the first native Linux deliverable for upstream JPEGView v1.3.46.
The original Windows/ATL/WTL project remains unchanged under `src/`.

## Build

The only runtime framework dependency is SDL2. Development headers are not required because
the frontend uses the small SDL2 ABI declared in `src/sdl_abi.h`.

On Ubuntu 20.04, install the compiler, make, and SDL2 runtime first:

```sh
sudo apt install g++ make libsdl2-2.0-0
```

```sh
make -C linux
linux/build/jpegview-linux image.jpg
linux/build/jpegview-linux /path/to/photos
```

The default link statically includes libstdc++ and libgcc. Set `STATIC_RUNTIME=` if a local
toolchain does not provide those static runtime archives.

## Isolated Docker build

The Ubuntu 20.04 Dockerfile contains the compiler, SDL2/WebP runtime, and AppImage tool. The
host only needs Docker; build outputs are written to a host `out/` directory:

```sh
mkdir -p out
docker build -f linux/Dockerfile.ubuntu20 -t jpegview-linux-build .
docker run --rm -v "$PWD/out:/out" jpegview-linux-build appimage
```

This creates `out/JPEGView-Linux-1.3.46-linux.1-x86_64.AppImage`. To export only the binary,
use `jpegview-linux-build binary`; to select another release label, pass it as the second
argument. An optional `--build-arg APPIMAGETOOL_SHA256=...` pins the downloaded AppImage tool.

If SDL2 is installed in a non-standard location, override the linker settings:

```sh
make -C linux SDL2_LIBS='-L/path/to/lib -lSDL2'
```

Supported formats are JPEG, PNG, GIF, BMP, TGA, PSD, PNM-family files, and WebP. Image decoding
is provided by the vendored public-domain/MIT `stb_image` single-header library; WebP is loaded
through the system or bundled `libwebp` at runtime.

## AppImage

The packaging script creates an AppDir, bundles the SDL2 shared library, and invokes
`appimagetool` when it is available:

```sh
make -C linux appimage VERSION=1.3.46-linux.1
```

The resulting AppImage still relies on the host kernel, glibc-compatible userspace, and a
working X11 or Wayland display server. Codec and SDL dependencies are carried with the artifact;
Ubuntu 20.04 runtime compatibility still needs to be verified by running this image in the
intended Docker environment.

## Controls

Arrow keys or Space navigate; mouse wheel zooms around the cursor; left-drag pans; dropped files
open in the viewer; `0` fits the image; `1` shows it at actual size; `F` toggles fullscreen;
`R` reloads; `Esc` or `Q` quits.

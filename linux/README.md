# JPEGView Linux frontend

This directory contains the first native Linux deliverable for upstream JPEGView v1.3.46.
The original Windows/ATL/WTL project remains unchanged under `src/`.

## Build

The only runtime framework dependency is SDL2. Development headers are not required because
the frontend uses the small SDL2 ABI declared in `src/sdl_abi.h`.

On Ubuntu 20.04, install the compiler, make, and SDL2 runtime first:

```sh
sudo apt install g++ make libsdl2-2.0-0 libjpeg-dev libpng-dev libjpeg-turbo-progs xclip wl-clipboard
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

Display resizing follows JPEGView's high-quality path: downsampling uses its integrated
best-quality filter with the default sharpening value, and enlargement uses endpoint-preserving
Catmull-Rom bicubic interpolation. The resulting display-size bitmap is cached until the image or
target size changes, so SDL does not have to scale the original texture with nearest-neighbor
sampling on every frame.

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

Right/Left or PageUp/PageDown navigate; Up/Down rotate 90 degrees; mouse wheel zooms around the
cursor; left-drag pans; dropped files open in the viewer. Space toggles fit/actual, Return fits,
and F11 toggles fullscreen (`0`, `F`, and `Q` remain convenience aliases; `1`–`9` start a
slideshow at that interval). F2 toggles the top-left picture information panel; Ctrl+F2 toggles
the filename overlay. Ctrl+O opens the native in-app file browser, Ctrl+R reloads, and Ctrl+N
toggles the panel. Ctrl+C copies the displayed image, Ctrl+X copies it at original size, Ctrl+Shift+C
copies its path, Ctrl+V pastes a PNG image, Ctrl+P sends the processed image to `lp`, and Delete
opens the move-to-trash confirmation. Ctrl+Shift+M/E set the modification date to now/EXIF date;
R/T perform lossless JPEG rotations when bundled `jpegtran` is available. Move the pointer to
show the navigation panel, whose buttons mirror the core controls from JPEGView's Windows
navigation panel (first/previous/next/last, fit/actual, and fullscreen). The checked panel
remains visible until Ctrl+N disables it; it is temporarily suppressed while a modal menu or
file browser is open. Right-click
opens the core JPEGView context menu; it also supports keyboard selection with the arrow keys and
Return. Hovering over a lower navigation-panel button displays its Windows-style action hint. The selected fit/fill/actual-size or manual zoom mode is retained when navigating to the
next or previous image and is saved between application runs, as is the last maximized or
normal window mode. These settings are stored in
`${XDG_CONFIG_HOME:-$HOME/.config}/jpegview-linux/settings.conf`. Esc stops an active slideshow first,
matching the Windows default escape command, and otherwise quits.

The port follows the Windows `CFileList` navigation model: the default display order is ascending
last-modification time; `N`, `M`, `C`, and `Z` select filename, modification date, creation date,
and random order. `F7` loops the current folder, `F8` traverses non-empty subfolders, and `F9`
traverses sibling folders. The context menu exposes the same navigation and display-order commands.
Previous-folder history is retained when recursive or sibling navigation enters another directory.

The context menu is a native rendering of the Windows `PopupMenu` resource, including its navigation,
sorting, slideshow/movie, transform, zoom, auto-zoom, settings, and administration sections. The
portable commands include folder opening, printing through `lp`, modification-date updates, GNOME/
`feh`/`nitrogen` wallpaper integration, text/image clipboard copy and paste, filename and EXIF
overlays, slideshow transitions, window mode toggles, and `jpegtran`-backed lossless JPEG transforms.
The AppImage bundles `xclip`, `wl-copy`/`wl-paste`, and `jpegtran` when the build environment provides
them. `lp`, `gsettings`, `feh`, and `nitrogen` remain host desktop integrations. The clipboard tools
are also needed for image copy/paste in a local non-AppImage build.

The context menu keeps Windows-only operations visible but disabled where their underlying Windows
subsystem has no Linux implementation yet: batch rename/copy dialogs, free rotation, perspective
correction, the original JPEGView image-processing parameter engine, parameter databases, settings
editors, Open-With management, default-viewer registration, and user-command configuration. This
makes the remaining port boundary explicit while preserving the original command vocabulary.

`Ctrl+S` opens the native save dialog for a full-size processed image; `Ctrl+Shift+S` saves the
displayed screen-size result. JPEG, PNG, BMP, TGA, and WebP output are supported, with the default
filename following Windows JPEGView's `<name>_proc.jpg` convention. Existing files require a second
Enter to confirm replacement. The source file itself remains unchanged unless a lossless JPEG
transform or confirmed delete is explicitly selected.

The Linux command dispatcher uses the original numeric `IDM_*` values from
`src/JPEGView/resource.h`, and the supported keyboard bindings follow the corresponding entries
in `src/JPEGView/Config/KeyMap.txt.default`. This keeps the portable SDL input layer translating
into the same command vocabulary as the Windows `CMainDlg::ExecuteCommand` path. The context menu
is currently a native Linux rendering of the complete Windows `PopupMenu` resource; unsupported
Windows-only commands are shown disabled rather than being silently ignored.

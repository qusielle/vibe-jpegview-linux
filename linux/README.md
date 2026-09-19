# JPEGView Linux frontend

This directory contains the first native Linux deliverable for upstream JPEGView v1.3.46.
The original Windows/ATL/WTL project remains unchanged under `src/`.

## Build

The only runtime framework dependency is SDL2. SDL2 development headers are not required because
the frontend uses the small ABI declared in `src/sdl_abi.h`; codec development packages are still
needed when compiling the optional format support.

On Ubuntu 20.04, install the compiler, make, and SDL2 runtime first:

```sh
sudo apt install g++ make libsdl2-2.0-0 libjpeg-dev libpng-dev libjpeg-turbo-progs xclip wl-clipboard
```

```sh
make -C linux -j"$(nproc)"
linux/build/jpegview-linux image.jpg
linux/build/jpegview-linux /path/to/photos
```

The default link statically includes libstdc++ and libgcc. Set `STATIC_RUNTIME=` if a local
toolchain does not provide those static runtime archives.

## Isolated Docker build

The Ubuntu 20.04 Dockerfile contains the compiler, SDL2 and all optional codec development
libraries. It builds JPEG XL and AVIF from pinned sources because those development packages are
not present in the Ubuntu 20.04 archive. The host only needs Docker; build outputs are written to
a host `out/` directory:

```sh
mkdir -p out
DOCKER_BUILDKIT=1 docker build -f linux/Dockerfile.ubuntu20 -t jpegview-linux-build .
docker run --rm -v "$PWD/out:/out" jpegview-linux-build appimage
```

This creates `out/JPEGView-Linux-1.3.46-linux.1-x86_64.AppImage`. To export only the binary,
use `jpegview-linux-build binary`; to select another release label, pass it as the second
argument. The Dockerfile builds the Highway/JPEG XL and AOM/AVIF dependency chains in parallel
with BuildKit. An optional `--build-arg APPIMAGETOOL_SHA256=...` pins the downloaded AppImage tool.

If SDL2 is installed in a non-standard location, override the linker settings:

```sh
make -C linux SDL2_LIBS='-L/path/to/lib -lSDL2'
```

Supported input formats are JPEG, PNG/APNG (including animation), GIF (including animation), BMP, TGA, PSD, PNM-family files,
QOI, WebP (including animation), TIFF, HEIF/HEIC, AVIF, JPEG XL (including animation), JPEG XR/WDP/HDP, and LibRaw camera
formats such as CR3, CR2, NEF, DNG, ARW, RAF, and RW2. The save dialog can write JPEG, PNG, BMP, TGA, WebP, GIF, TIFF,
PSD, PNM, QOI, HEIF/HEIC, AVIF, and JPEG XL still images; RAW and JPEG XR are decode-only, and animated input is view-only.
Common single-frame formats use the vendored public-domain/MIT `stb_image`
single-header library, while the additional formats use their native codec libraries.

Display resizing follows JPEGView's high-quality path: downsampling uses its integrated
best-quality filter with the default sharpening value, and enlargement uses endpoint-preserving
Catmull-Rom bicubic interpolation. The resulting display-size bitmap is cached until the image or
target size changes, so SDL does not have to scale the original texture with nearest-neighbor
sampling on every frame.

Fit-to-screen mode does not enlarge images that are smaller than the available window; those images
remain at their native size and are centered. Larger images are reduced to fit as usual.

The native navigation panel, menus, tooltips, information overlays, and modal dialogs use
semi-transparent backgrounds so the image remains partially visible underneath them.

The default window title follows the Windows-style image title format:
`filename (widthxheight, file size) - JPEGView`.

## AppImage

The packaging script creates an AppDir, bundles the SDL2 shared library, and invokes
`appimagetool` when it is available:

```sh
mkdir -p out
APPIMAGETOOL=/path/to/appimagetool \
APPIMAGETOOL_ARGS=--appimage-extract-and-run \
BUILD_DIR="$PWD/out/build" \
APPDIR="$PWD/out/JPEGView-Linux.AppDir" \
OUTPUT="$PWD/out/JPEGView-Linux-1.3.46-linux.1-x86_64.AppImage" \
make -C linux appimage VERSION=1.3.46-linux.1
```

The resulting AppImage still relies on the host kernel, glibc-compatible userspace, and a
working X11 or Wayland display server. Codec and SDL dependencies are carried with the artifact;
Ubuntu 20.04 runtime compatibility still needs to be verified by running this image in the
intended Docker environment.

The native window and AppImage desktop entry both use the largest frame embedded from the upstream
`src/JPEGView/res/JPEGView.ico`. Packaging exports that same frame as a Linux icon-theme PNG without
requiring an external image-conversion tool.

## Tests

The dependency-light core suite builds and runs with:

```sh
make -C linux test
```

It covers file-list ordering/navigation, sort and settings persistence mappings, the complete
supported keyboard-command mapping, viewport fit/fill/zoom/pan geometry, resize-dialog validation,
content-sized overlay layout, compact/advanced menu filtering and keyboard selection,
decoder and writer round trips across static and animated formats, all PNM variants, malformed
input, batch-copy planning, desktop-application command expansion, and JPEG metadata. The optional
X11 smoke suite covers startup controls,
mouse-wheel navigation versus Ctrl+wheel zoom, held-key repeat, maximize restoration, and
persisted settings:

```sh
make -C linux test-ui
```

The UI suite uses `Xvfb`, `openbox`, `wmctrl`, and `xdotool`; it reports `SKIP` when those tools are
not installed. When ImageMagick's `import` and `compare` are available it also checks the context
menu repaint pixel-for-pixel. `make -C linux check` runs both suites. The Ubuntu Docker build runs
the core suite.

## Controls

Right/Left or PageUp/PageDown navigate; Home/End select the first/last image; mouse wheel up/down
navigates previous/next, while Ctrl+mouse wheel and Ctrl+Up/Down zoom around the pointer or center.
Up/Down rotate 90 degrees. Space toggles fit/actual, Return/0 fits, Ctrl+Return fills with crop,
`+`/`-` zoom, and F11/F toggles fullscreen; F12 spans screens, Ctrl+F11 fits the window to the
image, Shift+F11 hides the title bar, and Shift+F12 toggles always-on-top. `1`–`9` start a
slideshow at that interval. F2 toggles the top-left picture information panel; Shift+N or Ctrl+F2
toggles the filename overlay, while N/M/C/Z select filename, modification-date, creation-date,
or random sorting. Ctrl+O opens the native in-app file browser, Ctrl+R reloads, and Ctrl+N toggles
the panel. Ctrl+C copies the image at original size, Ctrl+Shift+C copies its path, Ctrl+V pastes a
PNG image, Ctrl+P sends the processed image to `lp`, and Delete opens the move-to-trash confirmation.
Ctrl+Shift+M/E set the modification date to now/EXIF date; R/T perform lossless JPEG rotations when
bundled `jpegtran` is available; F5 toggles the ported automatic histogram contrast correction, and
Ctrl+Shift+R opens the image resize dialog. Move the pointer to the lower edge of the window to
show the navigation panel, whose buttons mirror the core controls from JPEGView's Windows
navigation panel (first/previous/next/last, ordering mode, fit/actual, and fullscreen). The
ordering button shows `N` for file-name order and `D` for modification-date order; clicking it
switches between those two modes. By default the panel is hidden until the pointer enters the
lower edge of the window; the context menu can disable this automatic reveal mode. Ctrl+N
disables the panel entirely, and the panel is temporarily suppressed while a modal menu or file
browser is open. Right-click or the keyboard Context Menu key opens the compact core JPEGView
context menu; Show Advanced Options temporarily restores Open image with, Print, batch rename/copy,
date and wallpaper commands, extended navigation and sorting, image transforms and correction,
extra zoom and window controls, slideshow controls, and settings administration (including
disabled Windows-only commands) without saving that choice. The compact menu keeps common
navigation, fit/actual-size, fullscreen, and fit-window-to-image commands available. The menu also
supports keyboard selection with Up/Down and Return; if it spans multiple
columns to fit the window height, Left/Right moves between columns. Hovering over a lower
navigation-panel button displays its Windows-style action hint. The selected fit/fill/actual-size or manual zoom mode is retained when navigating to the
next or previous image and is saved between application runs, as is the last maximized or
normal window mode, the lower navigation panel's show/hide selection, and the F2/Ctrl+F2 overlay
visibility choices. The navigation panel hover preference and current file-order mode/direction are
also saved. These settings are stored in
`${XDG_CONFIG_HOME:-$HOME/.config}/jpegview-linux/settings.conf`. Esc stops an active slideshow first,
matching the Windows default escape command, and otherwise quits.

The port follows the Windows `CFileList` navigation model: the default display order is ascending
file modification time from the filesystem; `N`, `M`, `C`, and `Z` select filename, modification
date, creation date, and random order. `F7` loops the current folder, `F8` traverses non-empty
subfolders, and `F9` traverses sibling folders. The navigation panel and context menu show the
current display-order mode, and the context menu exposes the same navigation and sorting commands.
Previous-folder history is retained when recursive or sibling navigation enters another directory.

The context menu is a native rendering of the Windows `PopupMenu` resource, including its navigation,
sorting, slideshow/movie, transform, zoom, auto-zoom, settings, and administration sections. The
portable commands include folder opening, printing through `lp`, modification-date updates, GNOME/
`feh`/`nitrogen` wallpaper integration, text/image clipboard copy and paste, filename and EXIF
overlays, slideshow transitions, window mode toggles, and `jpegtran`-backed lossless JPEG transforms.
The `Auto correction` command uses the Windows histogram-derived RGB correction LUT and can be toggled
with `F5`; it remains non-destructive until the processed result is explicitly saved.
The `Open image with` submenu is populated from matching freedesktop `.desktop` applications and
launches them with the current image, including standard `%f`/`%F` and URI placeholders. Applications
are discovered from the user and system application directories at menu-open time.
`Batch rename/copy...` is also available: select images, preview a Windows-compatible target pattern,
save it as a template, and rename within the folder or copy into newly-created subdirectories without
overwriting existing files. Its `%pictures%` placeholder maps to `$XDG_PICTURES_DIR` or `$HOME/Pictures`.
`Change size...` is ported from the Windows Resize dialog: percentage, width, and height edits retain
the aspect ratio, and the point, Lanczos/Bicubic, sharpen-low, and sharpen-medium filters are available.
The resize is applied to the processed image in memory and can then be saved with `Ctrl+S`; `Ctrl+Shift+R`
opens the same dialog directly.
The AppImage bundles `xclip`, `wl-copy`/`wl-paste`, and `jpegtran` when the build environment provides
them. `lp`, `gsettings`, `feh`, and `nitrogen` remain host desktop integrations. The clipboard tools
are also needed for image copy/paste in a local non-AppImage build.

Animated GIF, APNG, WebP, AVIF, and JPEG XL images start playing automatically at their embedded frame
delays. The `Movie` menu plays animated or multi-page images at a selected fixed rate (5, 10, 25,
30, 50, or 100 fps), and advances a folder of still images when the current image has no frames.
The original frame loop count is honored when a format provides one. `Alt+R` resumes stopped playback.
`Esc` stops animation, movie, or slideshow playback before it closes the viewer.

The context menu keeps Windows-only operations visible but disabled where their underlying Windows
subsystem has no Linux implementation yet: free rotation, perspective
correction, local density correction, the remaining JPEGView image-processing parameter engine,
parameter databases, settings
editors, Open-With menu management, default-viewer registration, and user-command configuration. This
makes the remaining port boundary explicit while preserving the original command vocabulary.

`Ctrl+S` opens the native save dialog for a full-size processed image; `Ctrl+Shift+S` saves the
displayed screen-size result. The additional output formats listed above are selected by their
filename extension, with the default filename following Windows JPEGView's `<name>_proc.jpg`
convention. Existing files require a second Enter to confirm replacement. The source file itself
remains unchanged unless a lossless JPEG transform or confirmed delete is explicitly selected.

The Linux command dispatcher uses the original numeric `IDM_*` values from
`src/JPEGView/resource.h`, and the supported keyboard bindings follow the corresponding entries
in `src/JPEGView/Config/KeyMap.txt.default`. This keeps the portable SDL input layer translating
into the same command vocabulary as the Windows `CMainDlg::ExecuteCommand` path. The context menu
is currently a native Linux rendering of the complete Windows `PopupMenu` resource; unsupported
Windows-only commands are shown disabled rather than being silently ignored.

## Architecture and future refactoring

Platform-independent behavior is split into small modules under `src/` and exercised by the core
suite; `main.cpp` is the SDL window, rendering, and event-dispatch composition layer. See
[`ARCHITECTURE.md`](ARCHITECTURE.md) for module ownership, the completed Viewer extractions, and the
ordered refactoring backlog.

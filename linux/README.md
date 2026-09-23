# JPEGView Linux frontend

This directory contains the first native Linux deliverable for upstream JPEGView v1.3.46.
The original Windows/ATL/WTL project remains unchanged under `src/`.

## Linux branch changes, in order of importance

This is the complete grouped summary of features and changes made since the native Linux branch
split from the Windows frontend. Later fixes, tests, and refactorings are grouped with the feature
they support.

1. **Native Linux viewer, AppImage, and Ubuntu 24 Debian package.** A native SDL2 frontend now opens individual
   images, multiple command-line inputs, and directories without modifying the original Windows
   application. It includes a resizable/maximizable/fullscreen window, drag-and-drop, a desktop
   entry, the upstream JPEGView application icon, Ubuntu 20.04, 22.04, and 24.04 build containers,
   AppImage packaging with bundled SDL and codec runtimes, and an Ubuntu 24.04 `.deb` package whose
   build and runtime dependencies come from the standard Ubuntu repositories.

2. **Broad native format and color support.** Linux decoding covers JPEG, PNG/APNG, GIF, BMP, TGA,
   PSD, PNM, QOI, WebP, TIFF, HEIF/HEIC, AVIF, JPEG XL, JPEG XR, and LibRaw camera formats. Embedded
   color profiles are transformed through LCMS2. The save dialog writes JPEG, PNG, BMP, TGA, WebP,
   GIF, TIFF, PSD, PNM, QOI, HEIF/HEIC, AVIF, and JPEG XL still images. Codec detection and fixtures
   were made portable across Ubuntu 20.04 and newer distributions, including giflib installations
   without pkg-config metadata and HEIF encoders with different supported profiles.

3. **High-quality viewing, fitting, zooming, and panning.** JPEGView's high-quality downsampling and
   sharpening path was ported, with bicubic enlargement and a shared 1 GiB image-cache budget.
   Fitted JPEGs use libjpeg-turbo's native reduced DCT decode, avoiding full 4000×6000 pixel buffers
   when the screen needs only a smaller image. Up to four hardware-aware, low-priority workers prepare
   the closest forward/backward pairs concurrently; the prefetch window is derived from the configured
   cache size and current viewport,
   so the default 1 GiB budget can cover roughly 128 full-HD neighbors. Large JPEG input is memory-mapped,
   letting native decoding and repeated neighboring access use the kernel page cache without an extra
   stdio copy layer. Prepared frames are then uploaded incrementally and retained as renderer-ready
   textures. The closest next and previous files take
   preparation and upload priority, and an already prepared static image is presented without first
   copying its full decoded pixels on the UI thread; navigation therefore avoids CPU resizing and
   normally avoids texture upload as well. Fit mode
   uses the full client area without artificial top/bottom gaps and does not enlarge small images.
   Fit, fill, actual-size, and manual modes survive navigation appropriately, while temporary zoom
   on one image is reset to the selected fit/actual mode for the next image. Ctrl+wheel zooms around
   the pointer, mouse dragging pans, and repeatable Shift+Arrow commands pan an actual-size image in
   the original 48-pixel steps.

4. **Folder navigation and ordering.** The Windows `CFileList` behavior was ported for first,
   previous, next, and last navigation; multiple inputs; folder looping; recursive subfolders;
   sibling folders; reload; and previous-folder history. Alt+Left/Right jumps directly to the first
   image in the previous/next populated sibling folder, independent of the active navigation mode.
   Ordering supports logical filename,
   filesystem modification date, creation date, file size, and random modes in either direction.
   The active filename/date ordering is visible and switchable from both the navigation panel and
   context menu, and the selected mode is preserved between runs.

5. **Responsive keyboard and mouse navigation.** Left/Right image navigation and menu/browser
   selection repeat while held. The open browser supports repeating Up/Down, PageUp/PageDown, and
   Home/End movement. Repeated image navigation presents progress immediately instead of freezing
   until key release. The plain mouse wheel selects the previous/next file, while holding Ctrl
   retains wheel zoom. The keyboard Context Menu key and the original Windows numeric command IDs
   and corresponding supported default bindings are retained.

6. **Neighboring-image thumbnail panel.** Ctrl+T or the context menu opens a vertical strip on the
   left in active file order. The current image stays centered and fully bright; neighboring images
   are darkened and clickable. The panel reserves image space instead of covering the picture,
   preloads nearest files first, and retains every generated thumbnail for the active file list.
   Completed neighbor display frames feed a very-low-priority thumbnail worker, so nearby thumbnails
   appear during display prefetch without decoding the large source file again. Its divider is
   mouse-resizable,
   its width and visibility persist, and thumbnail row height follows panel width so a narrow panel
   fits more images without large fixed gaps. Thumbnails have no forced horizontal inset and only a
   one-pixel vertical margin plus separator; source-area antialiasing keeps reduced images smooth.

7. **Native navigation panel with automatic reveal.** The lower panel provides first/previous/next/
   last, ordering, fit/actual, rotate, and fullscreen controls with action tooltips. By default it
   appears when the pointer reaches the lower edge, with options to keep it shown or disable it.
   Its compact 32-pixel height, 26-pixel outlined buttons, off-white icons, and yellow hover feedback
   follow the original Windows panel style. Navigation, fit/actual-size, window, and rotation glyphs
   use the original Windows geometry and show the action that clicking will perform. Rendering is
   clipped to the panel bounds, and its visibility and hover preference persist.

8. **Complete adaptive context menu.** The Linux-rendered menu uses the Windows `PopupMenu` command
   vocabulary and shows shortcuts and checked states. Its compact view keeps common actions visible;
   one-off **Show Advanced Options** reveals navigation, ordering, transforms, correction, extended
   zoom/window/auto-zoom, slideshow, Open With, print, batch, date, wallpaper, settings, and disabled
   Windows-only administration entries without persisting the expanded state. Long menus split into
   columns, support Left/Right column movement and repeating Up/Down movement, remain inside the
   window when expanded, open at the current pointer, and can be opened from the keyboard menu key.

9. **Portable file and desktop operations.** The branch adds a native open/save browser, processed
   full-size and screen-size saving with overwrite confirmation, live case-insensitive filename
   filtering, name/newest-modification-date listing order, Ctrl+Return direct folder opening, and
   non-blocking direct image/subdirectory counts for folder rows, plus a focused-item preview that
   shows the selected image or the first image in a selected folder. The dialog can be resized from
   its lower-right corner, its preview width can be adjusted by dragging the list/preview divider,
   and the mouse wheel scrolls an overflowing file list. Dialog dimensions and the preview/list
   proportion are preserved between runs. The preview image is resampled to the pane's usable area
   after resizing, using the thumbnail panel's source-area antialiasing. Preview decoding runs in
   the background; its temporary pixels stay outside the viewer caches. It also provides move-to-trash
   confirmation, original-size image copy on Ctrl+C, path copy, PNG paste, printing through `lp`,
   modification-date updates from now or EXIF, wallpaper integration, folder exploration, and
   lossless JPEG rotation through `jpegtran`. The **Open image with** submenu discovers freedesktop
   `.desktop` applications
   and expands their file/URI placeholders.

10. **Batch rename/copy and image resizing.** The batch dialog supports image selection, previews,
    saved Windows-compatible naming patterns, safe same-folder renames, and copying into newly
    created directories without overwrites. The resize dialog preserves aspect ratio across percent,
    width, and height fields and provides point, Lanczos/Bicubic, sharpen-low, and sharpen-medium
    filters. Both areas were separated into independently tested planning/model modules.

11. **Image processing and animation.** The Windows picture-level panel is ported: contrast,
    brightness/gamma, saturation, three color-balance axes, local shadow/highlight correction,
    correction strengths, and sharpening are editable with live preview. The separate unsharp-mask
    dialog previews radius, amount, and threshold before applying. Adjustments are non-destructive
    until save; per-image levels can be saved/removed in the parameter database, set as defaults for
    images without a saved entry, or kept between images. Automatic histogram correction remains
    available with F5. Animated GIF,
    APNG, WebP, AVIF, and JPEG XL honor frame delays and loop counts. Movie mode supports fixed frame
    rates and folder advancement, slideshow transitions are rendered natively, Alt+R resumes, and
    Escape stops active playback before quitting. Decoded pixels, prepared display frames, and
    retained renderer textures share one memory budget with per-layer LRU retention, while a
    low-contention background workers predecode nearby non-JPEG files in both directions. JPEG
    neighbors take the reduced-resolution display path directly, while full pixels remain lazy.
    Decode completions feed a
    separate display-preparation worker pool, and both decoded and display caches reject stale source
    identities. JPEG metadata parsing stops at the compressed scan instead of reading the full file
    on every navigation. Large evicted CPU buffers are retired on workers rather than destroyed on the event
    thread. Previously viewed and prefetched images
    therefore avoid repeated synchronous decoding, correction, high-quality scaling, and texture
    creation during navigation.

12. **Information overlays and window feedback.** F2 picture information and Shift+N/Ctrl+F2 filename
    overlays use compact translucent surfaces sized to their content with small comfortable margins.
    Filename, EXIF, and counter text remain responsive during navigation. The information popup uses
    a readable `W X H, Size` line and an unlabeled modification date. The EXIF popup includes a
    toggleable grayscale histogram, hidden by default. Overlay visibility persists
    immediately. The window title shows filename, dimensions, and size. Menus, dialogs, tooltips,
    and panels use the hinted 9-point Terminus bitmap when the complete string is printable ASCII,
    preserving lowercase letters as drawn and using crisp one-bit pixels without antialiased edges. Strings
    containing other characters use the desktop's configured UI font through Pango, retaining Unicode
    shaping and automatic installed-font fallback; translucent surfaces provide a consistent visual
    treatment.

13. **Reliable startup and saved session state.** Scale mode, default picture levels, ordering
    mode/direction, maximized or normal state, navigation-panel choices,
    filename/EXIF/histogram visibility, automatic correction,
    batch pattern, thumbnail visibility/width, open-dialog dimensions and preview proportion, and the
    image-cache budget are stored under XDG configuration paths. A previously
    maximized window is created maximized before it is shown, avoiding the visible delayed maximize.
    The real viewer window is painted and shown before the initial directory scan and image decode,
    so cold AppImage and large-folder startup provides immediate visual feedback without changing the
    image preparation or navigation path. Compatibility handling keeps always-on-top optional on older
    SDL runtimes.

14. **Rendering and metadata correctness fixes.** Context-menu close no longer leaves a white pixel
    over the image or revealed navigation panel; borders avoid endpoint rasterization artifacts;
    overlays no longer retain unnecessary minimum widths; large images fit edge-to-edge; and signed
    EXIF rational values are parsed correctly. Context menus and modal panels trigger clean redraws
    and no longer damage underlying image pixels.

15. **Regression tests, modularization, and faster builds.** A dependency-light core suite and X11
    UI smoke suite now cover codecs, mutable image transforms/resampling, file ordering, browser
    state, settings, keyboard mappings, viewport geometry, overlays, context-menu columns and
    repainting, thumbnail layout/persistence, resize and batch models, application discovery,
    metadata, startup maximization, and held-key behavior. Viewer logic was extracted into focused
    modules for image pixels, settings, sorting, input commands, viewport, open-dialog state,
    overlays, thumbnails, fonts, image information, context menus, resize, batch operations, and
    desktop applications. Shell and sanitizer targets supplement the regular suites. Docker builds
    use distro codec packages where available and build only codecs missing from that Ubuntu
    release; independent codec stages and the viewer/tests compile in parallel.

## Build

The runtime framework dependencies are SDL2 and Pango/FreeType. SDL2 development headers are not
required because the frontend uses the small ABI declared in `src/sdl_abi.h`; Pango development
headers and codec development packages are needed at compile time. The font stack is loaded only
when text outside the embedded printable-ASCII bitmap is used, so normal startup and ASCII UI do not
pay its initialization cost. The AppImage bundles these libraries while continuing to discover the
user's system fonts through Fontconfig.

On Ubuntu 20.04, install the compiler, make, and SDL2 runtime first:

```sh
sudo apt install g++ make libsdl2-2.0-0 libpango1.0-dev libfontconfig1-dev \
  libjpeg-dev libpng-dev libjpeg-turbo-progs xclip wl-clipboard
```

```sh
make -C linux -j"$(nproc)"
linux/build/jpegview-linux image.jpg
linux/build/jpegview-linux /path/to/photos
```

The default link statically includes libstdc++ and libgcc. Set `STATIC_RUNTIME=` if a local
toolchain does not provide those static runtime archives.

## Isolated Docker build

The Ubuntu 20.04, 22.04, and 24.04 Dockerfiles contain the compiler, SDL2, and optional codec
development libraries for their respective releases. Ubuntu 20.04 builds JPEG XL and AVIF from
pinned sources; Ubuntu 22.04 uses its AVIF package and builds JPEG XL from source; Ubuntu 24.04
uses distro codec packages and explicitly installs libheif's HEVC decoder/encoder plugins because
its image omits recommended packages. The host only needs Docker; build outputs are written to a host
`out/` directory:

```sh
mkdir -p out
DOCKER_BUILDKIT=1 docker build -f linux/Dockerfile.ubuntu20 -t jpegview-linux-build:ubuntu20 .
DOCKER_BUILDKIT=1 docker build -f linux/Dockerfile.ubuntu22 -t jpegview-linux-build:ubuntu22 .
DOCKER_BUILDKIT=1 docker build -f linux/Dockerfile.ubuntu24 -t jpegview-linux-build:ubuntu24 .
docker run --rm -v "$PWD/out:/out" jpegview-linux-build:ubuntu20 appimage
```

This creates `out/JPEGView-Linux-1.3.46-linux.1-x86_64.AppImage`. To export only the binary,
run `docker run --rm -v "$PWD/out:/out" jpegview-linux-build:ubuntu20 binary`; substitute the
Ubuntu 22.04 or 24.04 image tag to use another build environment. To select another release label,
pass it as the second argument. The Ubuntu 20.04 Dockerfile builds its Highway/JPEG XL and AOM/AVIF
dependency chains in parallel with BuildKit. Ubuntu 22.04 builds Highway/JPEG XL; Ubuntu 24.04
needs no codec source builds. An optional `--build-arg APPIMAGETOOL_SHA256=...` pins the downloaded
AppImage tool. Build the release artifact with the oldest supported base (Ubuntu 20.04) when it
must also run on later Ubuntu releases; newer-base artifacts can require newer system glibc.

The Debian package Dockerfile uses only the standard Ubuntu 24.04 repositories for build tools and
runtime libraries. Ubuntu 20.04 and 22.04 do not produce a `.deb`.

```sh
DOCKER_BUILDKIT=1 docker build -f linux/Dockerfile.deb.ubuntu24 -t jpegview-linux-deb-build:ubuntu24 .
docker run --rm -v "$PWD/out:/out" jpegview-linux-deb-build:ubuntu24 deb 1.3.46-linux.1 24
```

This creates `jpegview-linux_1.3.46-linux.1_ubuntu24_amd64.deb` in `out/`. Install it with
`sudo apt install ./out/jpegview-linux_1.3.46-linux.1_ubuntu24_amd64.deb`; APT resolves its
shared-library dependencies from Ubuntu 24.04.

GitHub Actions builds and tests all three AppImage Dockerfiles on branch pushes and pull requests.
Each successful Ubuntu build job uploads its x86_64 AppImage, native executable, and a `SHA256SUMS`
file as a downloadable workflow artifact named `jpegview-linux-ubuntu20-x86_64`,
`jpegview-linux-ubuntu22-x86_64`, or `jpegview-linux-ubuntu24-x86_64`. The Ubuntu 24 artifact also
includes its `.deb` package. These workflow artifacts are retained for 14 days and are
available from the workflow run's summary. When a GitHub Release is published, its workflow uploads
versioned AppImage and native executable assets for each Ubuntu base. Asset names include the Ubuntu
release because artifacts built on newer bases may require newer system glibc.

If SDL2 is installed in a non-standard location, override the linker settings:

```sh
make -C linux SDL2_LIBS='-L/path/to/lib -lSDL2'
```

Supported input formats are JPEG, PNG/APNG (including animation), GIF (including animation), BMP, TGA, PSD, PNM-family files,
QOI, WebP (including animation), TIFF, HEIF/HEIC, AVIF, JPEG XL (including animation), JPEG XR/WDP/HDP, and LibRaw camera
formats such as CR3, CR2, NEF, DNG, ARW, RAF, and RW2. The save dialog can write JPEG, PNG, BMP, TGA, WebP, GIF, TIFF,
PSD, PNM, QOI, HEIF/HEIC, AVIF, and JPEG XL still images; RAW and JPEG XR are decode-only, and animated input is view-only.
JPEG uses the linked libjpeg implementation (libjpeg-turbo in the supported builds), common
single-frame formats use the vendored public-domain/MIT `stb_image` single-header library, and the
additional formats use their native codec libraries.

Display resizing follows JPEGView's high-quality path: downsampling uses its integrated
best-quality filter with the default sharpening value, and enlargement uses endpoint-preserving
Catmull-Rom bicubic interpolation. JPEG neighbors are first decoded at the smallest native DCT scale
(1/8, 1/4, 1/2, or full size) that still covers the target, then converted to exact display-size
bitmaps on low-priority background workers. This keeps fitted 4000×6000 files out of the
full-resolution path during ordinary navigation; requesting original pixels still performs and
caches a full decode. The SDL thread uploads completed frames incrementally—SDL renderer objects are
thread-confined—and retains the resulting textures under the shared configured LRU budget, keyed by
source identity, animation frame, correction mode, and target size. If preparation misses, SDL can
temporarily scale the source texture while the high-quality result is produced; the expensive CPU
resize never runs in the render loop.

Fit-to-screen mode does not enlarge images that are smaller than the available window; those images
remain at their native size and are centered. Larger images are reduced to fit as usual.

The native navigation panel, menus, tooltips, information overlays, and modal dialogs use
semi-transparent backgrounds so the image remains partially visible underneath them. Printable
ASCII text uses the crisp embedded 9-point Terminus bitmap. The atlas is generated from
`TerminusTTF-4.47.0.ttf` by extracting its embedded 12-pixel monochrome strike at 96 dpi; the
scalable outlines are deliberately not used. The TTF itself is not bundled. Text requiring Unicode
uses the desktop font discovered from XFCE, GTK, xsettingsd, or KDE configuration. Set
`JPEGVIEW_FONT` to a Pango font description such as `Sans 11` to override desktop discovery for that
Unicode fallback.

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

The Unicode font-rendering test requires at least one installed system font. The Ubuntu Docker
build installs `fonts-dejavu-core` for this purpose; this font is not bundled into the AppImage,
which continues to use fonts installed on the user's system.

It covers file-list ordering/navigation, mutable image transforms, all resize filters and automatic
correction invariants, sort and settings persistence mappings, the complete supported
keyboard-command mapping, viewport fit/fill/zoom/pan geometry, open/save browser state, preview
downsampling, resize-dialog validation, content-sized overlay layout, compact/advanced menu filtering and
keyboard selection, thumbnail layout/resampling, shared cache accounting, reduced JPEG display
decoding, and nearest-display upload priority, desktop-font resolution, decoder and writer round
trips across static and animated formats, all PNM variants, malformed input, batch-copy planning,
desktop-application command expansion, and JPEG metadata. The optional X11 smoke suite covers the
open browser's filtering, folder counts, sorting, direct-folder opening, focus restoration, paging,
Home/End, held-key movement, wheel scrolling, and dialog/preview resizing; thumbnail
display/resizing/clicking/persistence; sibling-folder hotkeys; context-menu expansion and repainting; startup controls;
mouse-wheel navigation versus Ctrl+wheel zoom; held image navigation; maximize restoration; and
persisted settings:

```sh
make -C linux test-ui
```

The UI suite uses `Xvfb`, `openbox`, `wmctrl`, and `xdotool`; it reports `SKIP` when those tools are
not installed. When ImageMagick's `import` and `compare` are available it also checks the context
menu repaint pixel-for-pixel. `make -C linux check` runs both suites plus shell syntax checks and
ShellCheck when installed. `make -C linux test-sanitize` rebuilds the core suite with AddressSanitizer
and UndefinedBehaviorSanitizer. Leak detection is disabled for that target because linked desktop
font and optional codec libraries retain process-global caches. The Ubuntu Docker build runs the
core suite.

## Controls

Right/Left or PageUp/PageDown navigate; Home/End select the first/last image; mouse wheel up/down
navigates previous/next, while Ctrl+mouse wheel and Ctrl+Up/Down zoom around the pointer or center.
Up/Down rotate 90 degrees. Space toggles fit/actual, Return/0 fits, Ctrl+Return fills with crop,
`+`/`-` zoom, and F11/F toggles fullscreen; F12 spans screens, Ctrl+F11 fits the window to the
image, Shift+F11 hides the title bar, and Shift+F12 toggles always-on-top. `1`–`9` start a
slideshow at that interval. At actual size, Shift+Arrow pans the image in 48-pixel steps. F2
toggles the top-left picture information panel; Shift+N or Ctrl+F2
toggles the filename overlay, while N/M/C/Z select filename, modification-date, creation-date,
or random sorting. Ctrl+O opens the native in-app file browser; type any part of a name to filter
its files and folders case-insensitively, then press Enter to open the selected match. Ctrl+Return
opens a selected folder immediately at its first compatible image without entering the folder in
the dialog. The sorting control switches the listing between case-insensitive filename order and
newest-first modification-date order. Backspace removes one complete UTF-8 character from the
filter and navigates to the parent folder once the filter is empty. Up/Down move one row,
PageUp/PageDown move one visible page, and Home/End select the first/last row; all six keys repeat
while held. Entering a folder selects its first child rather than the `[..]` parent row; returning
to the parent selects the folder that was just exited. Folder rows show
right-aligned counts of compatible images and subdirectories at their immediate level; these are
calculated in the background. The mouse wheel scrolls the visible file list; drag the dialog's
lower-right corner to resize it, or drag the vertical separator to adjust the preview width. A
preview alongside the list follows the focused file, or the first image in a focused folder using
the current listing order. Ctrl+R reloads, and
Ctrl+N toggles the navigation panel. Ctrl+T toggles a thumbnail strip on the left. Ctrl+C copies the
image at original size, Ctrl+Shift+C copies its path, Ctrl+V pastes a
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
also saved, together with the thumbnail panel visibility. These settings are stored in
`${XDG_CONFIG_HOME:-$HOME/.config}/jpegview-linux/settings.conf`. Esc stops an active slideshow first,
matching the Windows default escape command, and otherwise quits.

The same settings file accepts `cache_size_mb=1024` to control the aggregate memory retained for
decoded images, worker-prepared display frames, and renderer-ready textures. The value is in MiB,
takes effect at the next launch, and defaults to 1024. Set it to `0` to disable retained image/display
caching; this does not disable the separate thumbnail cache, whose generated entries are retained for
the active file list regardless of the large-image budget.

The thumbnail panel is hidden by default and can be enabled from the context menu or with Ctrl+T.
It follows the active file ordering in a vertical strip: the current image remains centered and at
normal brightness, while surrounding images are darkened. Clicking a thumbnail opens that file.
Thumbnails are loaded incrementally in nearest-to-current order and kept for the active file list.
Display-ready neighbor pixels are reused for thumbnail preparation when available; remaining
entries are decoded during idle time. The panel reserves its own space on the left instead of
covering the image. Drag its right
separator to adjust its width; row height follows the width, so narrower panels display more
thumbnails without large fixed vertical gaps. The width and visibility are preserved between runs.

The port follows the Windows `CFileList` navigation model: the default display order is ascending
file modification time from the filesystem; `N`, `M`, `C`, and `Z` select filename, modification
date, creation date, and random order. `F7` loops the current folder, `F8` traverses non-empty
subfolders, and `F9` traverses sibling folders. Alt+Left/Right opens the first image in the previous
or next populated sibling folder without changing the active navigation mode. The navigation panel
and context menu show the current display-order mode, and the context menu exposes the same
navigation and sorting commands, including these sibling-folder jumps.
Previous-folder history is retained when F8/F9 traversal enters another directory. Explicit Alt+Left/Right
jumps discard that history so subsequent Left/Right navigation follows the active mode within the
destination folder.

The context menu is a native rendering of the Windows `PopupMenu` resource, including its navigation,
sorting, slideshow/movie, transform, zoom, auto-zoom, settings, and administration sections. The
portable commands include folder opening, printing through `lp`, modification-date updates, GNOME/
`feh`/`nitrogen` wallpaper integration, text/image clipboard copy and paste, filename and EXIF
overlays, slideshow transitions, window mode toggles, and `jpegtran`-backed lossless JPEG transforms.
The `Auto correction` command uses the Windows histogram-derived RGB correction LUT and can be toggled
with `F5`. `Edit picture levels...` opens the bottom adjustment panel; drag its sliders for live
preview, turn on `Local density` to enable the shadow/highlight controls, and use `Reset` to restore
the neutral slider values. Color/contrast correction strength controls refine automatic correction
and are available while automatic correction is enabled.
The separate `Unsharp mask...` dialog previews radius, amount, and threshold and has Apply/Cancel
actions. `Keep levels` carries current adjustments to the next image and temporarily takes precedence
over saved per-image values. With keep disabled, each saved entry (including its auto-correction and
local-density state) is restored for that image. Save/remove actions are disabled while Keep levels is
on; `Set current parameters as default...` stores slider values and automatic-correction state for
images without a saved entry. Removing an entry restores those defaults. The Linux-native
`picture-levels.db` is stored in
the JPEGView Linux configuration directory (`$XDG_CONFIG_HOME/jpegview-linux`, or
`~/.config/jpegview-linux` when XDG_CONFIG_HOME is unset). All levels remain non-destructive until
the processed image is saved.
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

### Known Windows-parity gaps

The Linux port does not yet match every user-facing Windows feature. The outstanding items identified
by comparing the Linux frontend with the Windows menus and feature panels are:

- **Free rotation and perspective correction.** The quarter-turn/mirror operations are available,
  but the interactive free-rotation and perspective/tilt-correction panels are not implemented.
- **Crop and selection tools.** The Windows selection overlay, crop aspect/fixed-size modes, crop,
  lossless crop, copy-selection, and zoom-selection commands are absent.
- **Zoom navigator.** Linux has a neighboring-file thumbnail strip, but not Windows' miniature
  viewport overlay for panning around an enlarged image.
- **Image comparison shortcuts.** Mark-image/toggle-back and the second processing-parameter set
  exchange workflow are not implemented.
- **Settings administration.** Editing global/user Windows configuration files and updating a user
  configuration from the global template are not available. The Linux frontend uses its own XDG
  settings file and does not translate every Windows setting.
- **Default processing preset.** Saving the current picture-level values as the default for images
  without a per-image entry is available through `Set current parameters as default...`.
- **Open-With management and user commands.** Open-With applications are discovered automatically
  from freedesktop `.desktop` files, but there is no manual menu editor. Windows-style custom user
  command definitions and their invocation menu are also absent.
- **Desktop association management.** Registering JPEGView as the default viewer/file-type handler is
  not implemented; associations remain controlled by the Linux desktop environment.
- **Parameter database administration.** Per-image parameters work in the Linux-native text database,
  but backup/restore actions are absent and the file is not compatible with the Windows binary DB.
- **Print setup and built-in help.** Printing currently delegates to `lp` with desktop defaults rather
  than offering the Windows print-layout/options dialog. The Windows help dialog is not ported; the
  Linux `--help` text covers the available controls.

The context menu keeps the applicable unsupported Windows commands visible but disabled. This list
tracks user-facing parity gaps; it does not include Windows-only implementation details that have no
Linux equivalent.

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

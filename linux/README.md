# JPEGView Linux frontend

This directory contains the first native Linux deliverable for upstream JPEGView v1.3.46.
The original Windows/ATL/WTL project remains unchanged under `src/`.

## Linux branch changes, in order of importance

This is the complete grouped summary of features and changes made since the native Linux branch
split from the Windows frontend. Later fixes, tests, and refactorings are grouped with the feature
they support.

1. **Native Linux viewer and distributable AppImage.** A native SDL2 frontend now opens individual
   images, multiple command-line inputs, and directories without modifying the original Windows
   application. It includes a resizable/maximizable/fullscreen window, drag-and-drop, a desktop
   entry, the upstream JPEGView application icon, an Ubuntu 20.04 build container, and AppImage
   packaging with bundled SDL and codec runtimes.

2. **Broad native format and color support.** Linux decoding covers JPEG, PNG/APNG, GIF, BMP, TGA,
   PSD, PNM, QOI, WebP, TIFF, HEIF/HEIC, AVIF, JPEG XL, JPEG XR, and LibRaw camera formats. Embedded
   color profiles are transformed through LCMS2. The save dialog writes JPEG, PNG, BMP, TGA, WebP,
   GIF, TIFF, PSD, PNM, QOI, HEIF/HEIC, AVIF, and JPEG XL still images. Codec detection and fixtures
   were made portable across Ubuntu 20.04 and newer distributions, including giflib installations
   without pkg-config metadata and HEIF encoders with different supported profiles.

3. **High-quality viewing, fitting, zooming, and panning.** JPEGView's high-quality downsampling and
   sharpening path was ported, with bicubic enlargement and a shared 1 GiB image-cache budget.
   Fitted JPEGs use libjpeg-turbo's native reduced DCT decode, avoiding full 4000×6000 pixel buffers
   when the screen needs only a smaller image. Nearby images are corrected and scaled on low-priority
   worker threads, then uploaded
   incrementally and retained as renderer-ready textures. The closest next and previous files take
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
   sibling folders; reload; and previous-folder history. Ordering supports logical filename,
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
   preloads nearest files first, and uses a bounded in-memory cache. Its divider is mouse-resizable,
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
   non-blocking direct image/subdirectory counts for folder rows. It also provides move-to-trash
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

11. **Image processing and animation.** The Windows histogram-derived automatic contrast correction
    and core rotate/mirror transforms are available non-destructively before save. Animated GIF,
    APNG, WebP, AVIF, and JPEG XL honor frame delays and loop counts. Movie mode supports fixed frame
    rates and folder advancement, slideshow transitions are rendered natively, Alt+R resumes, and
    Escape stops active playback before quitting. Decoded pixels, prepared display frames, and
    retained renderer textures share one memory budget with per-layer LRU retention, while a
    low-contention background workers predecode nearby non-JPEG files in both directions. JPEG
    neighbors take the reduced-resolution display path directly, while full pixels remain lazy.
    Decode completions feed a
    separate display-preparation worker pool, and both decoded and display caches reject stale source
    identities. Large evicted CPU buffers are retired on workers rather than destroyed on the event
    thread. Previously viewed and prefetched images
    therefore avoid repeated synchronous decoding, correction, high-quality scaling, and texture
    creation during navigation.

12. **Information overlays and window feedback.** F2 picture information and Shift+N/Ctrl+F2 filename
    overlays use compact translucent surfaces sized to their content with small comfortable margins.
    Filename, EXIF, and counter text remain responsive during navigation. The information popup uses
    a readable `W X H, Size` line and an unlabeled modification date. The EXIF popup includes a
    toggleable grayscale histogram, hidden by default. Overlay visibility persists
    immediately. The window title shows filename, dimensions, and size. Menus, dialogs, tooltips,
    and panels use the hinted 12-point Terminus bitmap when the complete string is printable ASCII,
    preserving lowercase letters as drawn and using crisp one-bit pixels without antialiased edges. Strings
    containing other characters use the desktop's configured UI font through Pango, retaining Unicode
    shaping and automatic installed-font fallback; translucent surfaces provide a consistent visual
    treatment.

13. **Reliable startup and saved session state.** Scale mode, ordering mode/direction, maximized or
    normal state, navigation-panel choices, filename/EXIF/histogram visibility, automatic correction,
    batch pattern, thumbnail visibility/width, and the image-cache budget are stored under XDG
    configuration paths. A previously
    maximized window is created maximized before it is shown, avoiding the visible delayed maximize.
    Compatibility handling keeps always-on-top optional on older SDL runtimes.

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
    compile independent codec stages and the viewer/tests in parallel.

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
ASCII text uses the crisp embedded 12-point Terminus bitmap. The atlas is generated from
`TerminusTTF-4.47.0.ttf` by extracting its embedded 16-pixel monochrome strike at 96 dpi; the
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
keyboard-command mapping, viewport fit/fill/zoom/pan geometry, open/save browser state,
resize-dialog validation, content-sized overlay layout, compact/advanced menu filtering and
keyboard selection, thumbnail layout/resampling, shared cache accounting, reduced JPEG display
decoding, and nearest-display upload priority, desktop-font resolution, decoder and writer round
trips across static and animated formats, all PNM variants, malformed input, batch-copy planning,
desktop-application command expansion, and JPEG metadata. The optional X11 smoke suite covers the
open browser's filtering, folder counts, sorting, direct-folder opening, focus restoration, paging,
Home/End and held-key movement; thumbnail display/resizing/clicking/persistence; context-menu
expansion and repainting; startup controls; mouse-wheel navigation versus Ctrl+wheel zoom; held
image navigation; maximize restoration; and persisted settings:

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
calculated in the background. Ctrl+R reloads, and
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
caching; this does not disable the small, separately bounded thumbnail cache.

The thumbnail panel is hidden by default and can be enabled from the context menu or with Ctrl+T.
It follows the active file ordering in a vertical strip: the current image remains centered and at
normal brightness, while surrounding images are darkened. Clicking a thumbnail opens that file.
Thumbnails are loaded incrementally in nearest-to-current order and kept in a bounded in-memory
cache. The panel reserves its own space on the left instead of covering the image. Drag its right
separator to adjust its width; row height follows the width, so narrower panels display more
thumbnails without large fixed vertical gaps. The width and visibility are preserved between runs.

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

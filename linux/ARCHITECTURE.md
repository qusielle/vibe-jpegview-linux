# Linux frontend architecture

The SDL frontend deliberately keeps platform-independent behavior outside `main.cpp`. New logic
should normally be added to one of these focused modules and covered by `tests/test_core.cpp`:

- `file_list`: discovery, ordering, navigation modes, direct sibling-folder jumps, current-file
  preservation, and the transient marked-image toggle pair used for A/B comparison. The marked
  path's index in the active ordered list is cached for constant-time thumbnail rendering.
- `double_page_model`: portrait-pair eligibility, cover handling, aspect-preserving shared-height
  spread geometry, page-step navigation, and physical-key direction in manga reading order. It owns
  no image pixels, filesystem work, or SDL resources.
- `archive_source`: generic container/member recognition, virtual-directory listings, source identity,
  and on-demand member access. ZIP catalogs use central-directory metadata; TAR/TGZ catalogs stream
  header metadata; unencrypted 7z uses libarchive's seekable reader; encrypted 7z uses the focused
  `seven_zip_backend` adapter described below. Ordinary unencrypted RAR catalogs and extraction
  remain on libarchive; when present, the optional `rar_backend` probes RAR metadata and handles
  encrypted RAR4/RAR5 catalogs and extraction.
  Immutable catalogs for at most four containers
  are keyed by device/inode/size/mtime and retain no extracted image payloads. Workers use independent
  libzip/libarchive handles, validate the selected member's identity, and stream at most 128 MiB of
  uncompressed data into an anonymous memory file for existing path-based decoders. Unsafe paths,
  archive links/devices are omitted; encrypted ZIP, data-encrypted 7z, and data-encrypted RAR retain
  their names and locked state. Header-encrypted 7z/RAR return a password-needed catalog result
  without exposing hidden names.
  Catalogs over 100,000 entries are rejected.
  New archive formats should extend this backend dispatch while keeping viewer consumers
  on the generic source operations.
- `seven_zip_backend.h` and its selected implementation: the optional `seven_zip_backend_7zip.cpp`
  wraps the official 7-Zip 24.09 `Format7zF` shared library through `IInArchive`, `IInStream`, and
  per-operation callbacks. A private handler and archive stream are created for each catalog or
  extraction operation; callbacks carry cancellation/generation checks, open/data password requests,
  and bounded member output. Passwords are never placed in argv or sent to a child process. The
  adapter emits raw member paths/metadata and writes extraction chunks into `archive_source`'s
  existing private memory-file flow, where canonical archive path checks and the 100,000-entry/
  128 MiB limits still apply. Without `SEVENZIP_SOURCE_ROOT`, Make selects
  `seven_zip_backend_unavailable.cpp`: unencrypted 7z remains available through libarchive, while
  encrypted 7z reports an explicit unsupported-encryption result.
- `rar_backend.h` and its selected implementation: `rar_backend_ffi.cpp` loads the separately
  packaged `librar_backend.so` through a narrow C ABI with opaque handles, fixed-width fields,
  explicit status codes, and bounded callbacks; `rar_backend_unavailable.cpp` preserves an explicit
  encrypted-RAR fallback for ordinary local builds. The Rust wrapper is pinned to Rust 1.89.0 and
  Apache-2.0 `bitplane/rars` at exact revision `afc60e4c669ba1fe6a18748b16b08164e69c5e44`.
  Cancellation, member-count/header limits, password copying/zeroization, and streaming output
  bounds are enforced across the ABI. Passwords are never passed through argv or a subprocess.
- `image`: validated mutable BGRA storage, half-open crop extraction, rotate/mirror transforms,
  high-quality downsampling, Catmull–Rom bicubic enlargement, and the automatic/manual picture-level
  processing pipeline.
- `crop_selection_model`: source-image crop bounds, free/aspect/fixed-size selection geometry,
  move/resize hit testing, image/view coordinate conversion, crop-mode drag eligibility, and JPEG
  MCU-boundary alignment; pixel-buffer cropping remains in `image`.
- `crop_size_dialog_model`: fixed-crop dimension text, focus/unit transitions, and validation.
- `image_processing` and `image_processing_store`: bounded adjustment ranges, parameter identity,
  pixel processing, the atomic native per-image levels database, and its portable backup/restore.
- `image_decoder`, `image_writer`, and `image_formats`: codec boundaries and format policy. Image
  decoders resolve archive-member paths through `archive_source` before invoking the existing codec
  path, retaining ordinary-file and reduced-DCT JPEG behavior. Decoded frames carry alpha-presence
  metadata so opaque-image textures can keep blending disabled.
- `cache_budget`, `image_cache`, and `display_image_cache`: aggregate cache accounting,
  source-aware decoded-image retention, nearest-first decode completion, and threaded picture-level
  processing/scaling of renderer-ready frames. Display keys capture every active processing value
  so an adjustment cannot reuse stale pixels. JPEG display requests use native reduced DCT decode
  before exact scaling, without requiring a retained full-resolution source frame.
- `input_commands`: SDL key chords to shared JPEGView command IDs.
- `desktop_association`: user-local desktop entry generation and atomic XDG MIME default updates.
- `transparency_pattern`: accepted background setting values and checkerboard tile colors.
- `settings` and `sort_mode`: persisted configuration (including the transparent-image background
  choice, default picture-level values,
  fixed crop dimensions/units, user crop aspect, the explicit crop-selection mode (disabled by
  default), zoom-navigator visibility, magnifying-glass size/zoom, and global double-page/manga-mode
  defaults), plus stable sort-mode values.
- `recent_files`: normalized absolute MRU image rows with one image per parent folder, a separately
  bounded per-file `ViewportSnapshot` LRU and independent bounded double-page/manga-mode snapshots,
  ordered row removal/restoration for the Recents dialog, plus tolerant atomic persistence in the
  XDG state directory. Virtual archive-member paths remain logical recent identities while source
  validation and cache freshness use the backing container. The recent database is independent from
  viewer settings and performs no image or directory scans while loading.
- `viewport`: fit/fill/manual zoom modes, pan state, destination geometry, and panning bounds that
  keep the viewport inside the image.
- `zoom_navigator_model`: responsive overview geometry, visible-image mapping, pointer conversion,
  and click/drag pan calculations for the transient zoom navigator.
- `magnifying_glass_model`: lens enable/size/zoom state and bounds, wheel-modifier transitions,
  and pointer-centered mapping from a clipped image source crop into lens content geometry.
- `resize_model`: resize-dialog values, aspect-ratio coupling, limits, filter selection, and pure
  focus/text-editing transitions.
- `context_menu_model`: the complete menu catalog, state-derived enablement/checkmarks,
  compact/advanced filtering, actionable-item keyboard navigation, and deterministic letter
  mnemonic assignment with duplicate-letter matching/cycling.
- `playback_scheduler`: wrap-safe animation, movie, and slideshow timing expressed as Viewer actions.
- `file_dialog_model`: filename filtering in Browse and full-path filtering in Recents, name/date
  sorting, UTF-8 editing, selection, paging, independently
  clamped viewport scrolling, proportional scrollbar thumb geometry and row-offset mapping, focus
  restoration, pane-aware preview image sizing, cancellable background archive listings and directory
  summaries (including supported archive containers in the directory count), encrypted-row marking,
  caller-preserved row order for recent MRU entries, and replaceable
  previews for a focused image or a directory's first image.
- `overlay_layout`: content-sized filename/EXIF panel geometry and window clamping.
- `viewer_chrome`: renderer-independent overlay and navigation-panel paint plans, including icon
  primitives, hit regions, dynamic labels, and tooltip placement.
- `thumbnail_panel_model` and `thumbnail_resampler`: strip geometry and current/marked row state,
  nearest-first cache scheduling,
  cancellation/LRU policy, memory sizing, alpha-preserving antialiased source-area reduction, and
  low-priority derivation from completed neighbor display frames. The viewer supplies the active
  double-page partner index so both displayed spread pages receive active-row styling without
  moving the panel's centering or changing which row represents the navigation index.
- `image_info_model`: stable image-position, dimensions, date, and file-size presentation. Viewer
  supplies the active spread partner so filename and information overlays report both visible
  positions without coupling the formatting module to SDL or spread state.
- `system_font` and `bitmap_font`: desktop-font discovery, UTF-8 shaping, measurement, rasterization,
  and exact embedded-glyph ink bounds for crisp renderer overlays such as menu mnemonics. The SDL
  adapter creates printable-ASCII bitmap-font textures with nearest-neighbor sampling while keeping
  best-quality sampling for image textures, preventing filtering from adding pixels to blank glyph
  cells.
- `app_icon`: extraction of the application icon embedded from the upstream ICO resource.
- `batch_copy`: pattern expansion, previews, and pure dialog focus/selection/scroll transitions.
- `desktop_applications`: non-UI discovery and planning for Open with commands.
- `external_commands`: pure argv plans and fallback order for printing, wallpaper, clipboard,
  desktop opening, trash, lossless JPEG crop, and lossless JPEG transforms.
- `exif_reader`: JPEG metadata parsing.

`main.cpp` remains the SDL composition root. It owns windows, textures, event dispatch, rendering,
and invoking desktop integrations. It reads the persisted transparency pattern and, for frames
marked as containing alpha, paints the matching background beneath the image before alpha-blended
texture rendering. The same renderer-thread helper backs transparent thumbnails and open-dialog
previews; opaque textures retain the non-blended path. It should translate SDL events into operations
on the modules above rather than duplicate their state. Context-menu pointer hit testing also lives
here: the Right key's release activates the row under the pointer, or moves to the next column when
released outside the menu, while key-held pointer movement remains available for click-like selection.

Archive members use the existing filesystem-shaped path contract (`container.ext/member.ext`) so
navigation, sorting, recent-folder grouping, cache keys, and decoder APIs remain unchanged. The
browser labels and color-marks ZIP/TAR/TGZ/7z/RAR containers and archive images without retaining image
payloads; Recents uses the same cancellable preview worker. ZIP catalogs retain central-directory
metadata only. TAR/TGZ catalogs stream member headers and skip payloads. Unencrypted 7z catalogs use
seekable libarchive input; encrypted 7z catalogs use the optional SDK adapter and retain member
ordinal, normalized name, size, modification time, and per-entry encryption state. Header encryption
is recorded separately from encrypted data entries because it hides the entire catalog. Ordinary
unencrypted RAR catalogs remain on libarchive; the optional RAR backend inspects RAR metadata and is
used for encrypted RAR4/RAR5 catalogs and extraction. Cold
archive-directory listing runs in `ArchiveDirectoryLoader`; a newer request
cancels obsolete libarchive, 7-Zip, or RAR callback work at read/seek boundaries and generation-checks returned results. A
selected ZIP member is read by index; TAR/TGZ/7z/RAR members are found by rescanning archive order. Gzip
streams are sequential, and solid 7z/RAR5 blocks can require decompressing earlier entries to reach a
later member. The libarchive path does not support solid RAR4 archives. The encrypted backend uses
sequential extraction for solid predecessors, but its application fixtures currently cover only
single-member encrypted archives. Multi-volume RAR sets and split members are not supported. In either backend, only
the selected member is copied, bounded to 128 MiB, into a short-lived
anonymous memory file. That output cap does not bound the codec's internal memory or CPU use. The
source archive is never modified and no persistent extraction directory is created. ZIP member
encryption is handled through libzip: the Open dialog owns password entry, while
`ArchivePasswordDialogModel` masks UTF-8 input. The SDL adapter handles Ctrl+V, Ctrl+Shift+V, and
Shift+Insert by passing clipboard UTF-8 through the same model and wiping SDL's temporary clipboard
buffer. `archive_source` checks credentials and keeps
accepted credentials in a process-only cache keyed by backing-file identity. Credentials are never
stored in settings or recent-file state. The 7z adapter supplies passwords through SDK callbacks and
extracts only through the same bounded memory-file path; it never invokes an external `7z` process.
The Rust RAR plugin exposes a fixed-width C ABI, catches panics before they can cross that
boundary, copies and zeroizes password buffers, and streams each extraction chunk directly into the
same bounded memory-file path. It never invokes a subprocess. The Open dialog marks hidden-header
`.7z`/`.rar` rows as encrypted after a locked result and prompts only when the user enters/unlocks
them. Previews use only an already cached credential and report a locked preview instead of opening
UI. Password validation tests the submitted candidate directly; successful header-encrypted catalogs
are invalidated when credentials are forgotten or cleared, so hidden names are not retained after
cache clearing. This integration covers encrypted RAR4/RAR5 data and header encryption. Multi-volume
sets and split members are unsupported; RAR7 is not yet covered by this application's fixture suite.
TAR/TGZ have no native password encryption. Archive links/devices and unsafe paths are omitted.
Filesystem-only actions are disabled or guarded for archive members, while image edits and saves
still use the ordinary in-memory image path. Future containers should add extension recognition
and list/read operations here rather than branching in the SDL viewer, recents, caches, or codecs.

Release Docker builds download the pinned 7-Zip 24.09 source archive and verify its SHA-256 before
building only `Format7zF` with `DISABLE_RAR=1`; they also install Rust 1.89.0 using a checksum-pinned
rustup-init and fetch the exact RAR source revision. AppImage and Ubuntu 24/26 `.deb` packages carry
both plugins, their license notices, wrapper source, and upstream source archives. Binary release
bundles use the same `lib/jpegview-linux/` layout next to the executable and include those notices
and sources. Ordinary local builds select explicit unavailable-backend fallbacks unless
`SEVENZIP_SOURCE_ROOT` and/or `RAR_BACKEND_ROOT` are set.

The Open dialog routes cold archive-directory scans through `ArchiveDirectoryLoader`; direct
command-line archive startup still builds the initial `FileList` through the synchronous source API.
Consequently, a cold TGZ passed directly at startup can wait for its sequential catalog scan and a 7z
or RAR can wait for its catalog scan before the viewer is ready, while browsing into those archives
from the Open dialog stays responsive.

The magnifying-glass lens is a temporary viewer interaction; its enablement resets each run while
its size and magnification are stored by `settings`. Its pure model owns those values, wheel
modifiers, and clipping math; the SDL adapter owns cursor visibility, image-area hit testing, and lens
painting. When an unmodified image has a lower-resolution renderer texture, Viewer asks the
display-image worker cache for a source-capped higher-resolution frame only while the lens is visible.
That optional request is ordered behind foreground and neighbor work, shares the existing byte
budget, and is canceled/released when no longer needed. The current renderer texture is always the
immediate fallback, and the higher-resolution SDL texture is uploaded, protected, evicted, and
destroyed on the renderer thread. Thus enabling the lens adds no decoding or resampling to ordinary
navigation and does not introduce a second cache budget.

At startup the composition root creates, paints, and maps the final SDL window before constructing
the initial `FileList` or loading its current image. Directory enumeration and the existing
decode/display-cache path then run unchanged while the visible dark startup frame provides feedback;
renderer resources remain confined to the main thread. Recent history is read after that startup
frame is shown and written once during normal cleanup, keeping history I/O out of the initial window
presentation and rapid navigation path. The three file-dialog workers and the low-priority
thumbnail-resampling worker start only when their first request arrives; the decoded-image and
display-preparation pools remain ready before the first image load so foreground rendering and
neighbor preparation are not delayed. `LoadCurrent` captures the outgoing file's exact viewport
snapshot when the path changes, restores that file's saved snapshot before sizing a new display
request, and records the path only after decoding and presentation setup succeed. New paths use the
shared navigation snapshot. Clipboard temporary paths never become recent entries, while the
ordinary file dialog's Recents tab shows one MRU image per parent folder and reuses the cancellable
preview worker; separate Browse and Recents dialog models preserve each tab's filter and selection.
With no positional startup arguments, the viewer skips the initial image-list scan and keeps the
event loop alive with Browse open at the current working directory, even if that directory contains
supported images. If the sole explicit startup argument is a directory with no directly supported
images, it opens Browse at that directory instead; other empty startup cases retain the no-images
exit behavior.

The Recents dialog delegates row removal and restoration to `recent_files`, preserving the unique
per-parent MRU order. Viewer owns the transient LIFO undo stack and clears it when the dialog closes;
Delete and the Remove button are active only in Recents, while Ctrl+Z is handled only by the open
file dialog. Removing a row does not erase its separate per-image viewport or display-mode snapshots.

Double-page behavior is modeled by `double_page_model` and adapted by Viewer. The active FileList
entry remains the navigation anchor; a visible partner is a separate page texture prepared through
`display_image_cache` at the partner's current spread-slot size. Pairing uses only already-available
dimensions and only strict portrait neighbors (the first cover is single), so checking the mode does
not decode a neighbor on the event thread. The virtual viewport canvas combines both aspect-preserving
page slots; zoom, pan, the zoom navigator, and magnifier hit testing use that canvas while crop
selection remains in the anchor page's source coordinates. SDL textures are still uploaded and
destroyed on the renderer thread. A spread does not create a composite pixel buffer or use the
single-image transition effect. The D/J mode overrides are stored beside, but independently from,
per-file viewport snapshots so Recents can restore them without changing the shared defaults in
settings.

The recent-files database stores normalized absolute paths with byte-safe record encoding, so legal
newlines and non-UTF-8 filename bytes do not break its line-based format. Loading skips malformed
records, accepts only finite zoom values, and clamps finite values to the viewport's supported zoom
range. Recent-folder retention is capped at 100 rows and viewport snapshots at 256 paths; these
bounds are independent so files from older folder rows can still restore their last view. Double-page
mode snapshots have their own 256-path bound.

## Refactoring status

The planned Viewer decomposition is complete. Future extractions should be driven by a concrete
feature or maintenance problem rather than moving SDL calls for its own sake.
   Rendering should remain last because pixel-level X11 smoke tests are its best safety net.

The image, viewport, playback, file-dialog, dialog-controller, context-menu, thumbnail cache,
external-command, font, image-information, and viewer-chrome extractions establish the intended
pattern: a small pure C++ object, thin SDL adapter methods in Viewer, focused core tests, then UI
smoke tests for integration. Worker threads belong behind model APIs (as with directory summaries),
while SDL windows, textures, cursors, process execution, and event translation remain owned by
platform adapters. File-dialog size/position, resize-grip hit testing, and preview-divider dragging
remain in the SDL Viewer adapter; the dialog dimensions and preview/list ratio persist through the
settings module. A proportional scrollbar reserves a narrow list gutter; its pure geometry and thumb
mapping live in `file_dialog_model`, while the adapter draws it, pages on track clicks, captures thumb
drags, and applies scroll offsets without changing keyboard selection. Wheel scroll deltas update the
model viewport independently from keyboard selection, then the adapter focuses the row under the
pointer. File-dialog preview workers derive their decode target from the pane's usable image area and
resolve/scale only the newest requested selection using
the thumbnail resampler's source-area antialiasing. A pane resize replaces the target-size request;
the generation check prevents stale work from replacing the current preview. Preview pixels remain
outside the persistent viewer caches, and their SDL texture is uploaded and destroyed by Viewer.
Display pixels may be prepared on workers, but SDL texture upload and destruction stay on the
renderer thread because SDL renderer objects are not thread-safe. Decoded pixels, prepared frames, and retained SDL textures
reserve from one configured cache budget. Prepared
frames are uploaded at most once per event-loop iteration, after the current frame is presented;
unfinished closer neighbors block farther uploads. Expensive CPU-buffer destruction is handed back to
cache workers. When the thumbnail panel is visible, completed display frames also feed one bounded,
very-low-priority thumbnail-resampling queue before their CPU pixels are retired; this avoids a
second large-file decode while leaving renderer upload and display preparation ahead of thumbnail
work. Static images backed by a ready display texture defer full-pixel materialization
until an edit, copy, save, histogram, or another pixel-consuming operation actually needs it.
For fitted JPEGs, header dimensions are cached by file size and modification time and workers decode
the smallest native libjpeg scale that covers the stable viewport. This makes renderer-ready textures,
rather than ~96 MiB source frames, the primary navigation cache for high-resolution photo folders.
JPEG headers and pixels are consumed through read-only file mappings, avoiding another compressed-data
buffer while allowing the kernel page cache to service repeated neighboring access. EXIF/comment
parsing reads only bounded JPEG header segments and stops before compressed scan data.
The speculative display window is conservatively sized from the shared byte budget and viewport area
(with a finite work cap), rather than using the decoded cache's fixed neighbor count.
Ordinary picture-level previews are processed by the display workers; full-resolution pixels are
processed only when save, copy, resize, transform, or another pixel-consuming operation requires
them. Rapid foreground preview requests coalesce queued obsolete adjustments, and an in-flight stale
foreground result cannot replace the latest one. The per-image parameter store is keyed by normalized
absolute filename and stores both the adjustment values and per-image automatic-correction state.
With keep-between-images enabled, current values take precedence; otherwise a saved entry is
restored. `image_processing` provides clamped ranges and identity defaults for the panel.
Unsharp-mask parameters are included in display request keys so changing its preview cannot reuse
stale prepared pixels.

Crop selection remains in source-image coordinates while the SDL adapter maps pointer gestures and
the dotted/handled overlay through the current viewport destination. Crop and copy actions first
materialize the full-resolution processed image, then extract only the selected region instead of
copying a second full-size source buffer; destructive crop updates both the cropped correction
base and its reprocessed display image as one operation, and flattening an animation is explicit.
Lossless JPEG crop reads sampling factors from the JPEG header, aligns the source selection to the
corresponding MCU grid, and delegates the crop to `jpegtran` through a direct argv plan. Fixed crop
size and custom aspect choices are user settings; fixed-size editor layout and rendering are owned by
the SDL composition root, with text and validation transitions in `crop_size_dialog_model`. Applying
a crop updates the current logical image dimensions, while copying a
selection leaves the viewed image unchanged. Lossless output is staged beside its destination for an
atomic rename and adopts the existing file's mode or the normal umask-derived mode for a new file.

The zoom navigator is a transient renderer overlay: when the image exceeds the viewport, hovering
its upper-right hotspot, zooming, or panning reveals a miniature of the current renderer-ready image
and a rectangle marking the visible source region. The SDL adapter owns hit testing, capture, cursor,
and drawing; `zoom_navigator_model` owns only the geometry and pointer-to-pan math. The overlay reuses
the current image texture, so it does not schedule another decode, resize, or cache entry. Click and
drag pans are clamped by `Viewport::ClampToView` so they cannot expose empty space beyond the image.
The user-visible toggle is persisted through `settings`.

## Build version metadata

`version.sh` resolves the nearest reachable semantic-version Git tag and adds commit-distance and
working-tree metadata for development builds. Make generates a small forced-include header from the
resolved or explicitly overridden `VERSION`; the executable uses that single value for `--version`
and the About panel. AppImage and Debian packaging scripts receive the same version; the AppImage
desktop entry's `X-AppImage-Version` and Debian control metadata use it as well. GitHub Actions
fetches tag history before resolving the value. Docker build contexts omit `.git`, so workflows and
documented Docker invocations resolve the version on the host and pass it into the container. Avoid
independent version literals in build, package, and executable metadata.

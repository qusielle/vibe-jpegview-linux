# Linux feature candidates

This is an unprioritized list of possible future work, not a promise or release plan. Items are
kept separate where they can be implemented and reviewed independently. Archive formats remain
separate candidates even if an implementation later shares a backend. Archive-format candidates
are independent of the JPEGView_L comparison below. The current behavior documented in
[`README.md`](README.md) is authoritative; candidate sources are identified by name.

## Archive input candidates

- **ZIP image archives:** browse supported images stored in ZIP files.
- **7z image archives:** browse supported images stored in 7z files.
- **RAR image archives:** browse supported images stored in RAR files.
- **TAR image archives:** browse supported images stored in uncompressed TAR files.
- **TGZ image archives:** browse supported images stored in `.tar.gz` files.
- **CBZ comic archives:** browse images in comic ZIP files. Comic-page bookmarks and remembered
  reading positions are explicitly out of scope for this candidate.
- **CB7 comic archives:** browse images in comic 7z files. Comic-page bookmarks and remembered
  reading positions are explicitly out of scope for this candidate.

## Comic-reading interaction candidates

- **Magnifying glass:** add a pointer-following rectangular lens that shows the image region under
  it magnified. In [YACReader](https://github.com/YACReader/yacreader) 9.6.2.0 the option is named
  “Magnifying glass”; press `Z` to toggle the lens and use the mouse wheel in this mode to increase
  or decrease the rectangle's size. Prefer a direct port of that implementation if feasible;
  otherwise adapt it rather than starting from scratch.
- **Reverse reading order in double page mode:** allow the two pages in a spread to swap sides for
  right-to-left reading. This matches the [YACReader](https://github.com/YACReader/yacreader) menu
  option; prefer porting or adapting its implementation if feasible rather than reimplementing the
  behavior independently.

## General interaction candidates

- **Context-menu letter hints (mnemonics):** underline a mnemonic letter in each applicable menu
  item and activate that command when its letter is pressed while the menu is open. For example,
  underline `O` in “Open file” so pressing `O` opens the file dialog.

## Navigation and file-history candidates

- **Recently opened files dialog (major feature):** show recent files in a dedicated dialog with
  the path left-aligned and filename right-aligned. Reuse the open-file dialog's layout and preview
  pane, showing a preview for the focused entry. Remember each file's zoom and viewing mode and
  restore them when that file is opened again.

## Candidates from [KrokusPokus/JPEGView_L](https://github.com/KrokusPokus/JPEGView_L)

- **Transparency background setting:** persist a choice of black, white, or checkerboard behind
  transparent image pixels, extending the checkerboard option found in JPEGView_L.
- **Book Mode:** add a book-oriented viewing mode with page size configurable as a percentage of
  the window height.
- **Linear-light resampling:** perform display scaling in linear light to reduce darkened edges and
  moiré in line art and other high-contrast imagery. The existing high-quality resampler remains
  the baseline; this would change its color-space math and needs image-quality and performance tests.
- **Additional downsampling kernels:** offer Hermite, Mitchell, and Catmull–Rom as selectable
  reduction filters, separately from the current best-quality and Lanczos paths.
- **Smooth keyboard panning:** interpolate keyboard pan movement instead of applying only discrete
  fixed-size steps.
- **Embedded-profile toggle:** allow users to disable the current automatic use of embedded color
  profiles.
- **Per-folder single-instance mode:** optionally route launches for a folder to one viewer window.

## Candidates from [andrewvladved/jpegview's `annotations` branch](https://github.com/andrewvladved/jpegview/tree/annotations)

Reviewed at `v1.3.46-annotations` (`dc2c4e7`). These are additions on that branch, compared with
its `master` base and with the current Linux frontend; they are not a claim that every Windows
feature is absent from Linux.

- **Image annotations (major):** add image-coordinate freehand strokes, text, rectangles, ellipses,
  and triangles, with arrowheads, fill/outline, color, opacity, width, and text-background controls.
  Include undo/redo, clear-all, drawing tools in the navigation panel, and a save/discard/cancel
  prompt before navigation, geometry edits, or close. The branch burns annotations into the image
  and offers overwrite or Save As; it does not keep editable annotations in sidecar files or allow
  individual marks to be moved/deleted. This is distinct from Linux's existing Ctrl+M
  mark-image/toggle-back navigation feature. See the branch's [design](https://github.com/andrewvladved/jpegview/blob/annotations/docs/superpowers/specs/2026-09-25-image-annotations-design.md),
  [annotation types](https://github.com/andrewvladved/jpegview/blob/annotations/src/JPEGView/AnnotationTypes.h),
  and [implementation plan](https://github.com/andrewvladved/jpegview/blob/annotations/docs/superpowers/plans/2026-09-25-image-annotations.md).
- **Vertical scroll reading mode:** play a folder by holding each image at its top, gliding down
  at a configurable screen-pixel speed, holding at the bottom, then advancing. By default, fit/crop
  each image to fill the window; an alternate setting preserves the current zoom and scrolls through
  the part extending beyond the viewport. Configure the hold duration; short images need no glide.
  This is separate from the existing slideshow and movie modes. See the branch's
  [scroll state model](https://github.com/andrewvladved/jpegview/blob/annotations/src/JPEGView/ScrollMath.cpp).
- **Fit-relative zoom mode:** optionally define the window-fitted image as 100%, so zoom presets,
  steps, snap points, and pause points have the same relative effect for differently sized images.
  Retain the current source-pixel scale in the zoom readout as well. Linux currently preserves the
  chosen zoom mode across navigation, but its 100% zoom still means original-pixel scale. See
  [zoom calculations](https://github.com/andrewvladved/jpegview/blob/annotations/src/JPEGView/ZoomMath.cpp).
- **Shared cross-fade for playback modes:** extend Linux's existing slideshow transitions with an
  optional cross-fade between files during movie playback and the proposed scroll mode, using one
  transition duration. Do not fade frames within an animated image; cap or skip fades that would
  consume most of a movie frame interval. Preserve the existing directional slideshow effects. See
  the branch's [playback handoff](https://github.com/andrewvladved/jpegview/blob/annotations/src/JPEGView/MainDlg.cpp).
- **Custom slideshow interval:** supplement the current fixed waiting-time choices with a bounded,
  persisted numeric interval entry. Reuse the branch's
  [numeric value dialog](https://github.com/andrewvladved/jpegview/blob/annotations/src/JPEGView/SetValueDlg.cpp)
  as a reference.
- **Custom movie frame rate:** supplement the current fixed playback-rate choices with a bounded,
  persisted numeric FPS entry; the branch uses the same
  [numeric value dialog](https://github.com/andrewvladved/jpegview/blob/annotations/src/JPEGView/SetValueDlg.cpp).
- **Folder wrap-around toggle:** expose a menu setting to stop at the first/last image instead of
  wrapping to the other end. Linux currently wraps within a folder by default and has no user-facing
  toggle for this behavior. See the branch's
  [file-list setting](https://github.com/andrewvladved/jpegview/blob/annotations/src/JPEGView/FileList.cpp).
- **Transparent title-bar presentation:** investigate a Linux-native equivalent to the branch's
  transparent title-bar panel. The Windows implementation uses DWM frame integration, so this is
  not a direct API port; Linux already supports hiding the window title bar.

## Candidates from [Masir01/jpegview_up](https://github.com/Masir01/jpegview_up)

Reviewed the repository's default [`dev-hw`](https://github.com/Masir01/jpegview_up/tree/dev-hw)
branch at `79a18f9` and also checked [`dev-up`](https://github.com/Masir01/jpegview_up/tree/dev-up)
at `93efb7a`, which was 24 commits ahead and contains newer decoder work. The bullets below are
gaps against Linux, not a recommendation to port Windows-specific code or dependencies verbatim.

- **Asynchronous directory scanning:** enumerate large folders off the event thread so opening a
  folder, refreshing it, or changing navigation scope remains responsive. Cancel obsolete scans and
  reject stale results so a slower previous-directory scan cannot replace the current list or
  resurrect removed files. Linux currently scans synchronously in `FileList::ScanDirectory`; the
  fork's [file-list implementation](https://github.com/Masir01/jpegview_up/blob/dev-hw/src/JPEGView/FileList.cpp)
  is a reference for the worker/result pattern.
- **Oversized JPEG viewing:** Linux currently rejects images above its 100-megapixel limit. Add a
  bounded reduced-resolution JPEG decode option for larger sources, clearly identify when the
  displayed pixels are only a reduced preview, and do not save that preview as though it were the
  full-resolution original. The fork's [`dev-up` JPEG decoder](https://github.com/Masir01/jpegview_up/blob/dev-up/src/JPEGView/TJPEGWrapper.cpp)
  chooses a capped downsampling factor when the normal image limits would be exceeded.
- **Viewport-sized WebP decode:** the fork's `dev-up` fast-fit path asks the decoder for a
  screen-sized result for lossy WebP as well as JPEG. Linux already uses reduced-DCT decoding for
  fitted JPEGs, so the distinct candidate is to add a reduced WebP display path while preserving
  full-resolution access when zooming to actual size or saving; validate animated WebP separately.
  See its [WebP wrapper](https://github.com/Masir01/jpegview_up/blob/dev-up/src/JPEGView/WEBPWrapper.cpp).
- **Optional half-size RAW preview:** add an opt-in fast RAW viewing path that develops at half
  width and height (one quarter of the pixels), with a full-resolution path still available for
  detailed inspection and output. Linux currently develops RAW images at full resolution; the fork's
  [RAW wrapper](https://github.com/Masir01/jpegview_up/blob/dev-hw/src/JPEGView/RAWWrapper.cpp)
  shows the LibRaw setting and its configuration option.
- **Large Photoshop documents (`.psb`):** extend the existing PSD reader to PSB's large-document
  variant and dimensions. Linux currently accepts `.psd` only and caps decoded images at 100
  megapixels. The fork's [PSD wrapper](https://github.com/Masir01/jpegview_up/blob/dev-hw/src/JPEGView/PSDWrapper.cpp)
  is a format-behavior reference, not a dependency to copy.
- **DDS texture images:** add optional `.dds` decoding, including common BCn/DXTC compressed
  textures, using a Linux-compatible decoder rather than the fork's Windows DirectXTex library. This
  is a specialized format candidate, lower priority for a photo viewer. See its
  [DDS reader](https://github.com/Masir01/jpegview_up/blob/dev-hw/src/JPEGView/DDSWrapper.cpp).
- **Direct zoom after selection:** add a setting to choose whether releasing a newly drawn selection
  zooms directly to that region or opens the crop-action menu. Linux already supports Shift-drag to
  zoom a selection, but its normal selection release opens the crop menu. See the fork's
  [`SelectionZoomMode` change](https://github.com/Masir01/jpegview_up/commit/4c47c927f81d9db3f0509f425c338c50cee32f89).

The fork also has linear-light resampling and per-folder single-instance behavior on `dev-up`; both
ideas are already tracked above under the JPEGView_L comparison and are not duplicated here. Its
reduced-DCT JPEG display path also overlaps Linux's existing fitted-JPEG path, so the separate
viewport-decode candidate is limited to WebP.

## Existing Windows-parity gaps

These user-facing gaps are also listed in the [Linux README](README.md#known-windows-parity-gaps)
and remain possible candidates:

- **Free rotation and perspective correction:** interactive rotation and perspective/tilt-correction
  panels beyond the existing quarter-turn and mirror operations.
- **Processing-parameter set exchange:** exchange a second parameter set for image comparison; this
  is distinct from the already implemented marked-image/toggle-back workflow.
- **Windows settings administration:** edit global/user Windows configuration files or update a
  user configuration from the global template. Any Linux version would need a suitable native
  settings design.
- **Open-With management and custom commands:** manually manage the Open With menu and add
  Windows-style user command definitions.
- **Per-extension desktop associations:** provide finer-grained default-viewer selection than the
  existing common-MIME registration.
- **Windows-compatible parameter database administration:** support interoperability with the
  Windows binary database; Linux currently stores its own text database and native backups.
- **Print setup and full help:** add a print-layout/options dialog and port the fuller Windows help
  content and localization.

## Already covered or not a direct port candidate

- **Catmull–Rom enlargement is already implemented.** The Linux display-resize path uses
  endpoint-preserving Catmull–Rom bicubic interpolation for enlargement. The possible filter work
  above concerns additional selectable downsampling kernels, not adding Catmull–Rom enlargement.
- **Fixed `CacheRange=2` behavior is not currently proposed.** Linux has a shared configurable image
  cache budget and adaptive neighbor prefetch; adopting a fixed before/after count would need a
  demonstrated use case rather than copying KrokusPokus/JPEGView_L's setting literally.

## Compatibility items to verify if reproduced

- [KrokusPokus/JPEGView_L's release notes](https://github.com/KrokusPokus/JPEGView_L/releases)
  report a fix for very large JPEGs that failed to load. Linux has memory-mapped input and
  reduced-DCT display decoding, but also has an explicit 100-megapixel image limit. Masir01's
  `dev-up` branch adds a bounded downsample path for oversized JPEGs (see above); compare the limits
  and output using a reproducible file before treating the existing cap as a regression.
- [KrokusPokus/JPEGView_L's release notes](https://github.com/KrokusPokus/JPEGView_L/releases)
  report JPEG XL animation and archive-contained image fixes. Linux advertises animated JPEG XL
  support, while archive browsing is not implemented; verify a concrete failing fixture before
  adding a separate bug-fix item.

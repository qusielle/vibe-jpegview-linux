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
  chosen zoom mode across navigation, but its 100% zoom still means original-pixel scale. A related,
  distinct request is to preserve the manually chosen on-screen width, height, or area while moving
  between images ([upstream issue #285](https://github.com/sylikc/jpegview/issues/285)). See
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
  resurrect removed files. Linux currently scans synchronously in `FileList::ScanDirectory`; reports
  about large-folder startup ([upstream issue #194](https://github.com/sylikc/jpegview/issues/194)
  and [#263](https://github.com/sylikc/jpegview/issues/263)) reinforce this need. The fork's
  [file-list implementation](https://github.com/Masir01/jpegview_up/blob/dev-hw/src/JPEGView/FileList.cpp)
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

## Candidates from upstream JPEGView issues

Reviewed the [open issue list](https://github.com/sylikc/jpegview/issues) and the closed
[`wontfix` issues](https://github.com/sylikc/jpegview/issues?q=is%3Aissue+state%3Aclosed+label%3Awontfix)
on 2026-09-26. An upstream `wontfix` label records that project's decision; it does not by itself
rule out a useful native Linux feature. That query returned four issues at review time; their
disposition is recorded below. Duplicate requests and behavior already present in Linux are not
repeated as new candidates below.

- **Sort by pixel dimensions:** add a file ordering by pixel area (with a deterministic tie-breaker),
  alongside the existing filename/date/size choices. Cache or lazily obtain dimensions so sorting a
  large folder does not synchronously decode every image. This was independently requested in
  [issues #388](https://github.com/sylikc/jpegview/issues/388) and
  [#359](https://github.com/sylikc/jpegview/issues/359).
- **Sort by capture date:** add EXIF capture-time ordering, with a documented fallback for missing
  dates and stable tie-breaking. Metadata lookup should not put a full-folder scan on the event
  thread. See [issue #224](https://github.com/sylikc/jpegview/issues/224).
- **More resilient capture metadata:** read common shooting fields from the available TIFF/EXIF
  directories, including a safe IFD0 fallback when the ExifIFD is absent, and extend beyond JPEG
  where a supported format exposes equivalent metadata. This addresses the DNG/JPEG metadata layout
  described in [issue #393](https://github.com/sylikc/jpegview/issues/393); it should have fixtures
  for both layouts before changing the reader.
- **Show the embedded profile name in image information:** Linux already applies embedded ICC
  profiles, but does not identify the profile in its information overlay. Add a concise profile
  description when available; pixel dimensions are already shown, so a megapixel count from
  [issue #363](https://github.com/sylikc/jpegview/issues/363) is optional rather than essential.
- **Go to image number:** provide a small command to jump directly to an index in the current
  ordered file list, complementing the existing `[current/total]` indicator in the F2 information
  overlay. See
  [issue #26](https://github.com/sylikc/jpegview/issues/26).
- **Show position in the window title:** add `[current/total]` to the SDL window title; Linux
  currently shows that count in the F2 information overlay, but its title only shows the filename,
  dimensions, and file size. See [issue #260](https://github.com/sylikc/jpegview/issues/260).
- **Skip hidden images:** add an optional setting to omit hidden image files from navigation. On
  Linux, define this in terms of dotfiles (and decide explicitly whether `.hidden` directory
  metadata should also count), rather than copying Windows hidden-attribute behavior. See
  [issue #114](https://github.com/sylikc/jpegview/issues/114).
- **Quick rename of the current image:** add a one-file rename command and shortcut, initially
  selecting the basename but not the extension, with collision-safe behavior. This complements the
  existing batch rename/copy dialog; see [issue #280](https://github.com/sylikc/jpegview/issues/280).
- **Deletion confirmation preview:** show a small thumbnail and filename in the move-to-trash
  confirmation so the user can verify the target before confirming. Reuse an already available
  thumbnail when possible rather than decoding synchronously; see
  [issue #337](https://github.com/sylikc/jpegview/issues/337).
- **Pixel color sampler:** show the color under the pointer in a small readout and optionally copy
  it in a common notation such as hexadecimal RGBA. Define whether sampling reflects the source or
  the currently processed display. See [issue #278](https://github.com/sylikc/jpegview/issues/278).
- **Selection convenience actions:** optionally copy selected pixels to the clipboard immediately
  after a selection is made, then clear the selection, without changing the existing explicit crop
  and copy actions. The interaction should be configurable to avoid surprising current users. See
  [issue #193](https://github.com/sylikc/jpegview/issues/193).
- **Fast view-only color commands:** provide a direct invert-colors toggle and a separate quick
  grayscale/desaturate command; both should be reversible display operations, not destructive edits.
  See [issue #273](https://github.com/sylikc/jpegview/issues/273) and
  [#238](https://github.com/sylikc/jpegview/issues/238).
- **Krita documents (`.kra`):** optionally display the flattened `mergedimage.png` embedded in a
  Krita archive, without implying support for its editable layers. Bound archive extraction and
  validate paths and sizes. See [issue #385](https://github.com/sylikc/jpegview/issues/385).
- **HDR still-image viewing:** investigate a controlled tone-mapping path for HDR AVIF/JXR content
  on ordinary SDR displays, preserving the source and avoiding clipped or unexpectedly dark output.
  Treat this as exploratory until representative HDR fixtures and a defined output policy exist.
  The request appears in open [issue #239](https://github.com/sylikc/jpegview/issues/239) and in
  upstream-closed-wontfix [issue #183](https://github.com/sylikc/jpegview/issues/183).
- **Motion Photos:** explore presenting the embedded video portion of a phone Motion Photo as an
  optional action while retaining the JPEG still as the normal image and navigation item. This may
  require a new video demux dependency and is lower priority. See
  [issue #275](https://github.com/sylikc/jpegview/issues/275).
- **GPS map action:** when GPS coordinates exist in EXIF, offer an explicit action to open them in a
  user-configurable map URL. Keep it opt-in per click so coordinates are not sent anywhere
  automatically. See [issue #59](https://github.com/sylikc/jpegview/issues/59).

Some issue ideas are already represented elsewhere in this file or implemented in Linux:

- Recent-file history ([#397](https://github.com/sylikc/jpegview/issues/397)), comic/archive reading
  ([#293](https://github.com/sylikc/jpegview/issues/293) and
  [#301](https://github.com/sylikc/jpegview/issues/301)), transparency backgrounds
  ([#43](https://github.com/sylikc/jpegview/issues/43),
  [#287](https://github.com/sylikc/jpegview/issues/287),
  [#339](https://github.com/sylikc/jpegview/issues/339)), oversized images
  ([#141](https://github.com/sylikc/jpegview/issues/141) and
  [#371](https://github.com/sylikc/jpegview/issues/371)), and custom commands
  ([#383](https://github.com/sylikc/jpegview/issues/383)) overlap candidates above.
- The F2 information overlay already shows `[current/total]`, though adding it to the window title
  remains a candidate ([#260](https://github.com/sylikc/jpegview/issues/260)); the Linux thumbnail
  pipeline supports PNG, AVIF, and JPEG XL as well as JPEG
  ([#387](https://github.com/sylikc/jpegview/issues/387)); crop mode is off by default
  ([#401](https://github.com/sylikc/jpegview/issues/401)); and choosing the processed image for
  wallpaper is already supported ([#400](https://github.com/sylikc/jpegview/issues/400)). The
  requested Linux port itself ([#69](https://github.com/sylikc/jpegview/issues/69)) is the purpose
  of this repository.
- The upstream `wontfix` request for single-instance behavior ([#155](https://github.com/sylikc/jpegview/issues/155))
  overlaps the per-folder single-instance candidate listed under KrokusPokus/JPEGView_L; decide
  whether Linux should forward later launches globally or per folder. Duplicate HEIC enumeration
  ([#147](https://github.com/sylikc/jpegview/issues/147)) is a platform-specific bug report to
  reproduce independently on Linux, not a feature to port as described.

## Existing Windows-parity gaps

These user-facing gaps are also listed in the [Linux README](README.md#known-windows-parity-gaps)
and remain possible candidates:

- **Free rotation and perspective correction:** interactive rotation and perspective/tilt-correction
  panels beyond the existing quarter-turn and mirror operations. An optional auto-level angle
  suggestion from line/horizon detection could complement this work; see
  [issue #252](https://github.com/sylikc/jpegview/issues/252).
- **Processing-parameter set exchange:** exchange a second parameter set for image comparison; this
  is distinct from the already implemented marked-image/toggle-back workflow.
- **Windows settings administration:** edit global/user Windows configuration files or update a
  user configuration from the global template. Any Linux version would need a suitable native
  settings design.
- **Open-With management and custom commands:** manually manage the Open With menu and add
  Windows-style user command definitions or Linux-native user scripts; see
  [issue #383](https://github.com/sylikc/jpegview/issues/383).
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

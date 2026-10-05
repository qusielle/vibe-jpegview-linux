# Linux feature candidates

This is an unprioritized list of Linux features that are not implemented yet, not a promise or
release plan. Items are kept separate where they can be implemented and reviewed independently.
Unimplemented archive formats remain separate candidates even if an implementation later shares a
backend. Archive-format candidates are independent of the JPEGView_L comparison below. When a
candidate is implemented, document its user-facing behavior in [`README.md`](README.md), update
[`ARCHITECTURE.md`](ARCHITECTURE.md) when relevant, and remove it from this list. Candidate sources
are identified by name.

## Candidates from [KrokusPokus/JPEGView_L](https://github.com/KrokusPokus/JPEGView_L)

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
- **Transparent title-bar presentation:** investigate a Linux-native equivalent to the branch's
  transparent title-bar panel. The Windows implementation uses DWM frame integration, so this is
  not a direct API port; Linux already supports hiding the window title bar.

## Candidates from [Masir01/jpegview_up](https://github.com/Masir01/jpegview_up)

Reviewed the repository's default [`dev-hw`](https://github.com/Masir01/jpegview_up/tree/dev-hw)
branch at `79a18f9` and also checked [`dev-up`](https://github.com/Masir01/jpegview_up/tree/dev-up)
at `93efb7a`, which was 24 commits ahead and contains newer decoder work. The bullets below are
gaps against Linux, not a recommendation to port Windows-specific code or dependencies verbatim.

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

## Candidates from other recently updated forks

Reviewed on 2026-09-26. I used the
[active-forks index](https://techgaun.github.io/active-forks/index.html#sylikc/jpegview) to find
repositories, ordered them by GitHub's last-push metadata, and started with the newest. I checked
default and non-default branch histories rather than treating a recent push timestamp as proof of
new feature work. Repositories already covered above—KrokusPokus/JPEGView_L,
andrewvladved/jpegview, and Masir01/jpegview_up—were skipped. These are feature ideas, not a
recommendation to port Windows-only code or dependencies verbatim.

### Workflow and curation ideas from [famomatic/jpegview](https://github.com/famomatic/jpegview)

Its `master` branch had recent feature commits through 2026-09-01. The fork's
[changelog](https://github.com/famomatic/jpegview/blob/master/CHANGELOG.txt) and feature commits
describe the following ideas:

- **Optional persistent thumbnail cache:** retain source-identity- and geometry-keyed thumbnails
  across sessions in a configurable cache location, allowing an SSD cache for HDD photo libraries.
  Define its quota, invalidation, clearing, and atomic publication before implementation. Keep disk
  reads and writes off the event thread. See the fork's
  [thumbnail-cache changelog](https://github.com/famomatic/jpegview/blob/master/CHANGELOG.txt).
- **Batch image conversion:** select multiple files and convert them to a chosen format, quality,
  and optional dimensions with progress and per-file errors. Linux currently has single-image
  saving and separate batch rename/copy tools. See the fork's
  [batch-conversion and metadata commit](https://github.com/famomatic/jpegview/commit/9cb44210ee8fa876e9de2cd19ef9a6974fb6ebf0).
- **Perceptual duplicate finder:** scan a chosen scope, group likely visual duplicates using a
  perceptual hash, and let the user review each group before taking action; never delete files
  automatically. See the fork's [duplicate-finder commit](https://github.com/famomatic/jpegview/commit/5d3c7a8b8932ad4d2212a7190eaaf9db5958e86a).
- **XMP ratings and culling:** read/write a 0–5 image rating (including a clear/unrated state),
  show it in the UI, and filter navigation by a minimum rating. Decide how JPEG-embedded XMP and
  sidecar files interact with saves and backups. See the fork's
  [rating commit](https://github.com/famomatic/jpegview/commit/654b22db28f1600e99df4b8e06a102b2158f59f1).
- **In-view type-to-jump search:** while the viewer has focus, accept an incremental filename
  query and cycle through prefix/substring matches without opening the file dialog. This differs
  from the existing open-dialog filter and the go-to-index candidate above. See the fork's
  [navigation feature commit](https://github.com/famomatic/jpegview/commit/654b22db28f1600e99df4b8e06a102b2158f59f1).
- **Favorite and recent folders:** add a quick-open list for pinned folders and recently visited
  directories. This complements, rather than replaces, the already-listed recently opened files
  dialog. See the fork's
  [workflow feature commit](https://github.com/famomatic/jpegview/commit/9cb44210ee8fa876e9de2cd19ef9a6974fb6ebf0).
- **View bookmarks:** let the user save and recall named zoom-plus-pan positions, useful for
  returning to details in a large image. Keep these separate from the existing marked-image
  navigation feature. See the fork's
  [bookmark implementation](https://github.com/famomatic/jpegview/blob/master/src/JPEGView/MainDlg.cpp)
  and [feature commit](https://github.com/famomatic/jpegview/commit/654b22db28f1600e99df4b8e06a102b2158f59f1).
- **Zoom/pan history:** add Back/Forward traversal through recent zoom/pan states, independently
  of bookmarks and file navigation, with clear rules for whether changing images creates a history
  entry. See the fork's
  [history feature commit](https://github.com/famomatic/jpegview/commit/9cb44210ee8fa876e9de2cd19ef9a6974fb6ebf0).
- **A/B comparison and difference overlay:** pin a second image, compare it with the current one,
  and optionally show an amplified heat-map of pixel differences. This extends the existing mark/
  toggle-back behavior without changing its quick-navigation semantics. See the fork's
  [A/B compare commit](https://github.com/famomatic/jpegview/commit/9cb44210ee8fa876e9de2cd19ef9a6974fb6ebf0)
  and [difference-overlay commit](https://github.com/famomatic/jpegview/commit/5d3c7a8b8932ad4d2212a7190eaaf9db5958e86a).
- **Smart crop suggestion:** detect likely uniform borders and propose a crop rectangle, leaving
  confirmation and final adjustment to the user so legitimate borders are not silently removed.
  See the fork's [smart-crop commit](https://github.com/famomatic/jpegview/commit/9cb44210ee8fa876e9de2cd19ef9a6974fb6ebf0).
- **Portable processing recipes:** import/export named picture-processing parameter sets as JSON
  for backup or transfer, separately from Linux's per-image parameter database and its whole-DB
  backup/restore. See the fork's
  [recipe commit](https://github.com/famomatic/jpegview/commit/5d3c7a8b8932ad4d2212a7190eaaf9db5958e86a).
- **Animated-frame extraction:** export every frame of a supported animated image as individual
  still files, with an explicit format and naming pattern. Linux can play the listed animations but
  currently saves a still image rather than offering a frame-export workflow. See the fork's
  [frame-extraction commit](https://github.com/famomatic/jpegview/commit/5d3c7a8b8932ad4d2212a7190eaaf9db5958e86a).
- **Copy and edit image metadata:** copy EXIF information to the clipboard and offer deliberate
  operations such as correcting orientation or removing GPS metadata. Preserve source files unless
  the user confirms a metadata write, and define whether unrelated metadata is retained. See the
  fork's [metadata commit](https://github.com/famomatic/jpegview/commit/9cb44210ee8fa876e9de2cd19ef9a6974fb6ebf0).
- **Dominant-color palette extraction:** calculate a small representative palette for the current
  image and let the user copy individual color values. This is a whole-image summary, distinct from
  the cursor pixel sampler already listed above. See the fork's
  [palette-extraction commit](https://github.com/famomatic/jpegview/commit/5d3c7a8b8932ad4d2212a7190eaaf9db5958e86a).
- **Histogram clipping warnings:** extend Linux's existing histogram overlay with clear shadow and
  highlight clipping indicators, without changing the image or its correction settings. See the
  fork's [histogram commit](https://github.com/famomatic/jpegview/commit/654b22db28f1600e99df4b8e06a102b2158f59f1).

### Format, file-list, and display ideas from [sdneon/jpegview](https://github.com/sdneon/jpegview)

The repository's main branch was pushed on 2026-08-21; I also inspected its non-default
`feature/encrypted-zip` branch. Its README notes that several behaviors are experimental.

- **SVG and SVGZ images:** rasterize vector files to the current view size, rather than always
  decoding at a fixed intrinsic size. SVG support is also present in
  [aviscaerulea/jpegview-nt](https://github.com/aviscaerulea/jpegview-nt), another recently
  updated fork. See [famomatic's supported-format list](https://github.com/famomatic/jpegview/blob/master/README.md).
- **PDF page browsing:** open a PDF as a multi-page document with page navigation, distinct from
  treating it as a folder image or comic archive. Keep rendering optional if the required PDF
  library is unavailable. See the
  [PDF support commit](https://github.com/sdneon/jpegview/commit/5d79cad3cc98d502576a8c357bffaea540ac67d6).
- **Minimum-file-size filter:** optionally skip tiny images such as embedded contact sheets or
  comic-folder cover thumbnails during navigation. Define the threshold and make it easy to disable
  for a directly opened image. See the fork's
  [file-filter documentation](https://github.com/sdneon/jpegview/blob/master/README.md).
- **Same-stem duplicate filtering:** optionally show only one file when multiple supported
  extensions share a basename, such as `photo.jpg` and `photo.png`. Keep this separate from
  perceptual duplicate detection, and leave it off by default because those files may be different
  edits. See the fork's [HideSameName setting](https://github.com/sdneon/jpegview/blob/master/README.md).
- **Animated-image frame controls:** step backward/forward through frames, freeze/resume, and adjust
  frame delay while viewing an animation. This is separate from the existing movie-mode FPS
  candidate, which advances between files. See the fork's
  [multi-frame navigation documentation](https://github.com/sdneon/jpegview/blob/master/README.md).

### Platform and performance ideas from other recently pushed branches

- **User-editable keymap:** load command-to-shortcut mappings from an XDG user configuration file,
  validate conflicts and unknown commands, and fall back safely to today's defaults. Linux currently
  hard-codes its main-viewer bindings. The [sdneon keymap notes](https://github.com/sdneon/jpegview/blob/master/README.md)
  and [TetraTheta's removal of hard-coded slideshow keys](https://github.com/TetraTheta/jpegview/commit/01afe3057ec763df359593e0d61877ec1da70697)
  show the user-customizable Windows behavior to adapt.
- **Inherit source JPEG quality on save:** offer an explicit `Auto`/inherit choice that estimates
  the source encoding quality and subsampling when re-saving JPEG, while preserving the normal manual
  quality control. Do not claim exact recovery where the source quantization cannot be mapped
  reliably. See [GlenXie920's JPEG-save change](https://github.com/GlenXie920/jpegview/commit/9a19d36e9680a2a3e275faef219932fe3490d26b).
- **Viewport/region decoding for very large TIFFs:** extend the oversized-JPEG candidate with lazy
  tile or region loading for huge TIFF/BigTIFF images, decoding newly exposed regions as the user
  pans instead of requiring a full-resolution bitmap. Keep cancellation/generation checks and
  preserve full-source semantics for edits and saving. See
  [famomatic's ultra-large TIFF work](https://github.com/famomatic/jpegview/commit/0a236812dc5c42e4fdeb4a9967c98c8810bcd7de)
  and its [general ROI-decoding commit](https://github.com/famomatic/jpegview/commit/5d3c7a8b8932ad4d2212a7190eaaf9db5958e86a).
- **Parallel progressive-JPEG decoding:** benchmark whether decoding independent color components
  and conversion work in parallel improves time-to-first-pixel on Linux's large progressive JPEGs.
  Treat the other fork's reported 40% as its result, not a Linux expectation; retain bit-exact output,
  cancellation, and the existing low-latency navigation path. See
  [Prit36's progressive JPEG commit](https://github.com/Prit36/jpegview/commit/3b290a8b25b41ecd97e92077e9ce4d02532cacd7).
- **Optional GPU image-processing backend:** benchmark GPU acceleration for expensive scaling and
  correction passes using a Linux-compatible API, while retaining the CPU implementation as the
  default/fallback. Include upload and synchronization costs in measurements; the other fork's
  optional D3D11 implementation is not a portable design to copy. See its
  [processing-backend overview](https://github.com/famomatic/jpegview/blob/master/README.md).
- **AArch64 Linux builds:** add an ARM64 build/test and packaging lane if the Ubuntu codec packages
  and AppImage path are available for the supported releases. This is an adaptation idea, not a
  direct port: [NK8998's branch](https://github.com/NK8998/jpegview/commit/7fde9ee450a61b4b461b2df7906d8714e37fea60)
  adds Windows ARM64 builds.
- **Per-monitor ICC output profile:** investigate applying the selected display's ICC profile after
  the existing embedded-image-to-sRGB transform, while keeping saved/clipboard pixels in a defined
  color space. The fork documents an opt-in monitor-profile transform in its
  [color-management commit](https://github.com/famomatic/jpegview/commit/654b22db28f1600e99df4b8e06a102b2158f59f1).
- **HDR monitor output:** separately explore rendering HDR sources to HDR-capable Linux displays,
  rather than only tone-mapping to SDR as in the existing AVIF/JXR candidate. This depends on the
  Linux window-system and renderer path and should remain optional with a tested SDR fallback. See
  the fork's [HDR display commit](https://github.com/famomatic/jpegview/commit/654b22db28f1600e99df4b8e06a102b2158f59f1).
- **OpenEXR/Radiance HDR images:** add optional decoding of `.exr`, `.hdr`, and `.pic` sources, with
  a documented SDR tone-mapping default and full-resolution save semantics. The same fork includes
  these as distinct formats in its [format documentation](https://github.com/famomatic/jpegview/blob/master/README.md).
- **32-bit PSD support:** the Linux PSD reader currently accepts 1-, 8-, and 16-bit inputs; add
  32-bit-per-channel PSD decoding if a compatible, bounded conversion path can preserve useful
  precision for viewing and saving. See the fork's
  [PSD format documentation](https://github.com/famomatic/jpegview/blob/master/README.md).

Repeated ideas from the reviewed forks are consolidated into one candidate entry rather than listed
multiple times.

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
- **Skip hidden images:** add an optional setting to omit hidden image files from navigation. On
  Linux, define this in terms of dotfiles (and decide explicitly whether `.hidden` directory
  metadata should also count), rather than copying Windows hidden-attribute behavior. See
  [issue #114](https://github.com/sylikc/jpegview/issues/114).
- **Quick rename of the current image:** add a one-file rename command and shortcut, initially
  selecting the basename but not the extension, with collision-safe behavior. This complements the
  existing batch rename/copy dialog; see [issue #280](https://github.com/sylikc/jpegview/issues/280).
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

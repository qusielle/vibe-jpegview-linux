# Linux frontend architecture

The SDL frontend deliberately keeps platform-independent behavior outside `main.cpp`. New logic
should normally be added to one of these focused modules and covered by `tests/test_core.cpp`:

- `file_list`: discovery, ordering, navigation modes, direct sibling-folder jumps, configurable
  folder-boundary wrap-around, current-file preservation, and the transient marked-image toggle pair
  used for A/B comparison. The marked path's index in the active ordered list is cached for
  constant-time thumbnail rendering. `IndexOf(path)` uses the same sorted path index for an exact
  optional result; unlike the internal selection lookup, a missing path never falls back to the last
  entry. Each entry also owns an immutable `SourceDescriptor` captured with its filesystem metadata
  during enumeration. `MutationRevision()` changes when membership or ordering changes, while
  `DescriptorRevision()` changes when source identity or metadata is refreshed; ordinary current-image
  navigation changes neither. Filename sort data is folded once per entry and the natural comparator
  consumes that cached key, retaining existing numeric, case, leading-zero, and path-tie behavior.
  Ordinary-file identity, size, modification time, and optional birth time are requested together with
  `statx`; when that request is unavailable or omits required fields, the stat fallback preserves
  modification-time creation ordering.
- `file_list_scan_worker`: one lazy, low-priority worker for viewer-list initialization, reloads,
  recursive/sibling transitions, dropped inputs, cross-folder marked-image toggles, and
  multiple-input scope changes. Requests carry
  immutable input/settings snapshots with list and descriptor revisions; the worker owns
  enumeration, archive catalog reads, file metadata, sorting, and replacement-list construction. It
  checks a generation cancellation predicate while traversing and publishes only the newest complete
  result. The SDL thread applies it only after checking generation, membership/order revision,
  descriptor revision, and navigation source. A source refresh invalidates in-flight prepared scans even when
  the active sort order and list membership stay unchanged; dropped-input replacement uses the same
  acceptance check and retries its original request after a stale result.
  While foreground image or spread work is pending, enumeration pauses at the existing continuation
  checkpoints and resumes the same scan when foreground work clears; the active list is never rebuilt or
  replaced early. A foreground yield reported from a nested archive callback is retained by the scan
  owner even if the coordinator gate clears before the scan returns. The worker releases archive
  admissions before waiting for the shared foreground gate; replacement, clear, and shutdown wake
  that wait. Initial input classification (including archive-container, directory, and regular-file
  probes) also uses cancellable source admission, then releases it before catalog reads or paired
  descriptor capture.
  The active `FileList` remains main-thread-owned, so ordinary next/previous steps within its loaded
  entries stay lock-free and do not copy the list. A compact path-sorted vector of entry indices
  preserves logarithmic path selection regardless of the active metadata sort, without duplicating
  each path string. A direct image launch uses a provisional one-image list so decoding can start
  before its folder scan completes. Cancellation never blocks waiting for the worker; destruction
  joins it during normal owner teardown.
- `file_list_sort_worker`: a separate foreground CPU-admitted worker for reordering an already loaded
  catalog. `FileList` shares its immutable entry storage with the request; the worker copies and sorts
  off-thread, then publishes a complete order and path index. The active order stays usable until the
  matching result applies. Application checks catalog and descriptor revisions, then restores the
  source selected at apply time and the marked source by logical path. A descriptor refresh that
  changes the active metadata sort key uses this same asynchronous path. Superseded results and
  replaced catalog vectors move to worker retirement so their large path and metadata buffers are not
  freed during renderer-thread work.
- `source_work_coordinator` and `work_context`: shared admission across otherwise independent
  worker pools. Foreground source work has a separate lane; speculative and metadata reads share one
  active lane and new background source admission waits while foreground work is pending. A partner
  in the currently displayed spread uses foreground admission even when its cache request originated
  as background work. Admission identity is separate from cache/source-version identity: valid
  descriptors match by backing device and inode, while size and modification time remain part of
  source freshness and cache keys. If either side lacks a valid identity, admission falls back to the
  lexically normalized absolute backing path; two known, different device/inode pairs remain distinct
  even if a stale path matches. Context construction and admission-key resolution do not stat sources
  before admission, and archive members serialize on their container even when decoders use temporary
  extracted paths. Ordinary-file
  validation holds a cancellable source-only lease; archive validation holds the source/CPU pair across
  both the container stat and member metadata lookup. Scoped admission flags let nested metadata calls
  reuse that pair. One shared CPU semaphore caps
  active processing across pools at the hardware-aware limit (no more than four). RAII leases release
  source and CPU permits on every return, exception, and cancellation. Work needing both resources is
  admitted atomically, so a source lane is never held while waiting for a CPU permit; already-admitted
  contexts must carry both leases together. Waiting work can report live foreground promotion, which
  moves its source and CPU queue positions without holding coordinator locks during owner callbacks.
  Continuation and priority callbacks run outside coordinator locks; nested decoder/helper calls reuse
  the scoped context rather than admitting the same source twice. Archive/catalog cancellation
  callbacks return immediately when foreground work arrives, allowing paired source and CPU leases to
  unwind before the outer worker waits; interrupted workers discard partial results and retry after
  the foreground gate clears. Other metadata workers yield at entries or bounded batches, and canceled
  waiters remove themselves promptly from admission queues. Work carries the observed interruption
  reason across gate changes, so an interrupted traversal retries even if foreground work completes
  before its post-scan check; shutdown wakes coordinator waits.
- `double_page_model`: portrait-pair eligibility, cover handling, aspect-preserving shared-height
  spread geometry, whole-spread quarter-turn placement, page-step navigation, and configurable
  physical-key direction in manga reading order. It owns no image pixels, filesystem work, or SDL
  resources.
- `image_session_controller`: the selected source ticket, monotonically advancing load generation,
  document revision, processing snapshot, and viewport restoration choice. It owns the existing
  `RecentImageLoadState` and temporary clipboard-return viewport while keeping the last committed
  Recents owner distinct from a pending selection. Its selected-load stages cover asynchronous
  JPEG-header or full-source decode, display preparation, renderer-ready upload, commit, and failure.
  A selected-decode mailbox accepts only the active source key and load generation, including
  A-to-B-to-A reversals. Snapshot decisions suppress duplicate decoded work while an exact display
  frame, spread deferral, or header request owns preparation. Viewer commits selected history only
  after the matching prepared frame becomes a renderer texture; failure leaves the previous committed
  history owner intact. It returns value effects for clearing the prior presentation, restoring the
  captured viewport, and preparing the selected source; Viewer applies those effects through its SDL
  and cache adapters.
- `display_preparation_controller`: the display-prefetch planner lifetime, viewport and request-batch
  generations, and a bounded channel of owned `DisplayImageRequest` completions. Worker callbacks
  retain the channel and captured generation, never a raw display-cache pointer. Viewer drains the
  channel on the event thread and submits requests to `DisplayImageCache`; explicit shutdown closes
  the channel and joins the planner before renderer and cache teardown.
- `presentation_controller`: presentation and held-navigation readiness layered over
  `DoublePagePresentationModel`. It accepts selected-load, JPEG-header, spread, texture-readiness, and
  first-frame acknowledgement values; it also plans single-page fallback, dimension waits, rotated
  spread geometry, and pair-request admission from value snapshots. It owns no SDL resources. The
  renderer acknowledges the spread only after `SDL_RenderPresent` returns.
- `archive_source` and its internal modules: `archive_source.cpp` owns the public generic
  container/member API, source identity, session-password lifecycle, and bounded temporary-member
  extraction. `archive_source_catalog.cpp` owns catalog-cache/coalescing policy, while
  `archive_source_readers.cpp` owns ZIP/libarchive traversal and the optional 7-Zip/RAR reader
  boundaries. The private `archive_source_internal.h` shares catalog and location records without
  exposing format-specific readers to navigation or preview consumers. Temporary extracted members
  remain owned by `PrivateMemberFile`, whose destructor removes the link and closes the descriptor.
  ZIP and CBZ containers share the central-directory reader and member
  access path; CBZ retains its own display label. 7z and CB7 containers share libarchive's seekable
  reader and member path; CB7 retains its own display label. TAR/TGZ catalogs stream header metadata.
  Encrypted 7z and CB7 use the focused `seven_zip_backend` adapter described below. Ordinary
  unencrypted RAR catalogs and extraction remain on libarchive; when present, the optional
  `rar_backend` probes RAR metadata and handles encrypted RAR4/RAR5 catalogs and extraction.
  Immutable catalogs for at most four containers
  are keyed by device/inode/size/mtime and retain no extracted image payloads. Workers use independent
  libzip/libarchive handles, validate the selected member's identity, and stream at most 128 MiB of
  uncompressed data into an anonymous memory file for existing path-based decoders. Unsafe paths,
  archive links/devices are omitted; encrypted ZIP, data-encrypted 7z, and data-encrypted RAR retain
  their names and locked state. Header-encrypted 7z/RAR return a password-needed catalog result
  without exposing hidden names.
  Catalogs over 100,000 entries are rejected.
  `SourceIdentity` records backing-file device, inode, size, and stat modification seconds/nanoseconds;
  `SourceKey` adds the raw logical path bytes, keeping archive members distinct while sharing their
  container identity. `SourceDescriptor` carries available sort and display metadata as well. Archive
  member size is the uncompressed size. Modification and creation metadata for regular files and
  archive members use Unix epoch nanoseconds; only adapters to `file_time_type` convert to the
  filesystem clock domain. The backing identity retains Unix stat seconds and nanoseconds. Failed
  stats produce an invalid identity that is distinct from every valid cache key.
  New archive formats should extend this backend dispatch while keeping viewer consumers
  on the generic source operations. Catalog/listing and member-extraction tasks acquire the shared
  source lane using the backing container identity. Extraction carries its original archive member
  context into temporary-file decoding, so nested codec calls do not admit the temp path as a new
  source or lose cancellation for the archive read.
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
- `image_document`: lazy current-image metadata plus immutable, shared source and presentation pixels,
  owner/source identity, document revision, frame identity, effective processing snapshots, edit state,
  and transfer of replaced pixel ownership to retirement. No-op processing aliases the source buffer
  instead of copying a second full-resolution image.
- `image_operation_worker`: cancellable CPU-admitted transform, crop, resize, materialize, reprocess,
  selection-copy, and output-size preparation requests. Each request captures an `ImageDocumentSnapshot`;
  only a matching successful revision can be applied. Viewer creates any replacement SDL texture before
  swapping document pixels, so failure or staleness preserves the last successful presentation. Large
  result and decoded-buffer references retire off the event thread.
- `crop_selection_model`: source-image crop bounds, free/aspect/fixed-size selection geometry,
  move/resize hit testing, image/view coordinate conversion, crop-mode drag eligibility, and JPEG
  MCU-boundary alignment; pixel-buffer cropping remains in `image`.
- `crop_size_dialog_model`: fixed-crop dimension text, focus/unit transitions, and validation.
- `image_processing` and `image_processing_store`: bounded adjustment ranges, parameter identity,
  pixel processing, the atomic native per-image levels database, and its portable backup/restore.
- `image_decoder`, `image_writer`, and `image_formats`: codec boundaries and format policy.
  `image_decoder.cpp` owns validation, append/conversion helpers, source admission, and codec dispatch;
  `image_decoder_stb.cpp`, `image_decoder_jpeg.cpp`, `image_decoder_apng.cpp`,
  `image_decoder_builtin.cpp`, and `image_decoder_optional.cpp` isolate the corresponding reader
  families behind the shared internal interface. `image_writer.cpp` owns validation and dispatch;
  `image_writer_jpeg_png.cpp`, `image_writer_basic.cpp`, and `image_writer_optional.cpp` hold codec
  implementations. Decoder-owned pixel buffers move into `DecodedImage` when the decoder has finished
  using them. Mapped JPEG inputs and non-longjmp codec handles use scope ownership; libjpeg/libpng
  retain explicit cleanup at their `setjmp` recovery points because those C APIs report failures with
  `longjmp`. Image decoders resolve archive-member paths through `archive_source` before invoking the existing codec
  path, retaining ordinary-file and reduced-DCT JPEG behavior. JPEG cancellation is checked before
  opening, after header parsing, between 16-row scanline batches, before color conversion/resampling,
  and before publication; opaque codec calls are checked before and after their supported boundaries.
  callback exception boundaries remain in place, and callbacks are never thrown across C codec frames.
  Decoded frames carry alpha-presence metadata so opaque-image textures can keep blending
  disabled. If giflib is unavailable, the built-in `stb_image` path still returns a GIF's first frame;
  animation delays and compositing require the giflib decoder.
- `image.cpp` keeps the scalar resize algorithm as the pixel oracle while caching complete resampling
  kernel sets in a shared 8 MiB LRU keyed by source axis, target axis, and filter. Kernel construction
  and eviction destruction happen outside the cache lock, incomplete/canceled kernels are not retained,
  and unchanged axes skip their scratch buffer and resampling pass. Color-cast response values are
  computed once per channel value before the pixel loop. `thumbnail_resampler` retains its transparent
  path and selects an opaque path only when source metadata proves every alpha sample is opaque; both
  paths preserve the characterized output bytes.
- `cache_budget`, `image_cache`, `display_image_cache`, `display_prefetch_planner`,
  `display_preparation_controller`, and
  `display_upload_scheduler`: aggregate cache accounting,
  source-aware decoded-image retention, nearest-first decode completion, and threaded picture-level
  processing/scaling of renderer-ready frames. Decoded, display, and thumbnail caches use the same
  structured `SourceKey` equality and hashing. Display keys add frame, effective processing controls,
  target geometry, and quarter-turn orientation, so disabled controls normalize away while an active
  adjustment, animation frame, or spread rotation cannot reuse different pixels. Renderer lookups use
  the `SourceDescriptor` already captured by the active `FileList`; they do not stat the path on each
  render or pan. Workers validate the descriptor before opening and before publishing. A stale worker
  result is discarded and emits a notice containing its exact previous key plus the observed
  descriptor, including an invalid descriptor when the source is missing. The SDL owner applies the
  notice only if the active file list still contains that exact previous key, then rebuilds requests
  from the refreshed descriptor. If the selected source changed, Viewer reloads it before the new
  descriptor can own current pixels. A noncurrent refresh retires affected display-prefetch and
  double-page state; changed partner dimensions or modified-time reordering rebuild the visible
  spread while keeping the selected path anchored. An accepted whole-list replacement retires all
  index-bound prefetch and spread state before rebuilding against the new order, while preserving the
  selected path and current pixels. Explicit reload and application-owned writes use the same refresh
  path.
  Rotated spread partners are oriented on the worker before final slot-size resampling; unrotated
  image requests retain the same processing path. JPEG display requests use native reduced DCT
  decode with swapped target axes for quarter-turns before exact scaling, without requiring a
  retained full-resolution source frame. Selected display requests clamp their preparation target to
  the effective source dimensions before key lookup. The renderer destination remains viewport-sized,
  so enlargement scales the source-resolution texture and pan changes only its position; neither needs
  another prepared frame. A larger cached representation may serve a smaller request only when source,
  frame, orientation, histogram, and effective processing keys match; the planner protects that
  retained texture instead of queueing a duplicate fitted frame. Rotated double-page partners clamp
  against the source dimensions after quarter-turn orientation, and spread admission estimates
  canonical prepared requests. If a retained higher-resolution anchor makes the pair exceed the
  shared budget, the viewer retries admission with the fitted anchor request before selecting the
  single-page fallback. Neighbor
  planning uses a separate fitted snapshot, preserving normal fitted frames while the selected image
  is in actual-size, manual, or fit-relative zoom. The planner caps its
  source-probe window at 512 candidates, sums each fitted request's estimated frame bytes, and stops
  admission at the shared cache capacity after reserving the current source-resolution image. Window
  and thumbnail-panel resizing update geometry immediately but hold neighbor rebuilding until the
  existing interaction quiet period expires. Active display-work diagnostics use each in-flight request's
  current priority and effective work class, so a promoted neighbor keeps its active-spread upload
  eligibility across an empty prefetch cancellation. Explicit display-work cancellation retires the
  active background class as well as its desired-key membership, so empty prefetch preserves only
  still-desired spread requests and cannot revive a retired partner. Decoded-image promotions
  likewise use the effective class for cache admission; header-only JPEG dimension requests use
  the decoded worker without retaining full-resolution pixels. The viewer captures a bounded
  neighbor window with catalog, descriptor, current-index, direction, fitted-prefetch viewport, and
  per-file processing snapshots. `display_prefetch_planner` probes missing JPEG dimensions and builds
  requests on a worker; the SDL owner applies results only when those captured revisions and viewport
  still match. Pan and replacement cancel obsolete planner work. Viewport invalidation retains the
  publication batch for a still-current active-spread dimensions read, while source or mode
  replacement deactivates that request's batch. A decoded insertion that cannot fit prunes queued
  speculation only; foreground and active-spread work continues draining, including metadata-only
  partner requests. Neighbor-batch bookkeeping releases decoded aliases after the display cache
  accepts a request; an active-spread batch may retain its required decoded partner fallback.
  Active-spread queued and active requests count as foreground source demand so
  visible thumbnails and list scans yield until the partner is ready.
  Speculative display preparation reserves completion capacity before processing: at most two
  in-flight or unconsumed speculative frames and 64 MiB of predicted pixels. Active-image and
  active-spread work bypass those speculative limits. Oversized speculative requests are skipped at
  admission, and workers wait when a valid speculative result cannot fit; cached speculative
  completions wait as metadata until a slot is free. Consuming, promoting, canceling, or rejecting a
  result releases its reservation. Thus a paused renderer cannot let completed speculative pixels
  grow with the neighbor window.
  Borrowed-image entries persist whenever needed to carry a temporary reservation through upload;
  performance tracing also tracks borrowed images without a reservation for byte diagnostics.
  Retirement deduplication uses the pending queue and active worker owner; the
  worker keeps each active frame alive until all external pixel handles are released, then performs
  the final destruction itself. Retired byte totals come from queue and active-worker metadata, so
  diagnostics never acquire a temporary pixel owner. Shutdown stops preparation workers before
  retirement; it joins after draining when no external handles remain, and returns while the shared
  retirement state finishes later when a valid handle survives.
- Worker source reads and CPU processing across decoded-image, display, thumbnail, dimension,
  EXIF, file-list, Browse, and archive tasks use the shared coordinator described above. Cache workers
  catch exceptions from injected decoder/processor callbacks, retain a structured last failure in
  diagnostics, release RAII admission permits, and continue servicing later requests. The EXIF worker
  publishes a terminal, metadata-cleared failure for a current-generation reader, validator, or
  admission exception; cancellation and replaced generations remain unpublished. JPEG and supported
  archive loops observe cancellation at bounded checkpoints; a call already blocked in filesystem I/O
  or an opaque codec may finish before its owner discards the result.
- `input_commands`: SDL key chords to shared JPEGView command IDs, the configurable Space/Shift+Space
  image-navigation direction, and held-navigation repeat state (including when Shift is permitted).
- `desktop_association`: user-local desktop entry generation and atomic XDG MIME default updates.
- `transparency_pattern`: accepted background setting values and checkerboard tile colors.
- `settings` and `sort_mode`: persisted configuration (including the transparent-image background
  choice, default picture-level values,
  fixed crop dimensions/units, user crop aspect, the explicit crop-selection mode (disabled by
  default), zoom-navigator visibility, magnifying-glass size/zoom, the validated window-title
  pattern, global double-page/manga-mode
  defaults, the default-enabled `manga_mode_inverts_left_right` preference, the default-disabled
  `spacebar_navigates_images` preference, the default-enabled `folder_wrap_around` preference, and
  the default-disabled `fit_relative_zoom_mode` preference),
  plus stable sort-mode values. Applying prepared list scans preserves the live folder-wrap value,
  so editing the setting does not require canceling or repeating a directory scan.
- `advanced_configuration_model`: category and row metadata for persisted settings that lack a normal
  context-menu command, including the Behavior category's fit-relative zoom option, with a transient
  `ViewerSettings` draft, value bounds, text editing, and
  selection/scroll transitions. It performs no file I/O; Apply is routed through the existing settings
  serializer by the SDL adapter, while Cancel drops the draft.
- `recent_files`: normalized absolute MRU image rows with one image per parent folder, a separately
  bounded per-file `ViewportSnapshot` LRU (absolute and fit-relative zoom) and independent bounded
  double-page/manga-mode snapshots,
  ordered row removal/restoration for the Recents dialog, plus tolerant atomic persistence in the
  XDG state directory. Virtual archive-member paths remain logical recent identities while source
  validation and cache freshness use the backing container. The recent database is independent from
  viewer settings and performs no image or directory scans while loading. `RecentImageLoadState`
  tracks the selected pending path and its viewport separately from the last successfully loaded
  history owner. It commits the Recents row and loaded-path ownership only after the selected frame
  is uploaded and accepted by the renderer; `LoadCurrent` only starts asynchronous preparation. The
  outgoing owner's viewport and D/J modes are saved once while that owner remains
  committed, so a cold-image continuation or replacement cannot overwrite them with the pending
  image's restored modes. Direct snapshot writes require the selected path to remain the committed
  owner. Saved per-image viewport snapshots are restored only for explicit Recents-tab opens;
  ordinary previous/next navigation carries the current fit/zoom/actual-size mode and scale even
  when the target has an older snapshot. Double-page and manga-mode restoration remains a separate
  policy. Viewport restoration resolves the selected identity before retiring its pending request:
  a matching pending selection keeps its captured snapshot, while an ordinary reversal uses the
  current navigation snapshot; only an explicit Recents open reads the selected path's saved view.
  Viewport mode changes update the pending snapshot so a selected-image
  continuation applies the latest user intent. Viewport and rotate/mirror actions share one
  source- and generation-bound sequence and replay from the incoming snapshot after the required
  dimensions or pixels arrive, so unknown geometry never substitutes the outgoing image's scale.
  The queue admits at most 256 actions; at capacity it rejects later actions without altering
  accepted state and exposes that limit in the pending title. Startup catalog completion keeps a
  matching provisional cold-header request and its
  queued input when selected path and source identity still match. Header continuation preserves the
  effective picture-level processing selected while the request was pending. While matching header,
  decode, or display-frame work is pending, generic title refreshes retain its loading status. A
  successful renderer commit starts snapshot saving for the new owner; canceling or replacing selected
  work drops only its pending snapshot and intents.
- `pending_image_intents`: ordered viewport and rotate/mirror actions plus slideshow-transition
  requests attached to the active selected-source generation across header, decode, and display
  stages, with a 256-action cap and
  deterministic rejection of overflow, plus deferred EXIF-date actions attached to the
  metadata generation. Header and metadata work can finish in either order; an EXIF-date update waits
  for both matching metadata and successful image commit so it cannot invalidate an in-flight header
  read by changing the source modification time.
- `viewport`: fit/fill/manual zoom modes, optional fit-relative scale base and
  preset/step/snap calculations, source-pixel plus fit-relative zoom readout formatting, relative
  navigation snapshots, a fitted prefetch snapshot independent of the selected image's transient zoom,
  pan state, destination geometry, and panning bounds that keep the viewport inside the image.
  Fit-relative calculations follow the `ZoomMath.cpp` model from
  [andrewvladved/jpegview](https://github.com/andrewvladved/jpegview/blob/9dcd25766585be5fb65ca6a26c0a6beb46346112/src/JPEGView/ZoomMath.cpp);
  the mode remains disabled by default.
- `zoom_navigator_model`: responsive overview geometry, visible-image mapping, pointer conversion,
  and click/drag pan calculations for the transient zoom navigator.
- `magnifying_glass_model`: lens enable/size/zoom state and bounds, wheel-modifier transitions,
  and pointer-centered mapping from a clipped image source crop into lens content geometry.
- `resize_model`: resize-dialog values, aspect-ratio coupling, limits, filter selection, and pure
  focus/text-editing transitions.
- `context_menu_model`: the complete menu catalog, state-derived enablement/checkmarks,
  compact/advanced filtering, actionable-item keyboard navigation, and deterministic letter
  mnemonic assignment with duplicate-letter matching/cycling. Mnemonics are assigned to the full
  catalog before compact filtering so commands shared by both views keep the same letter and
  underline position; the compact-only “Show Advanced Options” row is assigned afterward.
- `playback_scheduler`: wrap-safe animation, movie, and slideshow timing expressed as Viewer actions.
- `file_dialog_model`: filename filtering in Browse and full-path filtering in Recents, name/date
  sorting, UTF-8 editing, selection, paging, independently
  clamped viewport scrolling, proportional scrollbar thumb geometry and row-offset mapping, focus
  restoration, pane-aware preview image sizing and testable filename/details footer allocation,
  one authoritative entry collection with exact path lookup and filtered/sorted row indices,
  cancellable background filesystem/archive listings with both row orders prepared on the worker,
  and directory summaries (including supported archive containers in the directory count), bounded
  background source-metadata lookup for Browse and Recents rows, encrypted-row marking,
  caller-preserved row order for recent MRU entries, and replaceable previews for a focused image or
  a directory's first image.
  Preview results carry original source dimensions and byte size; archive-member sizes come from
  uncompressed member metadata. Filesystem row sizes and modification times are captured during
  directory enumeration. Descriptor/catalog capture for Browse and Recents stays on the file-size
  and preview workers; event-thread row rebuilds use path placeholders rather than opening cold
  archive catalogs. File-size work starts with the selected and visible rows and completion delivery
  is capped per UI update. Each preview request carries its requested descriptor and validates a valid
  identity before and after reading; a stale completion returns both the requested and observed
  descriptors for exact refresh. The SDL owner refreshes and invalidates only when their `SourceKey`s
  differ; an unchanged invalid key keeps its terminal missing-source error without resubmitting it.
  Filesystem directory scans and focused-preview enumeration admit bounded iterator/status batches,
  including empty-directory iterator opens, and release source-only leases before paired descriptor or
  archive work. Partial results are discarded and retried after a foreground yield.
  Replacing queued or ready previews records cancellation on the event
  thread; a stale active result records cancellation on its worker, once per discarded task.
  Image-dimension and archive-member size lookups stay off the SDL event thread.
- `overlay_layout`: content-sized filename/EXIF panel geometry and window clamping.
- `viewer_chrome`: renderer-independent overlay and navigation-panel paint plans, including icon
  primitives, hit regions, fit-relative scale labels, dynamic labels, and tooltip placement.
- `thumbnail_panel_model` and `thumbnail_resampler`: strip geometry and current/marked row state,
  nearest-first cache scheduling from a catalog refreshed when membership, order, source identity,
  or descriptor metadata changes. The viewer pairs each `FileList::MutationRevision()` and
  `FileList::DescriptorRevision()` with a monotonically increasing owner revision whenever it
  replaces the whole `FileList`, including dropped inputs and clipboard transitions.
  Applying a prepared background sort advances the mutation revision again, so a thumbnail catalog
  refreshed against the old order while sorting is pending cannot remain marked current.
  `SetCurrent` updates priority without visiting a full catalog; finite cache capacities select a
  nearest-first working window, while the viewer's full-list capacity keeps every active source
  eligible. `TakeNext` emits a bounded batch, and evictions outside the working window are not
  requeued until navigation or capacity makes them eligible. Navigation keeps useful in-flight work
  valid. Completion identities carry the source index, catalog revision, and target-geometry
  revision, so removed or reordered entries and old-size pixels cannot be published through a stale
  index. Thumbnail cache, queued work, failures, and results are keyed by `SourceKey`; catalog updates
  evict only identities no longer present. The viewer passes each entry's captured descriptor to
  cache planning, and source changes or incompatible geometry invalidate old thumbnail pixels while
  navigation and sorting keep unchanged ones. Renderer-texture invalidation erases by the exact
  structured keys returned by the scheduler, so bulk list replacement performs one lookup per
  evicted identity rather than rescanning the full texture map for each removed path.
  cancellation/LRU and decode-failure policy, memory sizing, alpha-preserving antialiased
  source-area reduction, and low-priority thumbnail preparation from either completed neighbor
  display frames or file-backed requests. One worker performs file reads, JPEG dimension lookup,
  reduced-DCT JPEG or ordinary image decode, and resampling; its prepared-frame reuse queue remains
  bounded to two queued requests. Completion publication reserves capacity before preparation and
  is capped at two in-flight or unconsumed results and 16 MiB of predicted pixels; invalid or
  oversized geometry is rejected at request admission. The worker waits for renderer consumption or
  cancellation to release a reservation, while canceled and uploaded thumbnail buffers move to a
  worker-owned retirement queue. Shutdown drains that queue without event-loop activity, or leaves
  its shared state running while an external pixel handle remains. When a prepared-frame request
  displaces farther queued file work,
  the admission returns that file request's identity to the scheduler for retry. `TickThumbnailPreload`
  only admits requests, consumes one completion
  at a time, validates the catalog revision, file index, source key, and target geometry, and uploads
  textures on the renderer thread. Failed decodes are tracked separately from cached
  pixels, while failed texture uploads retain their prepared pixels for retry. The viewer supplies
  the active double-page partner index so both displayed spread pages receive active-row styling
  without moving the panel's centering or changing which row represents the navigation index.
  Thumbnail diagnostics include unique display-source bytes held by queued and active requests.
  Work intersecting the strip uses `visible_thumbnail`; offscreen retained rows use
  `distant_speculation`. The selected class follows requests through source reads, decode,
  resampling, worker source-pixel reuse, and renderer upload.
- `interaction_work_policy`: pure foreground-demand aggregation and monotonic-clock policy that maps
  pan, zoom, resize, crop/navigator
  drag, held navigation, wheel activity, capture state, foreground demand, and visible thumbnail indices
  to permitted work classes. It holds distant work for 250 ms after the last activity or capture release,
  keeps active image/spread work eligible, and permits visible thumbnails only when they cannot compete
  with pending foreground source work.
- `work_batch_gate`: serializes completion publication with batch deactivation. Once an owner deactivates
  a prefetch batch, callbacks already copied by a worker cannot enqueue new work after cache cancellation.
- `display_texture_pins`: pure pin selection and scoped borrowed-texture handoff. A borrowed outgoing
  display texture receives a temporary capture pin before an old transition is cleared, then the new
  transition takes its pin before that temporary pin is released. All SDL texture operations remain on
  the renderer thread.
- `renderer_thread_resource`: move-only renderer-thread handles for the SDL window and renderer,
  with explicit renderer-before-window reset ordering.
- `renderer_texture_owner`: the renderer-thread owner for image, display, thumbnail, preview, and
  partial-upload textures created by Viewer. It performs image texture creation, upload, and
  destruction and provides a final live-handle drain before SDL renderer teardown. Cache maps keep
  their keys, admission reservations, and non-owning texture references; cache policy remains in the
  existing cache owners.
- `text_renderer`, `chrome_renderer`, `context_menu_renderer`, `file_dialog_renderer`, and
  `editing_dialog_renderer`: SDL drawing adapters that consume prepared paint snapshots and share
  narrow renderer/font services. Viewer still decides when to draw and builds snapshots from its
  models; the adapters own their renderer-facing helpers and do not become alternate state owners.
- `image_info_model`: stable image-position, dimensions, date, and file-size presentation, plus
  validation and single-pass expansion for the configurable window-title pattern. It caches title
  and information-line formatting by the relevant source, catalog, metadata, and display state, and
  tracks the last title sent to SDL. Archive-member titles and overlays use the uncompressed member
  size carried by the captured source descriptor, not the backing container size.
- `exif_metadata_worker`: cancellable, generation-checked optional EXIF/comment reads. Results carry
  the captured `SourceKey`; Viewer applies metadata only when both source and request generation
  still match, so slow metadata cannot delay initial image presentation or overwrite another image.
  Filesystem/archive freshness checks, including the final pre-publication check, run outside the
  mutex shared with event-facing requests, cancellation, and result polling. Archive-member reads
  retain foreground-yield reasons from nested archive callbacks and reader checkpoints, release
  paired source/CPU admission before waiting, then retry the same still-current generation after
  foreground work clears. Replacement, cancellation, and shutdown wake that wait; genuine
  source-unavailable and processing failures remain terminal results.
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
- `external_process`: structured `posix_spawnp` execution, atomically close-on-exec pipes, prepared
  spawn file actions, process-group cancellation and reaping, bounded clipboard output, and
  detached-child launch. It does not run allocation-heavy application code after `fork`; scoped
  pipe and child ownership also closes and reaps resources if output buffering fails. The clipboard
  writer's owner child remains alive after its encoded input has been delivered.
- `file_operation_service`: one worker for immutable save, clipboard, batch, lossless JPEG, print,
  wallpaper, timestamp, trash, registration, and desktop-launch requests. Requests never wait for
  filesystem or child-process work on the SDL thread; each completion carries an operation ID and
  captured owner generation and is applied by Viewer only after the matching request is checked.
  Batch cancellation stops at file boundaries and reports completed changes without rollback. Each
  copy is prepared in a mode-0600, non-image `.tmp` sibling and published without replacing a
  destination created after its existence check, so readers see only complete files; failed and
  canceled copies leave no partial image behind. Source permissions and modification times are
  restored before publication. Lossless JPEG results use the same private `.tmp` sibling policy;
  newly absent destinations use atomic no-replace publication, while replacing an existing
  destination requires confirmation and uses atomic rename after the codec succeeds and permissions
  are restored. In-progress outputs stay hidden from image scans. Encoded saves, print preparation,
  and wallpaper-cache images also use private non-image siblings and atomically publish after format
  encoding succeeds; the writer receives the selected output format separately from the staging
  filename. New save targets use atomic no-replace publication, while confirmed existing targets
  retain their prior mode and use atomic rename. Symlink save destinations continue to address their
  resolved target. Once printing,
  trash, or wallpaper commands have launched an irreversible side effect, ordinary request
  cancellation no longer terminates them; the worker waits for exit and publishes the actual success
  or failure without relabeling that completed command as canceled.
  Source reads share `SourceWorkCoordinator`; source-plus-CPU work is admitted as a pair, and scoped
  context lets nested EXIF readers reuse that admission. Clipboard PNG encoding uses a source+CPU
  pair; its subsequent temporary-file read uses source admission only. Both permits are released
  before waiting on the clipboard helper. Pasted bytes are capped before a temporary image is
  published. Printing releases its CPU permit after PNG encoding, then retains source admission while
  the printer reads the prepared file. Temporary-image cleanup uses source admission for the same
  file path, so it waits for an active reader before removing the file and its private directory;
  unpublished operation outputs use scoped cleanup on every exit.
- `exif_reader`: JPEG metadata parsing.
- `event_loop_model`: renderer-thread frame invalidation reasons, wrapping SDL tick-deadline
  selection, adjacent pointer-motion accumulation, and a process-wide coalesced completion wake.
  Workers publish their owned payloads to their normal generation-checked queues before notifying;
  the SDL user event carries no payload. `SDL_WaitEventTimeout` sleeps until input or the next
  playback, transition, overlay, held-navigation, retry, or interaction-idle deadline, with a 100 ms
  fallback that drains queues if SDL rejects a wake. An eligible in-progress display-texture upload
  adds an 8 ms continuation deadline between bounded bands; uploads waiting on cache admission do
  not poll and resume when capacity or completion work wakes the event loop. Drawing uses
  non-touching display-cache peeks
  and does not schedule work, apply completions, update readiness, or touch display/thumbnail cache
  recency. Timed effects and visible-thumbnail LRU touches run in the event/update phase. A clean
  static frame therefore leaves the renderer idle until an invalidation or deadline requires another
  frame. Opt-in periodic cache and queue snapshots are sampled from the update phase, independently
  of drawing.
- `perf_diagnostics`: opt-in monotonic timing and cache/queue snapshots. It is inactive unless
  `JPEGVIEW_PERF_TRACE` names a CSV output file; enabled samples enter a fixed-capacity ring and a
  writer thread handles formatting and file I/O. A stack-only `PerfContextScope` carries one of five
  resource-work classes plus event-thread/worker-thread execution through existing synchronous and
  worker paths; generic event, frame, presentation, renderer, and snapshot rows use `unspecified`.
  Source-read and cancellation rows inherit that context, and rows include an opaque numeric thread
  identifier. Direct source readers aggregate callback I/O duration and actual bytes returned for
  stb, giflib, libtiff, libheif, libavif, and LibRaw. LibRaw totals omit legacy compressed-DNG JPEG
  and JasPer handoffs that read through codec-owned streams; successful handoffs receive explicit
  zero-duration `libraw_jpeg_handoff_unmeasured` or `libraw_jasper_handoff_unmeasured` rows. JXR
  retains its decoder-owned filename stream; a count-only `jxr_unmeasured_read` row marks successful
  opens, while actual byte counts and read duration remain combined with decode timing. The work
  classes are active image/spread, focused preview, visible thumbnail,
  nearest navigation neighbor, and distant speculation. Browse/Recents preview decode and resampling
  use worker-thread focused-preview context; replacement cancellations are attributed to the event
  thread for queued/ready work and to the worker for stale active results, while preview texture upload
  uses the same event-thread class. Thumbnail attribution follows current row visibility through its
  event-thread and background-resampler paths. Queue urgency counts remain separate.
  Input-to-presentation latency keeps the oldest dispatch until a frame is presented, so a later
  queued input cannot hide an earlier slow one. Synchronous foreground processing/resampling and
  worker operations time the pixel operation once. Event/frame/presentation timings remain in the
  SDL adapter, while decoder and image-preparation stages report their own timings. Cache snapshots
  are sampled once per second to keep their thumbnail accounting scan out of the ordinary frame path.

`main.cpp` remains the SDL composition root. It owns event dispatch and top-level rendering
orchestration, applies file-operation completions, and invokes desktop integrations.
`RendererWindowResources` owns the window/renderer pair, while `RendererTextureOwner` owns image
textures created by Viewer and renderer adapters own their private text/font textures. Viewer keeps
texture-cache keys, pins, reservations, and presentation policy. The
`FileOperationService` owns slow filesystem operations and process waits; Viewer captures paths,
image pixels, overwrite decisions, and owner generations before submission, then reconciles a ready
result on the SDL thread. Image-session, display-preparation, and presentation controllers
hold source, generation, and readiness state and return value effects. Viewer preserves their
operation order while applying viewport, cache, status, and texture effects. It forwards relevant
input and capture changes to
`InteractionWorkPolicy`, then applies its work-class plan to neighbor admission, thumbnail scheduling,
completed-frame uploads, and the low-priority scan worker. Speculative display and thumbnail work is
cooperatively canceled when it becomes irrelevant; active image and spread preparation retains priority.
Pending active-spread work counts as foreground source demand, pausing visible-thumbnail preparation
and yielding list scans. When neighbor speculation is suspended, a cold JPEG partner receives an
`active_image_spread` header-dimension read; a cold non-JPEG partner receives an
`active_image_spread` decode. Both resolve pair eligibility without admitting adjacent neighbors.
For viewer images, it passes the active `FileList` entry's captured descriptor through decode, display,
thumbnail, metadata, and preview requests. Cold current-image JPEG dimension probes and EXIF reads run
on workers; the first presentation does not wait for optional metadata. A pending current-image JPEG
probe retains the selected per-file or clipboard-return viewport, navigation direction, and startup
context under its load generation, and replacement or cancellation clears that snapshot. Held image
navigation waits for this continuation to finish before it can repeat; the event loop presents the
resumed image before the next repeat. The pending selection remains separate from the last successful
history owner, and its Recents row is committed only after the continuation succeeds. Replacement or
cancellation therefore retains the pending target's stored viewport, while a failed cold probe falls
through to normal decoding and startup failure handling for malformed images.
Timed playback produces no navigation while the current JPEG header is pending and restarts its
slideshow or movie interval from the successful display commit. Rotate and mirror commands wait for
the matching header continuation before materializing pixels; a pending slideshow transition retains
the outgoing frame and starts when the incoming image is ready.
Pixel-edit workers operate on immutable source snapshots and validate the selected owner, document
revision, and animation frame before publication. The SDL thread creates a replacement texture first,
then applies the new pixels and retires replaced buffers through the shared cache retirement service.
For animated sources, frame-bound operations pause readiness while the captured frame is processed;
failure or cancellation resumes playback, while a successful edit keeps that frame as a still image.
After an in-place save flattens an animation, the scheduler clears active playback and marks the still
ready, allowing a later slideshow or movie to start without loading another image.
Viewport pan/zoom can continue during work and final fit/manual mode restoration uses the live viewport
snapshot. Failure leaves the current document and texture intact; replacing the selected source cancels
the operation. A direct pixel operation cannot pass an earlier queued image intent; if it is accepted
while the matching source is still loading, it starts after the successful display commit and later
queued intents resume after that operation succeeds or fails. Editing an animated image flattens the
captured displayed frame.
Navigation reports a completed move, a hard boundary, or a pending directory scan separately. A timed
advance suspends readiness while its forward-boundary scan runs; if the scan finds no target or fails,
playback stops so its expired deadline cannot drive repeated redraws. Replacing or abandoning that scan
re-arms playback only when no selected-image load owns readiness.
Neighbor planning receives a bounded captured source and processing snapshot, including valid cached
JPEG dimensions, then applies only results matching current catalog, descriptor, and viewport revisions.
Pan invalidates pending neighbor planning without reading source metadata. A changed-source notice
refreshes only the matching old key.
Explicit reloads and list-changing operations, including successful lossless JPEG crop saves, rebuild
the list. A successful processed-image save refreshes a matching entry's descriptor directly; overwriting
the current image keeps its materialized pixels detached from the refreshed source.
It assembles current viewer state from the captured descriptor for the pure title-pattern formatter,
caches the result, and calls SDL only when title text changes; transient loading and error titles
remain direct status messages. Information-line formatting is likewise cached until its source,
metadata, or document state changes. It reads the persisted transparency pattern and, for frames
marked as containing alpha, paints the matching background beneath the image before alpha-blended
texture rendering. The same renderer-thread helper backs transparent thumbnails and open-dialog
previews; opaque textures retain the non-blended path. It should translate SDL events into operations
on the modules above rather than duplicate their state. Context-menu pointer hit testing also lives
here: the Right key's release activates the row under the pointer, or moves to the next column when
released outside the menu, while key-held pointer movement remains available for click-like selection.
Right-click opens the compact menu, while Shift+right-click selects the expanded menu at invocation;
the mouse adapter reads SDL's modifier state before constructing the menu.

Double-page rotation keeps the current page's already-transformed renderer texture as the spread
anchor and applies the same quarter-turn to its partner in `display_image_cache`. Source page
dimensions remain available for pair eligibility after the current image becomes landscape, while
the model rotates the shared canvas and page placements together. The presentation model still gates
publication on both pages: a transformed anchor is marked ready from its existing SDL texture, and
the spread stays hidden until the partner's rotated frame is uploaded. Rotated partner frames are not
reused as source-orientation thumbnails.

The move-to-trash confirmation draws a small preview from the active-file thumbnail texture when
available, otherwise from the already-rendered current-image texture. An existing thumbnail texture
may be pinned while confirmation is open. It creates no preview decode or pixel resize on the SDL
event/render thread; if no matching renderer resource is ready yet, the dialog shows a no-preview
placeholder alongside the filename.

The Advanced configuration modal is a thin adapter over `AdvancedConfigurationModel`: its category
tabs, visible row count, hit testing, text input, and painting stay in Viewer, while the model owns
only the staged `ViewerSettings` values and their validated transitions. The Appearance category
edits the title pattern as a text setting; a three-line UI legend lists the supported formatter
codes, and title-format errors keep the edit open without changing the draft. Apply writes the
complete draft with `SaveViewerSettings` before updating the corresponding runtime fields; a failed
write leaves live state untouched. The image-cache budget remains next-launch-only, so the UI does
not change the shared cache capacity while running. Automatically captured window/session geometry
and settings with existing menu commands are intentionally outside this editor.

Archive members use the existing filesystem-shaped path contract (`container.ext/member.ext`) so
navigation, sorting, recent-folder grouping, cache keys, and decoder APIs remain unchanged. The
browser labels and color-marks ZIP/CBZ/TAR/TGZ/7z/CB7/RAR containers and archive images without
retaining image payloads; Recents uses the same cancellable preview worker. ZIP catalogs retain
central-directory metadata only. TAR/TGZ catalogs stream member headers and skip payloads. Unencrypted
7z/CB7 catalogs use seekable libarchive input; encrypted 7z/CB7 catalogs use the optional SDK adapter
and retain member ordinal, normalized name, size, modification time, and per-entry encryption state. Header encryption
is recorded separately from encrypted data entries because it hides the entire catalog. Ordinary
unencrypted RAR catalogs remain on libarchive; the optional RAR backend inspects RAR metadata and is
used for encrypted RAR4/RAR5 catalogs and extraction. Cold
archive-directory listing runs in `ArchiveDirectoryLoader`; a newer request
cancels obsolete libarchive, 7-Zip, or RAR callback work at read/seek boundaries and generation-checks returned results. A
password-validation request holds paired source and CPU admission for the backing container and
propagates its cancellation context through catalog reads, the password probe, and bounded extraction.
It stays in metadata priority, records foreground interruptions at admission and archive checkpoints,
releases the pair, and retries the same current password request after visible image work clears; owner
replacement and shutdown wake the retry wait, and the request copy is cleared when the operation ends. A
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

Docker image downloads tolerate transient repository/CDN failures at each layer. Ubuntu 20 APT has
five acquisition retries and its source downloads allow eight retries; the shared 7-Zip/Rust
downloads also allow eight retries, and the pinned RAR Git fetch uses the retry-command helper.
GitHub Actions gives Ubuntu 20 image builds ten outer attempts (other Ubuntu images keep three),
with exponential delays capped at 60 seconds. BuildKit can reuse completed stages between those
attempts; persistent failures still fail the job.

Ubuntu 20's AOM/AVIF stage fetches libaom 3.2.0 from AOMedia's release bucket and libavif 0.9.3
from the Ubuntu archive mirror, avoiding the frequently failing Gitiles and GitHub archive endpoints.
Both archives are pinned by SHA-256 before extraction.

The Ubuntu 20 and 22 `highway-jxl` stages fetch the pinned `skcms` snapshot from the MacPorts distfiles
mirror instead of the frequently failing Gitiles archive endpoint. Its SHA-256 is checked before
extraction so the alternate mirror does not weaken source integrity.

The Open dialog routes filesystem and generic archive directory listings through
`FileDialogDirectoryLoader`; archive password validation remains on `ArchiveDirectoryLoader`.
Viewer-list startup and folder transitions use `FileListScanWorker`, so even a sequential TGZ catalog
or a 7z/RAR catalog cannot block the SDL event thread. Listing generations replace older work, sorting
is prepared by the directory worker under shared CPU admission, with generation checks during name
preparation and sorting. Shutdown changes the worker predicate under its condition-variable mutex
before notifying and joining. Escape clears the listing and its dependent metadata, summary, and
preview requests. A Browse activation pressed while rows are loading is held until that listing is
applied; folder changes, tab switches, and dialog close clear the pending activation. Filtered
parameter-restore activation retains its selected row through listing application, and listing status
updates cannot replace an operation-owned confirmation message.
File-size and directory-summary completions are drained in bounded batches so a large Recents list or
a directory with many child folders cannot monopolize one UI update. The async result/cancellation
pattern was cross-checked against
the large-folder reports in [upstream issues #194](https://github.com/sylikc/jpegview/issues/194) and
[#263](https://github.com/sylikc/jpegview/issues/263), and the worker/result approach in
[Masir01/jpegview_up's `dev-up` FileList](https://github.com/Masir01/jpegview_up/blob/dev-up/src/JPEGView/FileList.cpp).

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

At startup the composition root creates, paints, and maps the final SDL window before requesting the
initial `FileList` scan. The lazy `FileListScanWorker` constructs and sorts a complete replacement
off-thread; a directly named image can start decoding from a provisional one-image list while that
scan runs. Only the current, revision-matching result is moved into the active list on the main
thread. Renderer resources remain confined to that thread. Recent history is read after the startup
frame is shown and written once during normal cleanup, keeping history I/O out of the initial window
presentation and rapid navigation path. The file-dialog workers, viewer-list scanner, and low-priority
thumbnail-resampling worker start only when their first request arrives; the decoded-image and
display-preparation pools remain ready before the first image load so foreground rendering and
neighbor preparation are not delayed. `LoadCurrent` captures the outgoing file's exact viewport
snapshot when the path changes, restores a saved per-image snapshot only for an explicit Recents
open, and records the path only after decoding and presentation setup succeed. Ordinary navigation
uses the shared current mode and scale, even for a path with saved Recents state. Clipboard temporary
paths never become recent entries, while the
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
not decode a neighbor on the event thread. A known landscape anchor resolves to single-page
presentation before its partner dimensions are queried, so a cold partner cannot suppress the
landscape image while interaction pauses discovery. Neighbor dimensions discovered by background
work are published to the Viewer thread before the layout is resolved. A cold JPEG partner gets a
header-only dimension request; a cold non-JPEG partner gets an active-spread decode while interaction
pauses neighbor work. `DoublePagePresentationModel`
tracks the exact final-size texture keys and suppresses the single-page fallback while dimensions or
either texture are pending. Once a spread is known, Viewer submits both page requests with one
`RequestBackgroundBatch` call; while that pair is pending, the renderer uploads up to two completed
page textures in one tick. The spread becomes visible only after both SDL textures exist, so a late
partner cannot shift a page that was already shown. The model records the first frame actually
presented, so held-key repeat cannot skip a spread between texture upload and its first draw. A failed
preparation/upload releases the single-page path instead of leaving the viewport waiting forever.
Later non-JPEG neighbor completions append display work without replacing the active spread batch;
deactivation waits for an already-publishing completion before clearing queued neighbors, and callbacks
from an obsolete prefetch batch are ignored after the Viewer replaces it.
Viewer tracks the exact cold partner source request. Replacing or retiring that partner removes queued
active-spread reads and sets the in-flight cancellation token, suppressing publication after a blocked
source operation returns; policy suspension preserves and rebinds the same partner request. If the
partner becomes the selected image, Viewer deactivates its old batch and retires that tracker before
submitting the current-image JPEG dimensions request. The cache request identity combines `SourceKey`
and the dimensions-only flag, matching the key used for coalescing; the current mailbox then owns the
completion. A subsequent partner replacement therefore cannot cancel the promoted read, while a
different outgoing partner still follows the normal cancellation path.
Up/Down rotates the current image as before while rotating the spread canvas and page placements as
a unit. The partner's matching orientation is prepared off the renderer thread; the already
transformed anchor texture counts as ready, but both pages remain hidden until the partner is ready.
Original page dimensions are retained for pairing after the anchor rotates to landscape, so toggling
double-page mode off and back on does not strand the rotated image in single-page presentation.
The virtual viewport canvas combines both aspect-preserving page slots; zoom, pan, the zoom navigator,
and magnifier hit testing use that canvas while crop selection remains in the anchor page's source
coordinates. SDL textures are still uploaded and destroyed on the renderer thread. A spread does not
create a composite pixel buffer or use the single-image transition effect. The D/J mode overrides are
stored beside, but independently from, per-file viewport snapshots so Recents can restore them without
changing the shared defaults in settings.

The recent-files database stores normalized absolute paths with byte-safe record encoding, so legal
newlines and non-UTF-8 filename bytes do not break its line-based format. Loading skips malformed
records, accepts only finite absolute and relative zoom values, and clamps finite values to the
viewport's supported zoom ranges. Version-3 viewport records carry both the source-pixel scale and
fit-relative factor; older records without the factor remain readable and default that factor to 1.
Recent-folder retention is capped at 100 rows and viewport snapshots at 256 paths; these
bounds are independent so files from older folder rows can still restore their last view. Double-page
mode snapshots have their own 256-path bound.

## Refactoring status

UI decomposition remains incremental. The current boundary keeps pure state and policy in their
existing model/controller modules, gives renderer-facing texture and drawing responsibilities
explicit owners, and leaves composition, event precedence, and presentation scheduling in `main.cpp`.
Viewer still contains substantial workflow orchestration and cache policy; future extractions should
move a cohesive responsibility with its state and lifecycle, rather than only wrapping an existing
method or moving SDL calls for their own sake. Each extraction should characterize observable
behavior first and add model and UI coverage appropriate to the boundary.
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
pointer. File-dialog preview workers derive their decode target from the pane's usable image area,
which reserves one shared footer row for the left-aligned filename and right-aligned image details,
and resolve/scale only the newest requested selection using
the thumbnail resampler's source-area antialiasing. A pane resize replaces the target-size request;
the generation check prevents stale work from replacing the current preview. Preview pixels remain
outside the persistent viewer caches. Viewer asks `RendererTextureOwner` to upload and destroy their
SDL texture; the file-dialog renderer only borrows it for drawing.
The same worker returns full source dimensions (including for reduced-DCT JPEG previews) and file
size; a separate replaceable file-size worker captures source descriptors for Browse and Recents rows
using archive central-directory/header metadata for virtual members. Ordinary Browse sizes are reused
from the directory-entry listing. For a nonempty image path, the preview worker
recaptures an invalid requested descriptor before decoding and publishes pixels only under a valid,
still-current source identity. Both reject stale generations, and neither adds image decoding to the
event thread.
Display pixels may be prepared on workers, but SDL texture upload and destruction stay on the
renderer thread because SDL renderer objects are not thread-safe. One configured retained large-image
budget covers decoded pixels, prepared display frames, and retained image textures. It does not cap
total RSS. Cache-owned active decoded/prepared allocations and CPU upload staging are reported in
separate categories; materialized `Image::bgra` buffers, processing copies, codec workspaces, external
renderer allocations, and independent thumbnail storage are outside that accounting boundary. The
current image stays displayable when the budget is zero or its frame is too large to retain.

The selected source's decoded pixels move from retained-cache accounting to active working data while
the Viewer needs them for current-image or spread preparation. When that source leaves the active set,
the cache restores retained accounting only if capacity is available; otherwise it removes the cache
entry and keeps any still-live source pixels charged as working data until their final owner retires.

Each retained cache entry owns a move-only reservation. Reservations use shared control-block
identity for pixel allocations, so aliases under multiple keys count once. Removing an entry transfers
its reservation with the pixels to a shared-budget retirement coordinator. Exactly one worker waits
for the final alias and releases attached charges after destruction; cancellation in another cache or
a current-image handle cannot create another sole-owner waiter. Cache-owned active results refused by
retention remain charged as working data until their final alias retires. Before SDL copies prepared
pixels, the renderer removes the prepared cache entry and classifies its reservation as upload staging.
Texture admission gets its own retained reservation while that staging charge remains visible, so
temporary CPU-to-texture overlap is not mistaken for reclaimed CPU memory. Failed uploads release
their local texture reservation by scope exit.

Decoded and prepared entries, and SDL textures, keep hash lookup plus O(1) LRU-list iterators in
separate protection tiers. The active image and active spread stay pinned; immediate forward and
backward neighbors are evicted only after distant speculation. The renderer-thread admission policy
receives the incoming protection tier, requests at most one eligible victim per attempt across
decoded pixels, prepared frames, and image textures, and releases each owner lock before consulting
the next owner. Removing an aliased victim does not count as reclaimed capacity; deferred texture
uploads stay in a bounded queue and retry after the shared budget's retained-capacity revision advances.
Releases of temporary upload or active-working charges do not authorize another retained eviction.
Both active-spread texture keys remain explicit renderer pins while their pair is preparing or ready;
the active protection tier alone is insufficient because temporary working textures also pass through
cleanup paths that remove unpinned entries.
Decoded protection snapshots reconcile entries targeting the active tier before lower-tier changes,
because reclassification can release retained capacity needed by another entry in the same snapshot;
the original LRU order is preserved within both groups.
Pending and newly completed uploads share one priority plan and current work-class policy. Before
selected candidates attempt texture admission, every unselected completion kept for later upload has
its retained prepared-frame reservation transferred to temporary upload staging. The bounded pending
queue can then hold aliases without letting a lower-priority prepared frame consume retained capacity
while an active texture is admitted; a selected candidate that defers is staged in its admission
attempt before it enters the queue. Staging remains charged until the final pixel alias retires. Every
unclaimed completion is handed to the pending queue or worker retirement before the tick returns.
An evicted frame can still have its reservation held by retirement while a completion aliases its
pixels. Staging may reclassify that single allocation only after every retained cache entry has
relinquished ownership; a retained entry under another key keeps the shared charge in retained
capacity. The allocation remains charged once across cache, retirement, and upload aliases.
The completion's effective priority and work class travel beside its prepared pixels through planning,
texture admission, and every deferred retry because later promotion can make payload metadata stale.
Texture destruction always remains on the SDL thread. Worker-side CPU admissions may evict only within their
own thread-safe cache; when shared capacity is held elsewhere they decline retention while still
delivering the active result. The configured limit therefore governs retained large-image caching,
not total RSS or required active display data.

Prepared frames are uploaded after the current frame is presented. Active spread frames take
priority and may advance together; each maintenance opportunity admits at most one speculative
image upload, even while an active spread is being prepared. Interaction or pending foreground
source work suppresses speculative uploads. Speculative image textures use the smaller of half the
configured shared cache budget and 256 MiB; this is an internal share of the existing limit, not a
second cache setting. Over-budget distant results are discarded, while nearer results may retire one
lower-priority texture and retry after its renderer reservation has returned.

Image textures larger than 8 MiB upload through a private incomplete SDL texture in bands of at
most 4 MiB per maintenance opportunity. The prepared pixels and texture reservation stay owned by
the pending upload while bands advance. A texture becomes cache-visible and presentation-ready only
after every band and its final blend setup succeed. Source-generation replacement cancels the
incomplete upload; failed band or setup work follows the same rollback path. Small images retain a
single-call upload.

Obsolete image textures leave the cache map immediately but enter a renderer-thread retirement
queue. Outside active interaction, one queued image texture is destroyed per maintenance opportunity
after presentation. During interaction, maintenance stays quiet until four obsolete textures have
queued, then destroys at most one per opportunity so held navigation cannot defer GPU reclamation
indefinitely. Its shared cache reservation stays attached until `SDL_DestroyTexture` completes, so
capacity cannot be reused while the old renderer allocation still exists. Shutdown drains the queue
before destroying the renderer. `texture_upload` records full or banded update duration and bytes;
`texture_destroy` records renderer-thread texture-destruction duration separately. Display texture
residency snapshots include speculative bytes and their internal limit.

Expensive CPU-buffer destruction is handed back to the retirement worker, which keeps its active
strong owner until renderer-thread upload handles have released the pixels. Its shared retirement
state can outlive the cache while a valid pixel handle does, so cache destruction returns and the
worker performs final destruction after that handle releases.
Retirement workers block while their queues are empty and poll only while queued buffers still have
external owners; the thumbnail retirement thread starts on its first enqueue.
When the thumbnail panel is visible, completed display frames also feed one bounded,
very-low-priority thumbnail-resampling queue before their CPU pixels are retired. File-backed
thumbnail requests use that same worker when no display frame is available, so independent thumbnail
reads, JPEG dimension lookup, decode, and resampling do not run on the event thread. The worker keeps
at most one request active and two queued requests; thumbnail results carry generation, file
list, source-key, and target-geometry identities for validation before retention or renderer-thread
upload. `ThumbnailPixelRepository` owns the prepared BGRA pixels for generated thumbnails in the
active catalog, keyed by exact `SourceKey` and cleared when required panel geometry changes. Sorting
and navigation preserve those CPU pixels. Removed or replaced source identities are evicted from the
repository, and large buffers are handed to the thumbnail retirement worker before the SDL thread
drops its final repository reference.

The separate SDL texture map is renderer-thread owned and contains only the visible thumbnail rows,
one viewport of rows above and below, and any selected thumbnail preview pinned by an open delete
confirmation. `ThumbnailTextureWindowIndices` computes this bounded set from panel geometry; moving
the current row destroys textures outside the new set without evicting their CPU pixels. A revisit can
therefore upload directly from `ThumbnailPixelRepository` without another source read, decode, or
resample. Upload failures keep the pixels available for retry. Cache diagnostics report retained
thumbnail pixel bytes/count separately from resident thumbnail texture bytes/count. This storage is
independent of the configured large-image cache budget and can approach 1 GiB for 15,000 default-size
thumbnails. This leaves renderer upload and display preparation ahead of thumbnail work. Static images backed by a
ready display texture defer full-pixel materialization until an edit, copy, save, or another
pixel-consuming operation actually needs it. When the visible histogram is enabled, display
preparation computes its grayscale spectrum from full-source processed pixels on the worker and
carries that compact result with the prepared frame and texture. File-backed JPEG requests use a
full-resolution decode in this optional mode to preserve histogram values; ordinary fitted-JPEG
requests retain reduced-DCT decoding. Painting does not materialize source pixels or rescan the image.
Edited images compute a replacement spectrum on a generation-keyed CPU worker only while the
histogram is requested. Each request retains immutable shared pixel storage; cancellation invalidates
the result generation without blocking the SDL thread, and final pixel ownership is released through
the worker retirement path. Stale source, document, frame, and processing results are discarded. The
overlay displays a loading or unavailable line while no matching result exists. Formatted information
lines and the complete clipped paint plan are cached by source/document state and window geometry;
pointer motion only recolors the cached histogram button. Pango measurement and text clipping remain
on the SDL thread and run only when that cache key changes.
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
When Save processed overwrites the selected source, including through a symlink alias, the
output-preparation worker also materializes the lazy document's source and processed presentation
pixels before the file worker publishes them. The file worker resolves the destination again and
reports whether it still targets the selected source. Viewer refreshes the file-list descriptor and
thumbnail catalog, rebinds the retained document to the new source identity, and keeps its correction
base. This preserves the image on screen and lets later edits use the current document without pairing
old pixels with a stale source key or applying the active picture-level preset twice. For animated
sources, playback remains paused through publication; a successful in-place save keeps the captured
frame and stops playback, while a failed save resumes it. Ordinary `LoadCurrent` clears the detached
state. While the image is detached, source-coordinate lossless JPEG crop is unavailable because it
operates on the refreshed source; reloading restores that operation.

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

Fit-relative zoom arithmetic and navigation snapshots belong to `viewport`; the Viewer passes the
current virtual image/window geometry and renders its formatted readout as a short-lived SDL overlay.
That indicator uses the same viewport state as presets and steps, reports both fit-relative and
source-pixel percentages when they differ, and does not decode, resize, or retain image data.

## Build version metadata

`version.sh` resolves the nearest reachable semantic-version Git tag and adds commit-distance and
working-tree metadata for development builds. Make generates a small forced-include header from the
resolved or explicitly overridden `VERSION`; the executable uses that single value for `--version`
and the About panel. AppImage and Debian packaging scripts receive the same version; the AppImage
desktop entry's `X-AppImage-Version` and Debian control metadata use it as well. GitHub Actions
fetches tag history before resolving the value. Docker build contexts omit `.git`, so workflows and
documented Docker invocations resolve the version on the host and pass it into the container. Avoid
independent version literals in build, package, and executable metadata.

Published AppImages receive `APPIMAGE_UPDATE_INFORMATION` from `release-assets.sh`; local and
branch-build AppImages omit it unless explicitly requested. Each Ubuntu release uses AppImage's
`gh-releases-zsync` transport with a `latest` release selector and a platform-specific `.zsync`
filename wildcard. `appimagetool -u` embeds that value and generates a sidecar. Release packaging
builds directly to the final versioned AppImage filename so the zsync target filename remains
aligned, then uploads/checksums both assets together. The Ubuntu AppImage Docker images install the
standard-repository `zsync` package to provide `zsyncmake`; package creation fails if requested
update metadata does not produce the sidecar. The Ubuntu 20 release job also publishes a byte-for-byte
copy as `JPEGView-x86_64.AppImage`, the stable compatibility download alias. The release workflow
requires that Ubuntu 20 job to complete before building or publishing the Ubuntu 22/24/26 variants;
the latter are supplementary assets, not substitutes when the lowest-glibc artifact failed. The
Ubuntu 20 build is the supported glibc baseline (2.31); glibc remains a host-provided base library
and is intentionally not bundled into the AppImage.

`package-appimage.sh` owns AppDir assembly and includes the AppRun launcher, root desktop entry,
`.DirIcon`, and `usr/share/metainfo` AppStream metadata. It runs `desktop-file-validate` and
`appstreamcli validate-tree --no-net` when those tools are installed; the Ubuntu AppImage build
images provide both from their normal repositories. `release-assets.sh` verifies that the produced
image can show help with Docker networking disabled and invokes `tests/appimage_x11_smoke.sh` in
the same network-isolated container to ensure the packaged app creates its JPEGView window on X11.
The smoke script uses Xvfb and checks for the visible window title; AppImageHub still performs its
own end-to-end screenshot and compatibility review. Since each release also carries supplementary
newer-Ubuntu AppImages, the catalog's one-line `data/` entry should link directly to the stable
`JPEGView-x86_64.AppImage` baseline asset instead of asking the catalog to choose between variants.

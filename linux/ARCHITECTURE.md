# Linux frontend architecture

The SDL frontend deliberately keeps platform-independent behavior outside `main.cpp`. New logic
should normally be added to one of these focused modules and covered by `tests/test_core.cpp`:

- `file_list`: discovery, ordering, navigation modes, direct sibling-folder jumps, and current-file
  preservation.
- `image`: validated mutable BGRA storage, half-open crop extraction, rotate/mirror transforms,
  high-quality resizing, and the automatic/manual picture-level processing pipeline.
- `crop_selection_model`: source-image crop bounds, free/aspect/fixed-size selection geometry,
  move/resize hit testing, image/view coordinate conversion, and JPEG MCU-boundary alignment;
  pixel-buffer cropping remains in `image`.
- `crop_size_dialog_model`: fixed-crop dimension text, focus/unit transitions, and validation.
- `image_processing` and `image_processing_store`: bounded adjustment ranges, parameter identity,
  pixel processing, the atomic native per-image levels database, and its portable backup/restore.
- `image_decoder`, `image_writer`, and `image_formats`: codec boundaries and format policy.
- `cache_budget`, `image_cache`, and `display_image_cache`: aggregate cache accounting,
  source-aware decoded-image retention, nearest-first decode completion, and threaded picture-level
  processing/scaling of renderer-ready frames. Display keys capture every active processing value
  so an adjustment cannot reuse stale pixels. JPEG display requests use native reduced DCT decode
  before exact scaling, without requiring a retained full-resolution source frame.
- `input_commands`: SDL key chords to shared JPEGView command IDs.
- `desktop_association`: user-local desktop entry generation and atomic XDG MIME default updates.
- `settings` and `sort_mode`: persisted configuration (including default picture-level values,
  fixed crop dimensions/units, user crop aspect, and default selection mode) and stable setting
  values.
- `viewport`: fit/fill/manual zoom modes, pan state, and destination geometry.
- `resize_model`: resize-dialog values, aspect-ratio coupling, limits, filter selection, and pure
  focus/text-editing transitions.
- `context_menu_model`: the complete menu catalog, state-derived enablement/checkmarks,
  compact/advanced filtering, and actionable-item keyboard navigation.
- `playback_scheduler`: wrap-safe animation, movie, and slideshow timing expressed as Viewer actions.
- `file_dialog_model`: filtering, name/date sorting, UTF-8 editing, selection, paging, independently
  clamped viewport scrolling, focus restoration, pane-aware preview image sizing, cancellable
  background directory summaries, and replaceable previews for a focused image or a directory's
  first image.
- `overlay_layout`: content-sized filename/EXIF panel geometry and window clamping.
- `viewer_chrome`: renderer-independent overlay and navigation-panel paint plans, including icon
  primitives, hit regions, dynamic labels, and tooltip placement.
- `thumbnail_panel_model` and `thumbnail_resampler`: strip geometry, nearest-first cache scheduling,
  cancellation/LRU policy, memory sizing, antialiased source-area reduction, and low-priority
  derivation from completed neighbor display frames.
- `image_info_model`: stable dimensions/date/file-size presentation.
- `system_font`: desktop-font discovery, UTF-8 shaping, measurement, and rasterization.
- `app_icon`: extraction of the application icon embedded from the upstream ICO resource.
- `batch_copy`: pattern expansion, previews, and pure dialog focus/selection/scroll transitions.
- `desktop_applications`: non-UI discovery and planning for Open with commands.
- `external_commands`: pure argv plans and fallback order for printing, wallpaper, clipboard,
  desktop opening, trash, lossless JPEG crop, and lossless JPEG transforms.
- `exif_reader`: JPEG metadata parsing.

`main.cpp` remains the SDL composition root. It owns windows, textures, event dispatch, rendering,
and invoking desktop integrations. It should translate SDL events into operations on the modules
above rather than duplicate their state.

At startup the composition root creates, paints, and maps the final SDL window before constructing
the initial `FileList` or loading its current image. Directory enumeration and the existing
decode/display-cache path then run unchanged while the visible dark startup frame provides feedback;
renderer resources remain confined to the main thread.

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
settings module. Wheel scroll deltas update the model viewport independently from keyboard selection,
then the adapter focuses the row under the pointer. File-dialog preview workers derive their decode
target from the pane's usable image area and resolve/scale only the newest requested selection using
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

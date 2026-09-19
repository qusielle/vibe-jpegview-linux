# Linux frontend architecture

The SDL frontend deliberately keeps platform-independent behavior outside `main.cpp`. New logic
should normally be added to one of these focused modules and covered by `tests/test_core.cpp`:

- `file_list`: discovery, ordering, navigation modes, and current-file preservation.
- `image`: validated mutable BGRA storage, rotate/mirror transforms, high-quality resizing, and
  histogram-derived automatic correction.
- `image_decoder`, `image_writer`, and `image_formats`: codec boundaries and format policy.
- `image_cache` and `display_image_cache`: source-aware decoded-image retention, nearest-first
  decode completion, and threaded correction/scaling of renderer-ready frames.
- `input_commands`: SDL key chords to shared JPEGView command IDs.
- `settings` and `sort_mode`: persisted configuration and stable setting values.
- `viewport`: fit/fill/manual zoom modes, pan state, and destination geometry.
- `resize_model`: resize-dialog values, aspect-ratio coupling, limits, filter selection, and pure
  focus/text-editing transitions.
- `context_menu_model`: the complete menu catalog, state-derived enablement/checkmarks,
  compact/advanced filtering, and actionable-item keyboard navigation.
- `playback_scheduler`: wrap-safe animation, movie, and slideshow timing expressed as Viewer actions.
- `file_dialog_model`: filtering, name/date sorting, UTF-8 editing, selection, paging, scrolling,
  focus restoration, and cancellable background directory summaries.
- `overlay_layout`: content-sized filename/EXIF panel geometry and window clamping.
- `viewer_chrome`: renderer-independent overlay and navigation-panel paint plans, including icon
  primitives, hit regions, dynamic labels, and tooltip placement.
- `thumbnail_panel_model` and `thumbnail_resampler`: strip geometry, nearest-first cache scheduling,
  cancellation/LRU policy, memory sizing, and antialiased source-area reduction.
- `image_info_model`: stable dimensions/date/file-size presentation.
- `system_font`: desktop-font discovery, UTF-8 shaping, measurement, and rasterization.
- `app_icon`: extraction of the application icon embedded from the upstream ICO resource.
- `batch_copy`: pattern expansion, previews, and pure dialog focus/selection/scroll transitions.
- `desktop_applications`: non-UI discovery and planning for Open with commands.
- `external_commands`: pure argv plans and fallback order for printing, wallpaper, clipboard,
  desktop opening, trash, and lossless JPEG helpers.
- `exif_reader`: JPEG metadata parsing.

`main.cpp` remains the SDL composition root. It owns windows, textures, event dispatch, rendering,
and invoking desktop integrations. It should translate SDL events into operations on the modules
above rather than duplicate their state.

## Refactoring status

The planned Viewer decomposition is complete. Future extractions should be driven by a concrete
feature or maintenance problem rather than moving SDL calls for its own sake.
   Rendering should remain last because pixel-level X11 smoke tests are its best safety net.

The image, viewport, playback, file-dialog, dialog-controller, context-menu, thumbnail cache,
external-command, font, image-information, and viewer-chrome extractions establish the intended
pattern: a small pure C++ object, thin SDL adapter methods in Viewer, focused core tests, then UI
smoke tests for integration. Worker threads belong behind model APIs (as with directory summaries),
while SDL windows, textures, cursors, process execution, and event translation remain owned by
platform adapters. In particular, display pixels may be prepared on workers, but SDL texture upload
and destruction stay on the renderer thread because SDL renderer objects are not thread-safe.

# Linux frontend architecture

The SDL frontend deliberately keeps platform-independent behavior outside `main.cpp`. New logic
should normally be added to one of these focused modules and covered by `tests/test_core.cpp`:

- `file_list`: discovery, ordering, navigation modes, and current-file preservation.
- `image`: validated mutable BGRA storage, rotate/mirror transforms, high-quality resizing, and
  histogram-derived automatic correction.
- `image_decoder`, `image_writer`, and `image_formats`: codec boundaries and format policy.
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
- `thumbnail_panel_model` and `thumbnail_resampler`: strip geometry, nearest-first cache scheduling,
  cancellation/LRU policy, memory sizing, and antialiased source-area reduction.
- `image_info_model`: stable dimensions/date/file-size presentation.
- `system_font`: desktop-font discovery, UTF-8 shaping, measurement, and rasterization.
- `app_icon`: extraction of the application icon embedded from the upstream ICO resource.
- `batch_copy`: pattern expansion, previews, and pure dialog focus/selection/scroll transitions.
- `desktop_applications`: non-UI discovery and planning for Open with commands.
- `exif_reader`: JPEG metadata parsing.

`main.cpp` remains the SDL composition root. It owns windows, textures, event dispatch, rendering,
and invoking desktop integrations. It should translate SDL events into operations on the modules
above rather than duplicate their state.

## Refactoring backlog

The remaining Viewer work is ordered by expected testability and reduction in coupling:

1. Separate external-command planning (print, wallpaper, clipboard helpers, and lossless JPEG
   transforms) from process execution and Viewer status reporting.
2. Separate overlay/navigation drawing from Viewer after the stateful behavior above is isolated.
   Rendering should remain last because pixel-level X11 smoke tests are its best safety net.

The image, viewport, playback, file-dialog, resize, context-menu rule, thumbnail-layout/resampling, font,
image-information, and overlay-layout extractions are complete. They establish the intended
pattern: a small pure C++ object, thin SDL adapter methods in Viewer, focused core tests, then UI
smoke tests for integration. Worker threads belong behind model APIs (as with directory summaries),
while SDL windows, textures, cursors, and event translation remain owned by Viewer.

# Linux frontend architecture

The SDL frontend deliberately keeps platform-independent behavior outside `main.cpp`. New logic
should normally be added to one of these focused modules and covered by `tests/test_core.cpp`:

- `file_list`: discovery, ordering, navigation modes, and current-file preservation.
- `image_decoder`, `image_writer`, and `image_formats`: codec boundaries and format policy.
- `input_commands`: SDL key chords to shared JPEGView command IDs.
- `settings` and `sort_mode`: persisted configuration and stable setting values.
- `viewport`: fit/fill/manual zoom modes, pan state, and destination geometry.
- `resize_model`: resize-dialog values, aspect-ratio coupling, limits, and filter selection.
- `batch_copy` and `desktop_applications`: non-UI planning for external operations.
- `exif_reader`: JPEG metadata parsing.

`main.cpp` remains the SDL composition root. It owns windows, textures, event dispatch, rendering,
and invoking desktop integrations. It should translate SDL events into operations on the modules
above rather than duplicate their state.

## Refactoring backlog

The remaining Viewer work is ordered by expected testability and reduction in coupling:

1. Move the in-memory `Image` type and its rotate, mirror, resampling, and auto-contrast algorithms
   out of `main.cpp`; add exact small-fixture tests for transforms and invariants for every filter.
2. Represent context-menu entries and advanced-option filtering independently of SDL rendering.
   This will permit exhaustive command/enablement tests instead of relying only on X11 smoke tests.
3. Extract animation/movie/slideshow timing into a scheduler that returns actions for Viewer to
   execute. Test frame delays, finite loop counts, pause/resume, and tick wraparound.
4. Split file, batch-copy, and resize dialogs into controllers whose models do not manipulate SDL
   text input directly.
5. Separate overlay/navigation drawing from Viewer after the stateful behavior above is isolated.
   Rendering should remain last because pixel-level X11 smoke tests are its best safety net.

The viewport and resize-model extractions are complete. They establish the intended pattern: a
small pure C++ object, thin SDL adapter methods in Viewer, focused core tests, then UI smoke tests
for the integration.

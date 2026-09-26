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
  reduced-DCT display decoding, but also has an explicit 100-megapixel image limit; compare with a
  reproducible file before treating this as a regression.
- [KrokusPokus/JPEGView_L's release notes](https://github.com/KrokusPokus/JPEGView_L/releases)
  report JPEG XL animation and archive-contained image fixes. Linux advertises animated JPEG XL
  support, while archive browsing is not implemented; verify a concrete failing fixture before
  adding a separate bug-fix item.

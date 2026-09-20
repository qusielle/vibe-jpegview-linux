# Repository agent guide

This file is the working agreement for coding agents in this repository. It applies to the entire
tree. More specific instructions in a subdirectory may add constraints, but must not weaken the
build, test, documentation, or commit requirements below.

## Project scope

- The native Linux frontend lives in `linux/`. The upstream Windows application lives in `src/`.
- Preserve the Windows implementation unless a task explicitly requires a shared or Windows change.
- Treat Ubuntu 20.04, GCC 9, C++17, and the AppImage as supported targets. Code that builds only on a
  newer local compiler is not complete.
- Read `linux/README.md` and `linux/ARCHITECTURE.md` before changing Linux behavior or structure.
- Inspect `git status --short` before editing. Existing changes belong to the user; do not overwrite,
  reformat, stage, or commit unrelated work.

## Implementation workflow

1. Reproduce or identify the requested behavior and trace the existing ownership before editing.
2. Search for an existing model, helper, test fixture, setting, and documentation section to extend.
3. Keep platform-independent behavior in focused modules under `linux/src/`. Keep `main.cpp` as the
   SDL composition root for events, windows, renderer objects, textures, and desktop integration.
4. Add or update regression tests with the implementation. A bug fix should exercise the original
   failure when that can be tested deterministically.
5. Update user and architecture documentation as part of the same logical change.
6. Review the complete diff, including staged content, before committing.
7. Run the required checks after the final edit. If source, tests, build files, or generated-input
   files change after a check, that check is stale and must be rerun.
8. Commit one logical change, then confirm the worktree is clean. The committed `HEAD` must be the
   same content that was compiled and tested.

Do not rely on an earlier build after a final cleanup, formatting, conflict resolution, or brace-only
edit. Small edits can still introduce syntax and compatibility failures.

## Architecture rules

- Follow the module boundaries documented in `linux/ARCHITECTURE.md`. Extend an existing module when
  it already owns the behavior; create a focused module when the behavior is independently testable.
- Pure calculations, state transitions, ordering, filtering, and command planning belong outside
  `main.cpp` and should be covered by `linux/tests/test_core.cpp`.
- SDL renderer resources are main-thread objects. Create, upload, render, and destroy SDL textures on
  the renderer thread.
- Background workers may perform filesystem access, decoding, and pixel processing. Their requests
  must support cancellation or generation checks so obsolete results cannot replace current state.
- Do not make worker destruction depend on the renderer or event loop continuing to run. Join workers
  safely during owner destruction.
- Preserve the single configured large-image cache budget. Thumbnail retention is independent and all
  thumbnails for the active file list should remain available; invalidate them only when their source
  identity or required geometry changes.
- Avoid blocking the event thread with image decoding, resizing, directory scans, process waits, or
  destruction of very large buffers.
- Keep compatibility code explicit. Do not use language or library facilities newer than C++17 or
  APIs unavailable in Ubuntu 20.04 unless they are detected and have a supported fallback.

## Tests and checks

Use the smallest relevant checks while developing, then run the complete required gate after the last
edit. The normal Linux commands are:

```sh
make -C linux -j"$(nproc)" all test
make -C linux test-ui
make -C linux test-shell
make -C linux test-sanitize
```

Requirements by change type:

- Documentation-only change: run `git diff --check` and inspect rendered Markdown structure.
- C++ implementation, test, or header change: perform a clean rebuild, then run all core tests:

  ```sh
  make -C linux clean
  make -C linux -j"$(nproc)" all test
  ```

- UI layout, input, rendering, dialog, popup, or window-state change: also run `make -C linux test-ui`.
- Shell, packaging, icon embedding, Makefile, or Docker change: also run `make -C linux test-shell`.
- Worker, cache, ownership, decoding, or memory-lifetime change: also run
  `make -C linux test-sanitize`; add a focused stress test when timing or request replacement matters.
- Codec or optional-dependency change: test both the available-codec path and the intended fallback or
  unavailable-codec path where practical.

The Ubuntu 20.04 container is the compatibility authority. For source or build changes intended for
release, run:

```sh
docker build -f linux/Dockerfile.ubuntu20 -t jpegview-linux-build .
docker run --rm -v "$PWD/out:/out" jpegview-linux-build appimage
```

If Docker, X11 tools, sanitizers, optional codecs, or another required facility is unavailable, say
exactly which command was not run. Never report a check as passing based on a different compiler,
an older build, or a run performed before the final edit.

Before committing, run:

```sh
git diff --check
git diff --stat
git diff
```

After committing, verify that no intended file was omitted and no later edit escaped validation:

```sh
git status --short
git show --check --stat --oneline HEAD
```

The final handoff must list the exact test commands that passed and any check that could not be run.

## Test design

- Test observable behavior and invariants rather than private implementation details.
- Cover success, empty input, malformed input, boundaries, cancellation, and stale-result replacement
  when those cases apply.
- For asynchronous code, use bounded deadlines and generation identities. Do not use arbitrary long
  sleeps as the assertion.
- For navigation and caching, test direction reversals and rapid replacement, not only linear access.
- For filesystem behavior, use isolated temporary directories and immediate-level versus recursive
  contents deliberately.
- For UI fixes, add a model test where the logic can be extracted and an X11 smoke assertion for the
  renderer or event integration when practical.
- Keep tests deterministic across local builds, Docker, headless X11, and filesystems with different
  directory iteration orders.

## Documentation

- `linux/README.md` is the user-facing source of truth for the Linux port. Keep its prioritized
  “Linux branch changes” inventory complete as features are added or behavior changes.
- Document new controls, shortcuts, context-menu entries, settings, defaults, persistence, supported
  formats, packaging changes, and visible limitations in `linux/README.md`.
- `linux/ARCHITECTURE.md` is the developer-facing source of truth. Update it when ownership moves,
  a module is added, threading or cache behavior changes, or a new cross-component invariant appears.
- Update the root `README.md` only when the top-level Linux summary or entry points change.
- Documentation must describe the implemented behavior. Do not document planned behavior as complete.
- Keep comments focused on constraints and reasons that are not evident from the code. Do not use
  comments as a substitute for README or architecture updates.

## Commit rules

- Make atomic commits: one feature, fix, refactor, test-only improvement, documentation update, or
  build-system change per commit. A feature commit may include its direct tests and documentation.
- Separate behavior-preserving refactors from behavior changes so either can be reverted independently.
- Do not mix opportunistic cleanup with a requested fix. Record larger follow-up ideas separately.
- Use a domain-prefixed imperative subject, for example:

  ```text
  feat(dialog): Preview the focused image
  fix(rendering): Clear stale pixels after orientation changes
  perf(cache): Protect the active neighbor texture set
  refactor(viewer): Extract file dialog state
  test(dialog): Cover stale preview replacement
  docs(linux): Record the release verification workflow
  build(docker): Parallelize independent targets
  ```

- Add a commit body for nontrivial changes. Explain the user-visible request or bug cause, the chosen
  design, and important constraints. For fixes, state why the old behavior failed. For performance
  work, state what work or contention is removed; do not claim unmeasured speedups.
- Keep the subject concise and the body descriptive. Avoid vague messages such as “fix issue”,
  “updates”, or “misc changes”.
- Do not rewrite published history unless the user explicitly requests it. Before an authorized
  history rewrite, create and report a backup branch that preserves the current state.

## Definition of done

A task is complete when:

- the requested behavior is implemented without known regressions;
- regression coverage exists at the appropriate model and integration levels;
- the exact final source tree passes its required clean build and checks;
- Ubuntu 20.04/AppImage compatibility has been verified, or the unavailable verification is reported;
- README and architecture documentation accurately reflect the result;
- commits are atomic and descriptive; and
- `git status --short` is clean, apart from user-owned changes that were present before the task.

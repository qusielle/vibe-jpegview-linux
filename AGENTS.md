# Repository agent guide

This file is the working agreement for coding agents in this repository. It applies to the entire
tree. More specific instructions in a subdirectory may add constraints, but must not weaken the
build, test, documentation, or commit requirements below.

## Project scope

- This is a Linux-focused fork of `sylikc/jpegview`. Maintain the Linux delivery and port useful
  Windows behavior when practical while preserving upstream attribution and license notices.
- The native Linux frontend lives in `linux/`. The upstream Windows application lives in `src/`.
- Preserve the Windows implementation unless a task explicitly requires a shared or Windows change.
- Treat Ubuntu 20.04, GCC 9, C++17, and the AppImage as supported targets. Code that builds only on a
  newer local compiler is not complete.
- CI builds native binaries and AppImages for Ubuntu 20.04, 22.04, 24.04, and 26.04. Ubuntu 20.04 is
  the broad-compatibility AppImage baseline; `.deb` packages are built for Ubuntu 24.04 and 26.04
  using their standard repositories. Check every affected target when changing packaging or
  dependencies.
- Read `linux/README.md` and `linux/ARCHITECTURE.md` before changing Linux behavior or structure.
- Inspect `git status --short` before editing. Existing changes belong to the user; do not overwrite,
  reformat, stage, or commit unrelated work.
- Match the style of nearby code and documentation. Keep edits focused; avoid opportunistic cleanup,
  broad formatting passes, or unrelated renames. Use `apply_patch` for targeted hand edits and reserve
  formatters or scripts for genuinely mechanical changes.

## Implementation workflow

1. Reproduce or identify the requested behavior and trace the existing ownership before editing.
2. Search for an existing model, helper, test fixture, setting, and documentation section to extend.
3. Keep platform-independent behavior in focused modules under `linux/src/`. Keep `main.cpp` as the
   SDL composition root for events, windows, renderer objects, textures, and desktop integration.
4. Add or update regression tests with the implementation. A bug fix should exercise the original
   failure when that can be tested deterministically.
5. Update user and architecture documentation as part of the same logical change.
6. Review the complete diff, both staged and unstaged, before committing. Stage only files belonging
   to this task.
7. Run the required checks after the final edit. If source, tests, build files, or generated-input
   files change after a check, that check is stale and must be rerun.
8. Commit one logical change, then confirm the task's changes are committed and no accidental edits
   remain. Preserve any user-owned work that was already present. The committed `HEAD` must be the
   same content that was compiled and tested.

Do not rely on an earlier build after a final cleanup, formatting, conflict resolution, or brace-only
edit. Small edits can still introduce syntax and compatibility failures.

When a task asks for parity with Windows or another named application, inspect the cited source and
version before implementing it. Test the observable behavior, document deliberate differences, and
retain appropriate attribution. Keep behavior-preserving refactors separate from feature changes.

## Workspace integrity and session continuity

- Treat the current filesystem and Git state as authoritative, especially after a pause, context
  restart, or handoff. Re-read applicable instructions, inspect `git status --short` and `git log -1`,
  and establish which requested work remains before editing. A prior summary is not proof that a
  change is present, committed, or validated.
- Preserve the initial worktree state. Do not use destructive reset/checkout commands or broad file
  deletion to get a clean tree. Before removing or regenerating files, inspect the exact targets and
  limit the operation to task-owned outputs.
- Keep a concise record of changed files, exact checks and their outcomes, known limitations, and the
  resulting commit. Re-run any check made stale by later edits. In the handoff, distinguish passed,
  failed, and unrun checks; never imply that an unavailable check passed.
- Do not push, publish releases, or otherwise mutate remote project state unless the user has
  explicitly authorized that action in the active task.
- Delegate only when the active instructions and task authorize it. Delegated agents share the
  worktree: assign non-overlapping work where possible, inspect the integrated diff, and run final
  verification on the exact combined tree yourself.

## Architecture rules

- Follow the module boundaries documented in `linux/ARCHITECTURE.md`. Extend an existing module when
  it already owns the behavior; create a focused module when the behavior is independently testable.
- Pure calculations, state transitions, ordering, filtering, and command planning belong outside
  `main.cpp` and should be covered by `linux/tests/test_core.cpp`.
- SDL renderer resources are main-thread objects. Create, upload, render, and destroy SDL textures on
  the renderer thread.
- Background workers may perform filesystem access, decoding, and pixel processing. Their requests
  must support cancellation or generation checks so obsolete results cannot replace current state.
- Pending selections remain provisional: only a successful display commit may claim loaded-image
  history, and saved state must still belong to the selected committed path.
- During asynchronous transitions, track the identity being selected separately from the last
  successfully committed owner; cancellation or reversal must resolve state for the new selection
  without reusing transient state belonging to a canceled selection.
- Asynchronous completions must reconcile user intent issued while work is pending, and apply it only
  to the captured owner and source generation.
- Deferred user-action queues need explicit capacity and overflow behavior, and must preserve accepted
  action order across categories.
- Unrelated updates must not present pending work as complete; its visible status remains owned by
  the active operation until that operation commits or fails.
- Scope cancellation to the work's dependencies. Detach requests when ownership is promoted; a
  viewport-only change rejects viewport-bound results while preserving still-current source-only
  work. Source or owner replacement still cancels that work.
- Do not make worker destruction depend on the renderer or event loop continuing to run. Join workers
  safely during owner destruction.
- Preserve the single configured large-image cache budget. Thumbnail retention is independent and all
  thumbnails for the active file list should remain available; invalidate them only when their source
  identity or required geometry changes.
- Avoid blocking the event thread with image decoding, resizing, directory scans, process waits, or
  destruction of very large buffers.
- Keep compatibility code explicit. Do not use language or library facilities newer than C++17 or
  APIs unavailable in Ubuntu 20.04 unless they are detected and have a supported fallback.
- Extend `archive_source`'s generic container operations for new archive formats. Keep navigation,
  Recents, and previews independent of format-specific readers; preserve cancellable catalog reads,
  bounded extraction, path validation, and session-only archive passwords.
- Prefer the matching Ubuntu release's standard packages for distributable builds. Pin and verify
  checksums for source downloads that cannot come from those repositories; retries may absorb
  transient network failures but must not conceal a persistent build or test failure.

## Tests and checks

Use the smallest relevant checks while developing, then run every gate applicable to the change after
the last edit. Do not run unrelated expensive gates by rote, but do not omit a required gate. Available
Linux checks include:

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
- Broad changes and release candidates should run the full applicable gate set, not just the narrow
  test that first exposed the issue.
- A test command that reports `SKIP` has not verified that facility, even if it exits successfully.
  Report the missing dependency and the skipped coverage explicitly.

The Ubuntu 20.04 container is the compatibility authority. For source or build changes intended for
release, run:

```sh
mkdir -p out
DOCKER_BUILDKIT=1 JPEGVIEW_RETRY_ATTEMPTS=10 \
  sh ./linux/retry-command.sh -- docker build \
    -f linux/Dockerfile.ubuntu20 -t jpegview-linux-build:ubuntu20 .
APP_VERSION="$(sh ./linux/version.sh)"
docker run --rm -v "$PWD/out:/out" jpegview-linux-build:ubuntu20 appimage "$APP_VERSION"
```

The Docker build context omits `.git`, so resolve the version on the host and pass it to the
container. For packaging, codec, or CI changes, also build and verify the affected Ubuntu 22/24/26
Docker variants and, when relevant, the Ubuntu 24/26 `.deb` images. See `linux/README.md` for the
current commands and artifact names.

If Docker, X11 tools, sanitizers, optional codecs, or another required facility is unavailable, say
exactly which command was not run. Never report a check as passing based on a different compiler,
an older build, or a run performed before the final edit.

Before committing, run:

```sh
git diff --check
git diff --cached --check
git diff --stat
git diff --cached --stat
git diff
git diff --cached
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
- Use checked-in fixtures with documented provenance when fixture creation depends on codec or
  encryption-writer support that differs between supported Ubuntu releases.
- For startup, scanning, decode, cache, or thumbnail performance changes, compare representative
  before/after behavior, including many JPEGs above 8000 pixels and 10 MiB when practical. Record
  the workload and measured result; preserve foreground navigation priority and avoid adding work
  to the ordinary image-loading path for optional features.

## Documentation

- `linux/README.md` is the user-facing source of truth for the Linux port. Keep its prioritized
  “Linux branch changes” inventory complete as features are added or behavior changes.
- Document new controls, shortcuts, context-menu entries, settings, defaults, persistence, supported
  formats, packaging changes, and visible limitations in `linux/README.md`.
- `linux/ARCHITECTURE.md` is the developer-facing source of truth. Update it when ownership moves,
  a module is added, threading or cache behavior changes, or a new cross-component invariant appears.
- Update the root `README.md` only when the top-level Linux summary or entry points change.
- Documentation must describe the implemented behavior. Do not document planned behavior as complete.
- `linux/FEATURE_CANDIDATES.md` contains unimplemented ideas only. Remove or revise an entry when
  its feature ships, and describe the finished behavior in the README and architecture documentation.
- Keep comments focused on constraints and reasons that are not evident from the code. Do not use
  comments as a substitute for README or architecture updates.

## Release workflow and notes

- Create a Git tag only when the user explicitly asks. When asked, confirm the target commit and that
  the tag name is unused; make an annotated tag with a `JPEGView Linux <version>` message. Use the
  project's semantic-versioning line from `1.4.0` onward (PATCH for compatible fixes, MINOR for
  backward-compatible features, MAJOR for incompatible changes). Do not push the tag or publish a
  release unless separately requested.
- For each explicitly requested release tag, prepare release notes from the actual diff in
  `out/RELEASE_NOTES_<version>.tmp.md`, then copy the complete Markdown contents verbatim into the
  annotated tag message body after the `JPEGView Linux <version>` subject. Do not replace the notes
  with a GitHub link; the tag object should preserve the notes independently of GitHub. After
  tagging, verify the complete annotation with `git cat-file -p refs/tags/<version>`.
- `linux/version.sh` derives build metadata from the nearest reachable semantic-version tag:
  exactly the tag version there, `+devN` after it, and `.dirty` for local edits. Do not add separate
  version literals to the binary, AppImage, or Debian packaging. For version or packaging changes,
  verify that `--version` and artifact metadata agree with the intended version.
- GitHub Actions uploads build artifacts for branch pushes and pull requests, but the release-assets
  workflow runs on a **published GitHub Release**. Pushing a tag alone does not populate that
  release. The Ubuntu 20 compatibility job must succeed before the newer Ubuntu release jobs run.
- When asked to publish or verify a release, wait for the release workflow and check the uploaded
  AppImages, native binary/plugin bundles, checksums, and `.zsync` files for all four Ubuntu bases;
  check the Ubuntu 24/26 `.deb`s and the stable Ubuntu 20 AppImage alias too. Pending jobs are not
  completed release assets.
- Prepare notes from the actual diff since the previous comparable release tag, not from memory or
  the whole repository history. If no such tag exists, state the chosen base commit explicitly.
  Verify both endpoints and include a compare link at the end.
- Use this user-facing Markdown pattern, adapting sections to the changes rather than adding empty
  headings:

  ```markdown
  # JPEGView Linux <version>

  Changes since `<previous-version>`:

  ## Highlights

  - **Feature name.** User-visible behavior, controls, and important limitations.

  ## Fixes

  - **Area fixed.** What was wrong and what users should observe now.

  ## Build and distribution

  - **Packaging change.** Relevant artifact or platform detail.

  Full Changelog: https://github.com/qusielle/vibe-jpegview-linux/compare/<previous-version>...<version>
  ```

- Lead with the biggest user-visible changes. When distinguishing completely new features from
  Windows ports, list additions absent from Windows first and describe ported features accurately.
  Use concise bullets with a bold topic, concrete behavior, and relevant caveats. Include versioning
  rationale when the release changes versioning policy or makes a significant version-line transition.
- When temporary, uncommitted notes are requested, save them under `out/` using
  `RELEASE_NOTES_<version>.tmp.md`; do not commit them unless asked.

## Commit rules

- Make commits the smallest independently reviewable and revertible units practical. One commit
  should represent one behavior or maintenance concern; include that change's direct tests and
  documentation with it, but keep unrelated behaviors in separate commits.
- Do not combine distinct user-visible capabilities merely because they touch the same dialog,
  module, or broader task. For example, file-list scrolling and dialog resizing belong in separate
  commits; likewise, separately requested changes should remain separate unless they are genuinely
  coupled and cannot sensibly be reviewed or reverted independently.
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

- Add a descriptive body for nontrivial changes. Explain the user-visible request or bug cause, the
  chosen design, and important constraints. For fixes, state why the old behavior failed. For
  performance work, state what work or contention is removed; do not claim unmeasured speedups. For
  ports from another project, name the reference project and version when known.
- Make messages rich enough to explain the change without relying on the chat, while keeping the
  subject concise. Format the body with real paragraphs (for example, separate `git commit -m`
  arguments or an editor); do not put literal `\n` escape sequences into the message.
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

For documentation-only work, the definition of done is the relevant Markdown reviewed for structure
and accuracy, `git diff --check` passing, and the documentation change committed unless the user
explicitly requested an uncommitted draft; application builds and UI tests are not substitutes for
Markdown review.

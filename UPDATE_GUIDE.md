# Update Guide

## What Foro Is

`foro` is a plugin-driven code formatter daemon written in Rust. It is the end-user formatter runtime: it accepts formatting requests, loads formatter plugins, caches them, and executes them quickly across repeated runs.

The `foro` source is in https://github.com/foro-fmt/foro .

## What This Repository Is

This repository, `foro-clang-format`, is one specific `foro` plugin. Its job is to package `clang-format` so that `foro` can use it as a formatter plugin for C and C++ files.

This plugin is a **C++ shared library** built directly with CMake. It uses LLVM's `FetchContent` mechanism to download and compile the LLVM/clang source as part of the build. There is no Rust code involved in the plugin binary itself.

That means this repository is responsible for:

- tracking the correct LLVM version in `CMakeLists.txt`
- building the C++ shared library against the downloaded LLVM/clang source
- packaging the plugin in the `.dllpack` release format expected by `foro`
- producing platform-specific native artifacts that `foro` can download and run

WASM is not supported here.

## Build Structure

The build is organized as follows:

- `CMakeLists.txt` — top-level cmake project, uses `FetchContent` to download LLVM
- `build.sh` — runs `cmake -G Ninja ..` and `ninja foro-clang-format`
- `dll-pack-build-local.sh` — calls `build.sh`, then invokes `dll-pack-builder`
- `dll-pack-build-global.sh` — merges per-platform artifacts into a `.dllpack` manifest
- `BUILD_OUT_DIR` is `./build` (not the Rust `target/` directory)

## CI / Release Model In This Repo

This repo should be operated in two stages:

1. `Release Verify` workflow on a PR or manual dispatch
2. `Release` workflow only on a pushed version tag

`Release Verify` exists to test build and packaging behavior without creating a GitHub Release. `Release` is the final publish workflow and should run only after the PR is merged and a real version tag is pushed.

The goal is not just "make it build locally". The goal is:

1. update to the intended upstream LLVM version
2. keep packaging/release behavior correct on every supported platform
3. finish with a real successful GitHub Actions release run

## Scope

This repository currently packages a native `clang-format` plugin and releases these targets:

- `x86_64-unknown-linux-gnu`
- `x86_64-apple-darwin`
- `aarch64-apple-darwin`
- `x86_64-pc-windows-msvc`

WASM is not supported.

## Non-Negotiable Rules

- Do not stop at a local build if the release workflow is still broken.
- If the failure is in a dependency tool such as `dll-pack-builder`, fix that tool in its own repo and pin a reviewed commit here.
- Prefer the smallest correct fix that preserves release behavior.
- When the user asks for a release/update, the task is complete only after GitHub Actions finishes successfully on the actual tagged release.
- Do not reintroduce WASM into the release matrix for this repo.

## Repositories Involved

- main repo: `foro-fmt/foro-clang-format`
- packaging helper: `foro-fmt/dll-pack-builder`
- upstream formatter source: LLVM project (downloaded via FetchContent in `CMakeLists.txt`)

## Required Tooling

Assume these CLIs are available and use them:

- `git`
- `cmake`
- `ninja`
- `gh`
- `uv`
- `jq`

## High-Level Workflow

1. inspect current pinned LLVM version in `CMakeLists.txt`
2. inspect the latest LLVM release
3. update `LLVM_VERSION` and the `URL_HASH` in `CMakeLists.txt`
4. run local build to verify
5. push a branch and run `Release Verify` on a PR
6. if CI fails, identify whether the breakage is:
   - C++ API or cmake changes in the new LLVM version
   - packaging helper
   - runner/workflow config
7. fix the correct layer and rerun PR verification
8. merge the PR after `Release Verify` is green
9. tag the merge commit and run the real `Release` workflow
10. repeat tag-level fixes only if publish-stage behavior still fails

## Step 1: Inspect Current State

Read at least:

- `CMakeLists.txt`
- `.github/workflows/release-verify.yml`
- `.github/workflows/release.yml`
- `dll-pack-build-local.sh`
- `dll-pack-build-global.sh`
- `src/main.cpp` (or equivalent entry points)

Confirm:

- current `LLVM_VERSION` and `URL_HASH`
- release target matrix
- any helper tools pinned in the workflow

## Step 2: Inspect Upstream

For LLVM/clang, check:

- latest LLVM release version and SHA256 hash

Typical commands:

```bash
# List latest LLVM releases
gh release list --repo llvm/llvm-project --limit 10

# Get the SHA256 for the source tarball
curl -sL https://github.com/llvm/llvm-project/releases/download/llvmorg-<VERSION>/llvm-project-<VERSION>.src.tar.xz | sha256sum
```

Do not guess the hash from memory.

## Step 3: Update This Repo

Update only what is required in `CMakeLists.txt`:

- `LLVM_VERSION` string
- `URL_HASH SHA256=...` to match the new tarball

Also update:

- version in `CMakeLists.txt` `project()` — see versioning rule below
- any C++ API changes if the LLVM version has breaking changes

Versioning rule for this repo:

- **Upstream tracking release**: use the upstream LLVM version as the plugin version tag verbatim (e.g., LLVM `22.1.1` → tag `22.1.1`). Update the `VERSION` field in `CMakeLists.txt` `project()` to match.
- **Plugin-only fix** (bug fix, foro ABI change, packaging fix, workflow fix — no LLVM version change): append `-<n>` to the last upstream version, where n starts at 1 and increments (e.g., `22.1.1-1`, `22.1.1-2`).
- When a new upstream tracking release happens, n resets — the bare upstream version is used again.
- Semver ordering is intentionally not preserved for the `-<n>` suffix. These tags are GitHub release identifiers, not semver coordinates.

## Step 4: Local Validation

Run the minimum local checks before tagging:

```bash
bash -n dll-pack-build-local.sh
# Full cmake build (slow):
DLL_PACK_TARGET=x86_64-unknown-linux-gnu BUILD_OUT_DIR=./build bash dll-pack-build-local.sh
```

Note: the LLVM FetchContent build is very slow (can take 30+ minutes). Run it locally only if needed.

## Step 5: Open A PR And Run Release Verify

Do not start with a tag push. First validate the change through the PR workflow.

Note: CI runs for this repo are long due to LLVM compilation. Be patient and inspect logs before retrying.

```bash
git switch -c update/<short-topic>
git add <files>
git commit -m "<message>"
git push origin HEAD
gh pr create --fill
```

Then inspect the verification workflow:

```bash
gh run list --workflow "Release Verify" --limit 5
gh run view <run_id> --json status,conclusion,jobs,url
gh run view <run_id> --job <job_id> --log
```

The PR is not ready to merge until:

- `build-local-artifacts` succeeds on every platform
- `build-global-artifacts` succeeds

## Step 6: Merge The PR

```bash
gh pr merge --merge --delete-branch
git switch main
git pull --ff-only origin main
```

Do not tag a pre-merge branch tip. Tag the merged commit on `main`.

## Step 7: Write Release Notes and Tag

**Release notes must be committed before pushing the tag.** The CI reads `RELEASE_NOTES.md` and fails if it contains the placeholder text.

### Write RELEASE_NOTES.md

Write the end-user release notes into `RELEASE_NOTES.md` in the repo root. See the content guidelines below.

```bash
# Edit RELEASE_NOTES.md with the release notes, then:
git add RELEASE_NOTES.md
git commit -m "docs: write release notes for <version>"
```

### Push the tag

```bash
git tag <version>
git push origin <version>
```

Then inspect the publish workflow:

```bash
gh run list --workflow "Release" --limit 5
gh run view <run_id> --json status,conclusion,jobs,url
gh run view <run_id> --job <job_id> --log
gh release view <tag>
```

The task is not complete until:

- `build-local-artifacts` succeeds on every platform
- `build-global-artifacts` succeeds
- `host` succeeds
- the GitHub release exists for the new tag with proper release notes

## Failure Triage

### A. LLVM API or cmake changed

Symptoms:

- cmake configure errors
- C++ compile errors referencing changed `clang-format` API

Action:

- inspect the specific error
- update `CMakeLists.txt` and/or C++ source as needed

### B. Release workflow or runner changed

Symptoms:

- unsupported runner labels
- missing system tools (`cmake`, `ninja`, `jq`)

Action:

- fix `.github/workflows/release-verify.yml` or `release.yml`
- add missing installation steps

### C. Packaging helper broke

Symptoms:

- build succeeds
- failure happens inside `dll-pack-builder`

Action:

- inspect `dll-pack-builder` logs and source
- fix `foro-fmt/dll-pack-builder` in its own repo
- pin the exact commit in both release workflows

## How To Fix A Dependency Helper Correctly

If the problem is in `dll-pack-builder`:

1. clone `https://github.com/foro-fmt/dll-pack-builder`
2. make the smallest correct change there
3. commit and push to that repo
4. pin the exact helper commit in:
   - `foro-clang-format/.github/workflows/release-verify.yml`
   - `foro-clang-format/.github/workflows/release.yml`
5. rerun `Release Verify`

Use an exact commit pin, not a floating branch reference.

Example install lines:

```yaml
run: uv tool install git+https://github.com/foro-fmt/dll-pack-builder@<commit>
run: python3 -m pip install git+https://github.com/foro-fmt/dll-pack-builder@<commit>
```

## Commit / Tag Strategy

Typical sequence:

1. LLVM version bump in `CMakeLists.txt`
2. PR verification fix commit(s) if CI reveals issues
3. merge after `Release Verify` is green
4. final tag for the actual release

Each failed tagged release may require a new patch version. Do not retag an existing released version.

## Release Notes Content Guide

This section explains what to write in `RELEASE_NOTES.md` (used in Step 7).

### Audience

End users of `foro` — developers who run foro to format C/C++ code. They are not plugin maintainers. They do not care about CI, dll-pack, CMake, or build internals.

### What to write

- State clearly which LLVM/clang-format version is bundled.
- Summarize 2–5 notable changes in clang-format from the LLVM release notes that affect formatting output (new style options, changed defaults, bugfixes that affect output).
- If this is a plugin-only fix (no LLVM change), describe what was fixed in plain terms.
- Include a link to the LLVM release notes for full details.
- Skip anything about CMake, FetchContent, dll-pack, or build system changes.

### Format

```markdown
Bundles clang-format **X.Y.Z** (from LLVM X.Y.Z).

**What's new in clang-format X.Y.Z:**
- ...
- ...

Full release notes: https://releases.llvm.org/X.Y.Z/docs/ReleaseNotes.html

---
*This release and summary were automatically generated by an AI agent.*
```

Or for a plugin-only fix with no user-facing changes:

```markdown
Bundles clang-format **X.Y.Z** (from LLVM X.Y.Z). No formatting behavior changes.

---
*This release and summary were automatically generated by an AI agent.*
```

To find upstream changes, check:
- https://releases.llvm.org/X.Y.Z/docs/ReleaseNotes.html (search for "clang-format")
- https://clang.llvm.org/docs/ClangFormatStyleOptions.html (for new style options)

### Tone and length

- Friendly, direct, present tense.
- Aim for 5–15 lines total.
- No jargon about packaging, CMake, or dll-pack.

## What To Report Back

When done, report:

- final version/tag
- successful GitHub Actions run ID and URL
- release URL
- what changed (LLVM version, hash, C++ changes)
- whether any dependency helper was changed and to which commit it was pinned
- the generated release notes text

# Changelog

## 2026-09-22 — HalService mode validation and regression fixes (CLI-117/118)

- HalService rejects requested execution modes not declared in the module's
  `modes` list (modules.yaml) with INVALID_ARGUMENT; modes are matched
  case-insensitively including the legacy aliases ONE_SHOT/EVENT; an empty or
  missing list is not a restriction (CLI-118, `registry/ModePolicy.h`).
- `ListModules` reports non-numeric module versions as 0 with a warning
  instead of swallowing `std::stoi` exceptions (CLI-117).
- Fixed use-after-free: cached prepared statements (CLI-078) were finalized by
  their callers, leaving dangling pointers in the cache (~Spool double-free
  segfault); cached statements are now finalizer-owned by the destructor only.
- Fixed `hal_call_watchdog` returning while the HAL worker thread was still
  joinable — `std::thread`'s destructor called `std::terminate` and aborted
  the process on every completed HAL call; the worker is now joined (success)
  or detached (exception) before return.
- Test hygiene: spool fixtures use per-process unique temp DB paths and remove
  pre-existing files, so stale databases from crashed runs can no longer
  collide; cursor-based replay tests updated to the CLI-080 `ins_seq` cursor
  API.

## 2026-09-21 — Root-agent feature workflow guard (task 006)

- Added a canonical instruction requiring leaf-started agents to refuse feature
  changes and direct the user to the Acadia parent root.

## 2026-09-18 — Agent documentation and task workflow standardization

- Added canonical `AGENTS.md` and `CLAUDE.md` pointers plus a concise canonical
  agent document with Docker build and test instructions.
- Added ignored local `Plan.md` and `Task.md` workflow artifacts.
- Verified the required document shape and the declared `make build` and
  `make test` targets.

All notable changes to Acadia-Client are documented here. Keep a Changelog;
versioning: SemVer (`project()` version in CMakeLists.txt).

## [Unreleased]

### Added
- Stream auth: `x-acadia-stream-token` metadata sent on every ClientService
  Connect; token from `server_stream_token` in client.conf (env override
  `ACADIA_SERVER_TOKEN`) (X2-003).
- `make version` target for the fleet versions table (CONV-004).

### Changed
- Build simplified to a plain native CMake/Ninja build: removed the Docker
  ARM64 cross-build, QEMU test path, and remote deploy/run targets along with
  the docker/ directory. `make build` now configures and builds directly.
- Makefile help corrected: default target is the native build; `docker-build`
  is the ARM64 cross build (CLI-001). `docker-test` verified to run ctest in
  the container (CLI-002).

[Unreleased]: https://github.com/soccentric-acadia/Acadia-Client/compare/master...develop

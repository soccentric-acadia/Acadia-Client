# Changelog

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

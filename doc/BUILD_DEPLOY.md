# Acadia-Client Build and Deployment

## Scope

Acadia-Client is the C++ client/test-application tree that uses Acadia-HAL contracts. The root
`Makefile` owns native CMake/Ninja builds, ARM64 Docker builds, focused tests, local execution,
and copying test binaries and libraries to target devices. The Docker path mounts the parent
`Acadia/` directory so sibling `Acadia-HAL` sources and protos remain visible.

## 1. Prerequisites

- Linux with GNU Make, CMake, Ninja, a C++ compiler, and the dependencies declared by
`CMakeLists.txt`.
- `nproc`; `ctest`; and, for quality targets, `clang-format`, `cmake-format`, `cppcheck`, and
Doxygen.
- Docker for ARM64/QEMU builds, with permission to run containers. `make docker-install` may
install `docker.io` with `sudo`.
- `sshpass`, SSH/SCP access, and a reachable target for `deploy-*` and `run-*`.

The native build is host-architecture only. The Docker build is conditional on Docker/QEMU and
produces an ARM64 build; it is not a substitute for validation on the real board.

## 2. Setup

From this repository root:

```sh
make help
make configure
```

`configure` creates `build/native` and runs `cmake -GNinja -DBUILD_TESTS=ON`. The client build
references sibling HAL content, so retain the normal `Acadia/Project` layout when using the
Docker path.

## 3. Build and compile

```sh
make build
make -j4 build JOBS=4
make docker-install       # conditional; uses sudo only when Docker is absent
make docker-image
make docker-configure
make docker-build
```

Native outputs are under `build/native/`; cross outputs are under `build/cross/`. `make all` is
the native `build` alias. `make docker-build` uses the `acadia` ARM64 image and caps Docker
parallelism based on host CPU count.

## 4. Test and lint

```sh
make test
make docker-test
make format
make cppcheck
make doc
```

The native `test` target runs CTest filtered to `serializer_tests` and `runner_tests`; Docker
runs the complete configured CTest set. `format`, `cppcheck`, and `doc` require the
corresponding tools and may modify files for formatting. `make run` is a runtime smoke path, not
an automated test.

## 5. Packaging and artifacts

There is no package target in this Makefile. The build produces executable test applications in
`build/native/bin` or `build/cross/bin` and shared libraries in the matching `lib` directory.
Use `make list` to inspect both locations. Docker image `acadia` is a build environment, not a
release package.

## 6. Deployment, install, and run

```sh
make deploy-a
make deploy-b
make deploy IP_A=192.168.1.20 IP_B=192.168.1.21
make run-a
make run-b APP_NAME extra-arg
make run APP_NAME extra-arg
```

`deploy-a` and `deploy-b` first run `docker-build`, then copy `build/cross/lib` and
`build/cross/bin` to `/tmp/acu-test/` on the configured target. Addresses and SSH
credentials are **not hardcoded** — they come from the environment at run time
(`ACADIA_TARGET_ADDRESS`, `ACADIA_TARGET_USER`, and `ACADIA_TARGET_SSH_KEY` or
`ACADIA_TARGET_PASS`). See `loop/tools/README.md` and the `.env.example` at the master
repo root; the single source of truth for bench targets is `loop/tools/targets.yaml`.
Deploy fails fast if an address or credentials are missing, so a stale/wrong target is
never silently deployed to. `run` executes locally with
`LD_LIBRARY_PATH=build/native/lib`; remote `run-a`/`run-b` execute from `/tmp/acu-test`.

## 7. Cleanup

```sh
make clean
make docker-clean
```

`clean` removes all `build/` directories. `docker-clean` removes the cross build directory
inside the container but retains the Docker image and ccache volume.

## 8. Hardware, cloud, and manual verification caveats

No cloud deployment is defined in this Makefile. Cross compilation and CTest under QEMU do not
prove GPU, device-driver, timing, peripheral, or ABI behavior on the target. Before accepting a
deployment, manually verify SSH connectivity, target architecture, library loading, each
selected application, and the client/HAL integration on the actual board. The deploy recipes use
password-based `sshpass` and disable strict host-key checking; use only on a controlled
development network.

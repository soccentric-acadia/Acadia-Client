# Acadia-Client

On-target daemon for the Acadia IV&V Suite. Runs on Raspberry Pi / SoMs inside `Acadia-OS`, translates test configs pushed from `Acadia-Server` into HAL invocations, batches results back at 1 Hz, and never loses data across network drops or power cuts.

---

## Build

### Prerequisites

| Package | Min version | Notes |
|---------|-------------|-------|
| CMake   | 3.20        |       |
| GCC / Clang | C++17 capable | |
| git     | any         | FetchContent needs it |

`spdlog` and `toml++` are fetched automatically by FetchContent if not found on the system.

### Configure and build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Enable sanitizers (debug builds):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build -j$(nproc)
```

Treat warnings as errors (CI):

```sh
cmake -B build -DWARNINGS_AS_ERRORS=ON
```

### Run

```sh
sudo ./build/acadia-client --config /etc/acadia/client.conf
sudo ./build/acadia-client --config conf/client.conf.sample --debug
```

---

## Configuration

Copy `conf/client.conf.sample` to `/etc/acadia/client.conf` and set `server_address`. See the sample for all fields and their defaults.

`client_id` is generated automatically on first run and persisted at `/var/lib/acadia/client_id`. Do not edit it by hand.

---

## Build order

See `req.md` §"Build Order" for the full 18-step plan. Current status:

- [x] Step 1 — Skeleton + build system (this commit)
- [ ] Step 2 — Spool: schema + result_store
- [ ] Step 3 — Spool: single-writer thread + queue
- [ ] ...

---

## Repo layout (abbreviated)

```
src/
  main.cpp              entry point, signal handling
  app.h / app.cpp       top-level App; owns all subsystems
  common/result.h       Result<T> for value-based error propagation
  config/
    client_config.h/cpp TOML config parser
    client_id.h/cpp     UUID generation + persistence
conf/
  client.conf.sample    documented example config
cmake/
  CompilerWarnings.cmake
  FindAcadiaHAL.cmake   placeholder for HAL package config
```

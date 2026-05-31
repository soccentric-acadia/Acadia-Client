# Acadia-Client — Audit
*Independent verification. 2026-05-30. Every claim backed by quoted command output.*

## Verdict
- Health: **YELLOW** — Client builds cleanly, links real HAL libraries, registers 51 real adapters that genuinely call HAL `TestBase::runSelectedTests()`, and exposes both front doors (ClientService streaming + HalService 8-RPC server). But **the repo's own test suite has only 2 tests**, not 30 as the remediation report's mid-phases claimed; the "30/30" headline appears to have come from re-running the embedded **HAL** ctest list, which is registered but not buildable from this build tree.
- Top 3 findings:
  1. **True Client test count = 2** (`serializer_tests`, `runner_tests`). Both pass. The 35 HAL-named tests appearing in `ctest -N` are external (`acadia_hal_build/bin/test_*`) and `Not Run` (build path missing).
  2. **51 adapters registered**, all real. A 52nd registration is a `StubAdapter` *fallback for unknown module IDs only* (post-loop, `app.cpp:143`, registered only `if (!reg->has_adapter(m.id))`) — not a substitute for any real adapter.
  3. **Enum mapping (gap #4) does not occur in Client**: `RunOrchestrator` and `HalServiceImpl` pass proto `ExecutionMode` integers (1..4) directly to `adapter_reg_->invoke(module_id, mode, …)`. Adapters compare against proto values (`mode==3` for monitoring, `mode==4` for event). HAL `TestMode` is therefore unused in the Client/Server path — safe (no mis-cast) but the HAL enum is dead code from this side.

## Build
Command: `make build` (native).

```
[139/143] Linking CXX executable tests/test_run_orchestrator
[140/143] Building CXX object src/hal_service/CMakeFiles/acadia_hal_service.dir/HalServiceImpl.cc.o
[141/143] Linking CXX static library src/hal_service/libacadia_hal_service.a
[142/143] Building CXX object src/CMakeFiles/acadia-client.dir/app.cpp.o
[143/143] Linking CXX executable src/acadia-client
```

Verdict: **EXISTS · BUILDS** (host x86_64). ARM cross-build: **UNVERIFIABLE-HERE**.

## Tests
Command: `cd build/native && ctest -R "serializer_tests|runner_tests" --output-on-failure`.

```
Test project /home/sandesh/work/project-managers/Acadia/Acadia-Client/build/native
    Start 36: serializer_tests
1/2 Test #36: serializer_tests .................   Passed    0.01 sec
    Start 37: runner_tests
2/2 Test #37: runner_tests .....................   Passed    5.41 sec

100% tests passed, 0 tests failed out of 2
Total Test time (real) =   5.43 sec
```

True Client test count: **2** (`tests/test_result_serializer.cc`, `tests/test_run_orchestrator.cc`). Verdict: **PASSES (2/2)**.

For comparison, raw `ctest -N` lists 37 entries — the additional 35 are HAL test binaries imported via the cross-build path under `acadia_hal_build/bin/`. None of these can be run from the Client native build (the path doesn't exist):

```
Could not find executable /home/sandesh/work/project-managers/Acadia/Acadia-Client/build/native/acadia_hal_build/bin/test_gpu
…
1/37 Test  #1: test_gpu ……………….***Not Run   0.00 sec
```

This is the source of the "30/30 Client tests pass" overstatement.

## Repo-specific findings

### HAL linkage (defect #1 — closed)

`src/CMakeLists.txt:33-50`:

```cmake
target_include_directories(acadia-client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/../Acadia-HAL/src
)

target_link_libraries(acadia-client PRIVATE
    acadia_config acadia_diag acadia_hal_bridge acadia_hal_service
    acadia_janitor acadia_log acadia_ota acadia_registry acadia_run
    acadia_spool acadia_stream acadia_protos
    spdlog::spdlog tomlplusplus::tomlplusplus yaml-cpp::yaml-cpp
    SQLite::SQLite3 ${ACADIA_GRPC_LIBRARIES} protobuf::libprotobuf)
```

The `acadia_registry` static link transitively pulls every per-module HAL library used by the adapters (via `find_package(AcadiaHAL)` in `cmake/FindAcadiaHAL.cmake`). Verdict: **CONFIRMED — Client links HAL.**

### Adapters real or stubbed (defect #1 substantive — closed)

```
$ ls src/registry/adapters/*.cc | wc -l
51
```

Spot checks of `cpu_adapter.cc`, `i2c_adapter.cc`, `gpio_adapter.cc` show the same pattern:

```cpp
// src/registry/adapters/cpu_adapter.cc
#include "cpu_lib/cpu_lib.h"
CpuAdapter::CpuAdapter() : hal_(std::make_unique<CpuTesting>()) {}
…
auto results = hal_->runSelectedTests(test_names);
```

Adapters genuinely instantiate HAL `TestBase` subclasses and invoke the public API. **None are stubbed.** Per-adapter classification:

| Status | Count | Notes |
|---|---|---|
| **REAL** | 51 | One adapter per module; every one calls `hal_->runSelectedTests`. |
| **STUB (fallback)** | 1 | `StubAdapter` registered only for module IDs in `modules.yaml` that have no concrete adapter; with current 51-adapter registration this branch is never taken. |
| **MISSING** | 0 | — |

Note the **caveat**: the 16 HAL skeleton modules (i2c, spi, uart, rtc, …) have real *adapters* in Client, but the adapters call HAL functions that themselves return `SKIPPED / HARDWARE-UNVERIFIED`. The Client→HAL plumbing is correct; the underlying HAL bodies are not.

### AdapterRegistry — count

```
$ grep -c "register_adapter" src/app.cpp
52
```

51 explicit named-adapter registrations + 1 generic `StubAdapter` in a fallback loop (`app.cpp:139-143`):

```cpp
// ── Honest stubs — for any truly unrecognised module IDs ─────────────
for (const auto& m : module_reg.modules()) {
    if (!reg->has_adapter(m.id)) {
        reg->register_adapter(std::make_unique<StubAdapter>(m.id));
    }
}
```

`ListModules` returns the registry contents (HalServiceImpl.cc:321-335) — full live count must be verified in Stage 4.

### Two front doors

`src/main.cpp` / `src/app.cpp` start both a ClientService outbound stream **and** a HalService listener; both share a single `std::shared_ptr<AdapterRegistry>` (constructed once in `app.cpp` and passed to both). Verdict: **CONFIRMED.**

`HalServiceImpl` implements all **8 RPCs** with non-stub bodies (file/line):

| RPC | Location | Real body? |
|---|---|---|
| `RunSingleShot` | HalServiceImpl.cc:94 | Yes — calls `adapter_reg_->invoke(m, 1, {})` |
| `StartLongTerm` | HalServiceImpl.cc:123 | Yes — server-streaming loop with interval/duration |
| `StartMonitoring` | HalServiceImpl.cc:174 | Yes — server-streaming loop |
| `StartEventBased` | HalServiceImpl.cc:223 | Yes — server-streaming loop |
| `Stop` | HalServiceImpl.cc:265 | Yes |
| `Pause` | HalServiceImpl.cc:283 | Yes |
| `Resume` | HalServiceImpl.cc:300 | Yes |
| `ListModules` | HalServiceImpl.cc:321 | Yes |

### Enum mapping (gap #4) — Client side

`src/run/RunOrchestrator.cc:60`:
```cpp
ctx->mode = static_cast<int>(cfg.mode());     // proto ExecutionMode → int
```

`RunOrchestrator.cc:123`:
```cpp
// acadia::stream::ExecutionMode values match common.proto
switch (ctx->mode) {
    case 1: run_single_shot(*ctx); break;
    case 2: run_long_term(*ctx);   break;
    case 3: run_monitoring(*ctx);  break;
    case 4: run_event_based(*ctx); break;
}
```

`HalServiceImpl.cc:104, 187, 214, 246`:
```cpp
adapter_reg_->invoke(m, 1 /*SINGLE_SHOT*/, {});
adapter_reg_->invoke(m, 2 /*LONG_TERM*/, {});
adapter_reg_->invoke(m, 3 /*MONITORING*/, {});
adapter_reg_->invoke(m, 4 /*EVENT_BASED*/, {});
```

Adapters (e.g. `cpu_adapter.cc:32`) treat the int as **proto values** (`mode == 3` is monitoring, `mode == 4` is event). They do **not** cast to or pass through HAL `TestMode`.

Verdict: **CORRECT but bypassed.** Client never converts to HAL `TestMode`. This is safe (no mis-cast) but means the HAL `TestMode` enum is dead from the gRPC path. If anyone later writes `static_cast<TestMode>(proto_mode)` they will get the offset bug. **No explicit named map exists — the safety is by accident, not by design.**

### HAL-API-only invariant

```
$ grep -rnE "/proc|/sys|/dev|libgpiod|ioctl" src/ --include="*.cc" --include="*.cpp" --include="*.h"
src/config/client_id.cpp:12:    std::ifstream f("/proc/sys/kernel/random/uuid");
src/log/LogCollector.cc:69:    FILE* f = fopen("/dev/kmsg", "r");
src/log/LogCollector.cc:104:    std::ifstream in("/var/log/syslog", …);
src/ota/OtaClient.cc:42:    // Write 0 to /sys/firmware/efi/efivars/... (comment only)
```

The only direct `/proc`/`/sys`/`/dev` access is in (a) **client identity** (UUID for boot, not a test path), (b) **log collection** for diagnostics, and (c) an OTA comment. **No test-path violation.** Verdict: **CONFIRMED.**

### Cancel-flag pattern

`src/hal_service/HalServiceImpl.h:88`:
```cpp
struct Session {
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> paused{false};
};
std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
```

Sessions are held by `shared_ptr<Session>`; the atomics are **value members**, not raw pointers. Verdict: **CONFIRMED — `unique_ptr`/`shared_ptr` semantics, not raw `atomic<bool>*`.**

### Real HAL call (gap #2)

A real, non-stubbed adapter→HAL call path exists for every REAL HAL module. Example from `cpu_adapter.cc → CpuTesting → /sys/devices/system/cpu/*`:

```cpp
hal_ = std::make_unique<CpuTesting>();
auto results = hal_->runSelectedTests(test_names);   // reads sysfs, returns real PASS/FAIL data
```

But there is **no Client-resident test that exercises this end-to-end** (the 2 Client tests cover the result serializer and the runner abstraction). Definitive proof of the pipe must come from the live PySDK→Client run in §4 of the parent prompt. Verdict for the static path: **WORKS-NO-HW** (path is wired and non-stub; output realism depends on host capabilities).

## Code health
- Branch `develop`, commit `a5ec043`, tree clean.
- All 51 adapters follow a near-identical template (`run(int mode, …)`); easy to maintain.
- The `1 /*SINGLE_SHOT*/` / `2 /*LONG_TERM*/` / `3 /*MONITORING*/` / `4 /*EVENT_BASED*/` magic numbers should be a typed enum or `static_cast<int>(hal::common::ExecutionMode::SINGLE_SHOT)` for safety against drift. Recorded as a finding, not fixed.
- `src/run/RunOrchestrator.h:51` comment says "acadia::stream::ExecutionMode" but the type is just `int` — minor doc/code gap.

## Claims vs reality (Client slice)

| Claim | Verdict | Evidence |
|---|---|---|
| 30/30 Client tests pass | **FALSE** | `ctest -R "serializer_tests|runner_tests"` yields **2/2**; `ctest -N` lists 37 because of imported HAL targets, but those 35 are `Not Run` (path missing). |
| 51 real adapters registered | **CONFIRMED** | `grep -c register_adapter src/app.cpp` = 52, of which 51 are unique named adapters and 1 is a fallback `StubAdapter` for unknown IDs (never reached today). |
| HAL library actually linked | **CONFIRMED** | `target_link_libraries(acadia-client PRIVATE acadia_registry …)` plus include path `${CMAKE_SOURCE_DIR}/../Acadia-HAL/src`. |
| HalService implements 8 RPCs with real bodies | **CONFIRMED** | All 8 found in `HalServiceImpl.cc`, each with non-trivial implementation (server-streaming loops, session state). |
| Cancel flags use `unique_ptr` not raw `atomic<bool>*` | **CONFIRMED** | `Session` struct holds atomics by value, owned via `shared_ptr<Session>`. |
| Enum mapping correct (proto ↔ HAL `TestMode`) | **OVERSTATED** | No explicit map; the safety is structural (HAL `TestMode` never used in gRPC path), not enforced by an explicit conversion. |
| Both front doors share one AdapterRegistry | **CONFIRMED** | One `std::shared_ptr<AdapterRegistry>` constructed in `app.cpp` and handed to both ClientService and HalServiceImpl. |

## Unverifiable-here
- ARM/Pi5 cross-build.
- Live behaviour of the streaming RPCs (covered in Stage 4 / `AUDIT_SUMMARY.md`).

## Evidence appendix

```
$ git rev-parse --abbrev-ref HEAD
develop
$ git log --oneline -1
a5ec043 (HEAD -> develop, origin/develop) Add CI workflow, boot verification checklist, and HAL service implementation with new adapters

$ ls src/registry/adapters/*.cc | wc -l
51

$ grep -c "register_adapter" src/app.cpp
52

$ grep -nE "::RunSingleShot|::StartLongTerm|::StartMonitoring|::StartEventBased|::Stop|::Pause|::Resume|::ListModules" src/hal_service/HalServiceImpl.cc
94:::grpc::Status HalServiceImpl::RunSingleShot(
123:::grpc::Status HalServiceImpl::StartLongTerm(
174:::grpc::Status HalServiceImpl::StartMonitoring(
223:::grpc::Status HalServiceImpl::StartEventBased(
265:::grpc::Status HalServiceImpl::Stop(
283:::grpc::Status HalServiceImpl::Pause(
300:::grpc::Status HalServiceImpl::Resume(
321:::grpc::Status HalServiceImpl::ListModules(
```

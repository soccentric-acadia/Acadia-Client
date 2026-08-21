# Acadia Client

## Project Description

Acadia-Client is the embedded C++ execution client that turns the Acadia hardware-test registry
into runs on a target board. The executable is assembled from `src/app.cpp` and CMake
subdirectories for configuration, diagnostics, the HAL bridge, registry, run orchestration,
spooling, and streaming. `client_config` and `client_id` provide process configuration and board
identity; `DiagSocket` supplies a local diagnostic channel. The registry owns module discovery
and adapters for GPIO, I2C, SPI, UART, USB, PCIe, NVMe, storage, thermal, power, camera,
display, audio, GNSS, IMU, GPU, video, security, watchdog, and system integration. The adapter
inventory is implementation evidence, but individual adapters may still be stubs or
hardware-dependent, so presence is not proof of successful board operation.

`RunOrchestrator` coordinates test execution and result production. `ResultSerializer` converts
results for the HAL bridge, while `StreamClient` handles the server-facing stream and `Spool`
provides local buffering when delivery is unavailable. Deployment configuration lives in `conf`,
`docker`, and `systemd`. The committed tests are `tests/test_result_serializer.cc` and
`tests/test_run_orchestrator.cc`. Suggested checks are `cmake -B build`, `cmake --build build`,
and the configured test target or `ctest`; runtime behavior still depends on the HAL, protocol
artifacts, server endpoint, and target device.

The repository is therefore an integration point, not a replacement for the HAL libraries or the
server. Configuration files and systemd assets describe deployment, while CMake files define the
compilation boundaries. Adapter-level confidence should be established from the corresponding
HAL tests and from a target with the expected Raspberry Pi device nodes. Disconnect/reconnect
behavior, spool replay, command ordering, and malformed-result handling are particularly
important operational cases that are not established by the two committed unit-test files.

## Feature Status

| # | Name / Description | Implemented? | Test Coverage? | Manual Test |
|---:|---|---|---|---|
| 1 | Application lifecycle | Implemented | Unknown | [ ] Manual verification required |
| 2 | Configuration and client identity | Implemented | Unknown | [ ] Manual verification required |
| 3 | Diagnostic socket | Implemented | Unknown | [ ] Manual verification required |
| 4 | HAL bridge and result serialization | Implemented | `test_result_serializer.cc` | [ ] Manual verification required |
| 5 | Module registry and adapter discovery | Implemented | Unknown | [ ] Manual verification required |
| 6 | Hardware adapter family | Partial: device behavior unverified | Unknown | [ ] Manual verification required |
| 7 | Run orchestration | Implemented | `test_run_orchestrator.cc` | [ ] Manual verification required |
| 8 | Server streaming client | Implemented | Unknown | [ ] Manual verification required |
| 9 | Offline result spool | Implemented | Unknown | [ ] Manual verification required |
| 10 | CMake, Docker, systemd, and Doxygen integration | Implemented | Unknown | [ ] Manual verification required |

## Test Evidence

- `tests/test_result_serializer.cc`
- `tests/test_run_orchestrator.cc`
- `ctest --test-dir build --output-on-failure` when CTest is generated.

## Limitations

No full-board, network, or server integration verification is claimed.

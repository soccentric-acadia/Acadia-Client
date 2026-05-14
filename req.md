# Client App Update — Align with HAL Refactor


the HAL repo is in ../Acadia-HAL reivew it fist get a good idea 

You are updating the **Client App** (on-target C++ gRPC agent) to consume the **refactored HAL result types**. The HAL has been refactored to a new canonical `TestResult` / `TestOutput` shape, and the shared protos have been updated in lockstep. The Client App must now be updated everywhere it touches results: serialization, streaming, buffering, adapters, runner integration, tests.

This is a long-running task. Time is not a constraint. **Correctness, completeness, and consistency are the only constraints.**

---

## 1. Background — what changed in the HAL

The HAL's canonical result types are now:

```cpp
namespace hal {

using Value = std::variant<bool, int64_t, double, std::string>;

enum class Status  { PASS, FAIL, ERROR, TIMEOUT, SKIPPED, ABORTED };
enum class Severity { INFO, WARNING, ERROR, CRITICAL };

struct TestOutput {
    Status status;
    Severity severity;
    std::map<std::string, std::string> reason;
    std::map<std::string, Value> metrics;
    std::map<std::string, Value> monitoring;
    std::map<std::string, Value> event;
    std::string error_message;
};

struct TestResult {
    std::string module_id;
    std::string test_id;
    std::string name;
    uint64_t sequence;
    std::chrono::system_clock::time_point timestamp;
    std::chrono::nanoseconds duration;
    TestOutput output;
    std::vector<TestResult> subtests;
};

} // namespace hal
```

The old shape (`bool passed`, `std::vector<std::string> data` or `std::map<std::string,std::string> data`, `duration<double>`) is gone. The Client App previously bridged the old shape to proto. It must now bridge the new shape.

The mirroring proto types under `hal.common`:

```proto
enum TestStatus  { PASS, FAIL, ERROR, TIMEOUT, SKIPPED, ABORTED, … }   // exact values per protos/common.proto
enum TestSeverity { INFO, WARNING, ERROR, CRITICAL, … }
message TypedValue { oneof value { bool, int64, double, string } }
message TestOutput { status, severity, reason, metrics, monitoring, event, error_message }
message TestResult { module_id, test_id, name, sequence, timestamp, duration, output, subtests }
```

Read `protos/common.proto` for exact field names, tags, and the enum ordering. **Do not assume — read.**

The Client App's role does not change. It still:
- Receives commands from Server via the control channel
- Drives the HAL in one of four modes (SINGLE_SHOT / LONG_TERM / MONITORING / EVENT_BASED)
- Wraps each HAL `TestResult` into `ResultEnvelope` and streams to the Server
- Buffers locally on disconnect, applies backpressure when full

What changes is **the shape of the data flowing through every result-handling code path**.

---

## 2. Scope of this update

You will touch:

1. **HAL bridge / serializer** — the code that converts `hal::TestResult` ↔ proto `hal.common.TestResult`. Full rewrite of the conversion logic.
2. **Module adapters** — per-module adapters under `client/src/registry/adapters/`. Each adapter must now produce the new `hal::TestResult` shape and pass it through the updated serializer.
3. **Runner integration** — the runner stamps `sequence` and `timestamp` (the HAL produces everything else). Verify and enforce that responsibility split.
4. **Result streamer** — wraps `hal::TestResult` into `ResultEnvelope` for transport. Update to use the new proto fields.
5. **Local buffering** — if results are buffered in memory (or to disk for overflow), the on-wire / on-disk format must match the new proto. Migrate buffer code; if there's any persistence format, version it.
6. **Backpressure path** — unchanged behavior, but verify it still applies cleanly under the new types.
7. **Tests** — every unit test, every integration test, every fake module / fixture that produces or asserts on results.
8. **Logs** — any log lines that include result fields (`passed`, `data`, etc.) must use the new fields.

You will **not** touch:
- The HAL itself (already done).
- The protos (already done).
- The Server App (separate prompt).
- The Web UI (separate prompt).

---

## 3. Architectural rules — unchanged but reaffirmed

These are load-bearing from the original Client design. The refactor does not relax any of them.

- **Registry-driven dispatch.** No `switch(module_id)` outside the `AdapterRegistry`. Adapters self-register; the refactor does not introduce dispatch shortcuts.
- **HAL is the only owner of result content.** The runner stamps `sequence` and `timestamp`. The Client never invents `metrics`, `monitoring`, or `event` data — it only forwards what the HAL produced.
- **Backpressure over drop.** Bounded queues. If the Server is unreachable and buffers fill, slow the HAL workers — never silently discard results.
- **gRPC I/O thread does no HAL work.** HAL execution always on workers; I/O thread only manages streams.
- **Adapter as the only HAL-aware code.** The serializer is generic over `hal::TestResult`; adapters do not implement their own serializers.

If the refactor tempts you to break any of these (e.g., "the new `metrics` map is awkward, let me add a quick switch in the streamer"), stop and reconsider.

---

## 4. Required code changes — concrete

### 4.1 The serializer (`hal_bridge/ResultSerializer.h/.cc`)

This is the single canonical conversion point between `hal::TestResult` and `hal.common.TestResult`. It must handle every status, every severity, every `Value` variant, and recursive subtests.

```cpp
namespace client::hal_bridge {

// hal::Status -> hal.common.TestStatus
hal::common::TestStatus to_proto(hal::Status s);
hal::Status              from_proto(hal::common::TestStatus s);

// hal::Severity -> hal.common.TestSeverity
hal::common::TestSeverity to_proto(hal::Severity s);
hal::Severity              from_proto(hal::common::TestSeverity s);

// hal::Value -> hal.common.TypedValue
hal::common::TypedValue to_proto(const hal::Value& v);
hal::Value              from_proto(const hal::common::TypedValue& v);

// std::chrono::system_clock::time_point -> google.protobuf.Timestamp
google::protobuf::Timestamp to_proto(std::chrono::system_clock::time_point tp);
std::chrono::system_clock::time_point from_proto(const google::protobuf::Timestamp& ts);

// std::chrono::nanoseconds -> google.protobuf.Duration
google::protobuf::Duration to_proto(std::chrono::nanoseconds d);
std::chrono::nanoseconds   from_proto(const google::protobuf::Duration& d);

// Maps. Free functions or inlined into to_proto(TestOutput).
void copy_reason_to_proto(const std::map<std::string,std::string>& src,
                          google::protobuf::Map<std::string, std::string>* dst);
void copy_value_map_to_proto(const std::map<std::string, hal::Value>& src,
                             google::protobuf::Map<std::string, hal::common::TypedValue>* dst);
void copy_reason_from_proto(const google::protobuf::Map<std::string, std::string>& src,
                            std::map<std::string,std::string>& dst);
void copy_value_map_from_proto(const google::protobuf::Map<std::string, hal::common::TypedValue>& src,
                               std::map<std::string, hal::Value>& dst);

// Top-level.
hal::common::TestOutput to_proto(const hal::TestOutput& out);
hal::TestOutput          from_proto(const hal::common::TestOutput& out);

hal::common::TestResult to_proto(const hal::TestResult& r);   // recursive subtests
hal::TestResult          from_proto(const hal::common::TestResult& r);

} // namespace
```

**Rules for the serializer:**

- **Enum mapping is exhaustive.** Every `hal::Status` value has a proto counterpart. Use a `switch` with no default; let the compiler warn on missing cases. Same for `Severity`.
- **`Value` variant uses `std::visit`** to set the proto `oneof`. On parse, switch on `TypedValue::value_case()`; on `VALUE_NOT_SET`, fall back to empty string and log a `[SERIALIZER-FALLBACK]` warning with the offending key.
- **`UNSPECIFIED` proto values on inbound** convert to `Status::ERROR` with `error_message = "received unspecified status"`. Same for severity → `INFO`.
- **Timestamp encoding** uses seconds + nanos split. Document UTC assumption.
- **Duration encoding** uses seconds + nanos split. Negative durations forbidden; clamp to zero with a `[SERIALIZER-CLAMP]` log line.
- **Recursive subtests** call `to_proto`/`from_proto` on the `TestResult` again. Watch stack depth — cap recursion at a configurable limit (default 32) and convert deeper trees to a single flattened error result.

**Round-trip property test** is mandatory: for every `Status`, every `Severity`, every `Value` variant (bool, int64, double, string), with non-empty `reason`/`metrics`/`monitoring`/`event` maps and at least one subtest level, `from_proto(to_proto(x)) == x` must hold.

### 4.2 The `ResultEnvelope` wrapper

Find the code that wraps a `hal::TestResult` into the on-wire `ResultEnvelope` for streaming (likely in `grpc_client/ResultStreamer.cc`). Update it to:

- Set `module_id` from `result.module_id` (no longer guessed at the streamer layer).
- Set `test_id`, `name`, `sequence`, `timestamp`, `duration` directly from the serialized proto `TestResult`.
- Carry the proto `TestResult` as the typed payload — either directly in `ResultEnvelope` if its schema embeds it, or via `google.protobuf.Any` if the existing envelope is `Any`-based.
- Tag with `run_id` from the runner context.

If the envelope contract changed in protos as part of the HAL refactor (it might have), follow what `hal_service.proto` and `common.proto` actually define. Do not assume the old shape.

### 4.3 Module adapters

Each adapter in `client/src/registry/adapters/<module>.cc` produces `hal::TestResult` instances by invoking the HAL and assembling output.

For every adapter:

1. Replace any old code that set `output.passed = true/false` with `output.status = hal::Status::PASS` (or appropriate non-PASS).
2. Replace any old code that pushed strings into `output.data` with the correct destination — `reason`, `metrics`, `monitoring`, or `event` — per the same decomposition rule used in the HAL refactor:
   - Numeric measurements → `metrics` as typed `Value`.
   - Environmental samples (mode = MONITORING) → `monitoring`.
   - Event-trigger payloads (mode = EVENT_BASED) → `event`.
   - Free-form explanations → `reason`.
3. Set `severity` deliberately. Defaults: `INFO` on PASS, `ERROR` on FAIL/ERROR, `WARNING` for soft failures the module classifies as such.
4. Set `module_id` from the module's canonical identifier in `modules.yaml`. Stop relying on the streamer to inject it.
5. Set `test_id` — stable across renames. If the module previously used `name` as identity, set `test_id == name` for now and add a code comment so this can be revisited per-module.
6. Do **not** set `sequence` or `timestamp` inside the adapter — the runner stamps these. Leave them default-constructed.
7. Populate `error_message` whenever the adapter catches an exception or detects a HAL precondition failure. Set `status = ERROR`.
8. For HAL modules that emit hierarchical results (per-core, per-channel, per-pin), populate `subtests` with sub-`TestResult`s instead of stuffing keys like `core0.temp` into the parent. Subtests are first-class.

If an adapter currently stringifies numeric values to fit the old `data: vector<string>` shape, restore the original numeric type into a `Value`. The Web UI's click-to-chart depends on numeric typing.

### 4.4 The runner

The runner owns `sequence` and `timestamp`. Verify the existing runner is the one and only place these are set:

- **SINGLE_SHOT**: stamp `sequence = 0`, `timestamp = now()` immediately after the HAL returns.
- **LONG_TERM / MONITORING / EVENT_BASED**: maintain a per-run, per-module monotonic `sequence` counter starting at 0. Stamp `timestamp = now()` when each result arrives from the HAL.
- The runner must also catch HAL exceptions / timeouts / cancellation and synthesize a `TestResult` with the appropriate non-PASS `Status` and `error_message`. Adapters can also catch and surface, but the runner is the safety net.
- **TIMEOUT** synthesis: when a worker exceeds its allotted duration, runner builds a `TestResult` with `status = TIMEOUT`, `error_message = "test exceeded N ms"`, and a partial `metrics` map if the HAL provided any pre-timeout data.
- **ABORTED** synthesis: when stop/cancel is received mid-run, runner builds a final `TestResult` with `status = ABORTED`, `error_message = "cancelled by server"`.

If `sequence`/`timestamp` are being set anywhere else (adapter, serializer, streamer), remove and consolidate to the runner.

### 4.5 Buffering and persistence

The local in-memory queue carries `ResultEnvelope` (or its predecessor, depending on what the queue type is). Verify:

- Queue element type matches the new envelope shape.
- Any disk-overflow path serializes the new proto. If the on-disk format is just `proto.SerializeToString()`, the change is transparent at the bytes level — but old buffered files written before the refactor are now incompatible. On startup, if the agent finds buffered overflow files in the old format, log a `[BUFFER-INCOMPATIBLE]` line and quarantine them (move to a `.legacy/` subdir, do not delete, do not attempt to send).
- Size accounting (bytes per queued envelope) still works under the new shape, since the payload size is generally larger now (typed maps).

### 4.6 Logs

Search the Client App source for log lines referencing `passed`, `.data`, `output.passed`, `output.data`, etc. Replace each with the equivalent new field — `status`, `metrics`, `reason`, etc. — making sure the log line is still readable.

### 4.7 Headers and includes

Audit `#include` directives across the Client. If any TU still includes an old HAL types header that no longer exists, replace with the new canonical header path. Document the new include path in the Client README.

---

## 5. Tests — required coverage

### 5.1 Serializer unit tests
For every `Status`, every `Severity`, every `Value` variant, including:
- Empty result (all maps empty, no subtests).
- Populated maps in `reason`, `metrics`, `monitoring`, `event` simultaneously.
- Subtests one level deep, two levels deep, and at the recursion cap (32 levels).
- Pathological cases: very large maps (1k entries), large string values (1 MB), zero duration, far-future and far-past timestamps.

Round-trip property: `from_proto(to_proto(x)) == x`.

### 5.2 Adapter migration tests
For each migrated adapter, at least one test that:
- Drives the HAL (or `FakeModule`) through a known scenario.
- Asserts on the new `TestResult` shape — `status`, specific keys in `metrics` or `reason` with their typed values, `module_id`, `test_id`.
- Confirms no leftover use of `passed` / `data`.

### 5.3 Runner integration tests
- SINGLE_SHOT: sequence == 0, timestamp set, exactly one result.
- LONG_TERM: monotonic sequence (0, 1, 2, …), timestamps monotonic-ish (allow tiny clock skew).
- MONITORING: `output.monitoring` is non-empty when configured; the keys match what was requested.
- EVENT_BASED: `output.event` is non-empty when triggered; sequences only increment on triggers (no events between).
- TIMEOUT path: kill a worker that overruns, assert `status == TIMEOUT` and `error_message` is set.
- ABORTED path: send cancel mid-run, assert final result has `status == ABORTED`.

### 5.4 End-to-end with mock server
Wire the in-process mock gRPC server. Drive a fake module in each mode. Verify the Server-side receives results with all new fields populated correctly. Verify disconnect-with-buffered-replay still works under the new payload shape.

### 5.5 Backward-compat smoke test (optional but recommended)
If old buffered overflow files exist on the agent disk, restart the agent and verify they are quarantined to `.legacy/`, not sent, not lost.

### 5.6 Grep verification
After all changes, grep the Client source tree:

```
grep -rn "\.passed\b" client/src/ client/tests/
grep -rn "output\.data\b" client/src/ client/tests/
grep -rn "duration<double>" client/src/ client/tests/
```

Expect zero hits (except in legacy/quarantine handling code, which should be commented and clearly scoped).

---

## 6. Workflow — phases

Long-running task. No human gates. Write each phase report under `client_update/`.

### Phase 1 — Discovery
1. Confirm the HAL refactor is in place: read the HAL types header and `protos/common.proto`. If they don't match Section 1, **stop** and report — the Client cannot be updated against a stale HAL.
2. Find every Client file that references the old result types or fields (`passed`, `data` on `TestOutput`, `duration<double>` on `TestResult`). List them all.
3. List every adapter under `client/src/registry/adapters/`. Cross-check against `modules.yaml` — flag any mismatch.
4. List the serializer file(s), the streamer, the runner, the buffering code, the test files.
5. Identify any code that owns `sequence` or `timestamp` today (it should be the runner only).

Write `client_update/01_discovery.md`.

### Phase 2 — Plan
For each file from Phase 1, a concrete edit plan in dependency order: types/serializer first, then runner, then adapters, then streamer/buffering, then tests, then logs.

Write `client_update/02_plan.md`.

### Phase 3 — Serializer rewrite
1. Implement / replace `ResultSerializer` per Section 4.1.
2. Write the round-trip property tests per Section 5.1.
3. Run serializer tests until green.

Write `client_update/03_serializer.md`.

### Phase 4 — Runner updates
1. Verify and enforce that `sequence` and `timestamp` are stamped only in the runner.
2. Add TIMEOUT, ABORTED, ERROR synthesis paths.
3. Add tests per Section 5.3.

Write `client_update/04_runner.md`.

### Phase 5 — Per-adapter migration
For each adapter (in alphabetical order of `module_id`):
1. Update per Section 4.3.
2. Add adapter test per Section 5.2.
3. Compile and run the adapter's tests. Fix breakage. Move on only when green.
4. Append a per-adapter entry to `client_update/05_adapters/<module_id>.md`.

### Phase 6 — Streamer and buffering
1. Update envelope wrapping per Section 4.2.
2. Update buffering type plumbing per Section 4.5.
3. Implement legacy-buffer quarantine.
4. Add the optional backward-compat smoke test if disk overflow is enabled.

Write `client_update/06_streaming.md`.

### Phase 7 — Logs and includes audit
1. Replace stale log lines (Section 4.6).
2. Replace stale includes (Section 4.7).
3. Run the grep verification (Section 5.6). Expect zero hits.

Write `client_update/07_audit.md`.

### Phase 8 — Full verification
1. Build the Client clean for host and at least one cross-compile preset (ARM64).
2. Run all unit + integration tests. All green.
3. Run end-to-end against the mock server (Section 5.4).
4. Run for 5+ minutes in LONG_TERM mode with a fake module, with simulated disconnect at minute 2 and reconnect at minute 3 — verify no data loss, sequences contiguous after replay.
5. Verify resource footprint hasn't regressed materially (steady-state RSS, CPU). Document numbers.

Write `client_update/08_verification.md` with evidence (build logs, test summary, footprint numbers, disconnect-test sequence audit).

### Phase 9 — Final report
Summarize all changes: serializer, runner, adapters (table), streamer, buffering, tests added, logs/includes updated, footprint before/after, grep result, any ambiguities resolved.

Write `client_update/09_final_report.md`.

---

## 7. Hard rules

- **The new types from Section 1 are the spec.** No additions, no omissions.
- **The proto definitions in `protos/common.proto` win.** If anything in this prompt contradicts what's actually in the protos, the protos are correct — stop and reconcile.
- **No `switch(module_id)`** introduced anywhere as a side effect of this refactor. Adapter registry remains the only dispatch.
- **`sequence` and `timestamp` belong to the runner.** No adapter, no serializer, no streamer sets them.
- **No silent data loss.** Old `data` keys flow into one of `reason`/`metrics`/`monitoring`/`event`. If the destination is ambiguous, log `[MIGRATION-AMBIGUOUS]` with file, key, and chosen destination — record in the audit.
- **`passed` and the old `data` field must not survive anywhere in active code paths.** Grep verifies. Legacy-quarantine handling is the only exception, and it must be commented as such.
- **Numeric values stay numeric.** Adapters that previously stringified numbers must restore the typed value. The UI's click-to-chart depends on this.
- **The HAL is not modified.** If the new HAL types appear to require a HAL change, stop and raise it — do not patch the HAL from inside the Client refactor.
- **The protos are not modified.** Same rule.
- **No new heavyweight dependencies.** This still runs on a Pi.
- **All long-running threads remain cooperatively cancellable.** Do not introduce detached threads while refactoring.
- **Backpressure semantics are preserved.** The new larger payloads must not silently cause queue size accounting to undercount.

---

## 8. Deliverables

At completion of Phase 9:
- Client App building clean for host and ARM cross.
- All unit + integration tests green.
- Serializer with round-trip property tests covering every `Status`, every `Severity`, every `Value` variant, including subtests.
- All adapters migrated and tested.
- Runner stamps `sequence`/`timestamp` exclusively; TIMEOUT/ABORTED/ERROR synthesis paths covered.
- Streamer and buffering using the new envelope; legacy-buffer quarantine in place.
- Logs and includes cleaned; grep for `passed` / `output.data` / `duration<double>` returns zero hits in active code.
- All phase reports under `client_update/01_…` through `client_update/09_final_report.md`.
- End-to-end disconnect-replay scenario captured as evidence.

Begin with Phase 1.
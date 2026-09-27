# Acadia-Client Remote Validation Suite

Validates a deployed `acadia-client` on a Raspberry Pi (or any Linux target)
**entirely over SSH from an x86 Linux host**. Nothing is installed on the
device; the host only needs Python 3.11+ plus the packages in
`requirements.txt`.

```
x86 Linux host (this suite)                    Raspberry Pi (target)
┌────────────────────────────┐   SSH/22  ┌────────────────────────────────┐
│ validate.py                │──────────>│ acadia-client.service          │
│  ├─ env    (shell checks)  │           │   ├─ HalService 127.0.0.1:50052│
│  ├─ proc   (systemctl…)    │  SSH TCP  │   ├─ diag /run/acadia/client.sock
│  ├─ diag   (socat/nc/py)   │  tunnel   │   ├─ spool /var/lib/acadia/... │
│  ├─ hal    (grpcio client) │──────────>│   └─ stream → Acadia-Server    │
│  └─ stream (mock server)   │  out:50051│<───────────── (outbound)       │
└────────────────────────────┘           └────────────────────────────────┘
```

The suite was written against a full code review of `src/` and validates the
following behaviour as-built:

| Subsystem | Validated contract |
|---|---|
| Binary CLI | `--version`, `--help`, unknown flag → exit 2, `--config` arg handling |
| `client.conf` | TOML parse, required `server_address`, `insecure` for plaintext stream, loopback/token rule (CLI-048) |
| Spool | SQLite WAL database present, WAL sidecar, client UUID identity |
| Diag socket | `/run/acadia/client.sock` status JSON: `connected`, `spool_bytes`, `active_runs`, `client_version` |
| HalService | `ListModules` (+ cross-check vs `modules.yaml`), `RunSingleShot` (happy + `INVALID_ARGUMENT` + `UNIMPLEMENTED` stub paths), `StartLongTerm`/`StartMonitoring`/`StartEventBased` streaming, interval floor (CLI-106), session cap → `RESOURCE_EXHAUSTED` (CLI-111), `Stop`/`Pause`/`Resume` incl. all-of-module semantics (CLI-110), `x-acadia-session-id` metadata, `authorization: Bearer` auth incl. loopback fail-closed (CLI-102) |
| Stream | `ClientService/Connect` bidi: `HelloReport` on connect + reconnect, `ConfigureAck` ok/error paths (unknown module, invalid mode), single-shot + long-term batch cadence, monotonic `batch_seq`, terminal `COMPLETED`/`CANCELLED`+`ABORTED`, pause/resume suppression, duplicate-StartRun rejection, ghost-StartRun tolerance, `BatchAck`/`LogBatchAck`, `StateReport` heartbeat, `LogBatch`, run-state persistence across reconnects, `x-acadia-stream-token` metadata |

## Layout

```
validation/
├── README.md             ← this file
├── requirements.txt      ← host-side Python dependencies
├── validate.py           ← CLI entry point
├── selftest.py           ← validates the tooling itself (NO device needed)
└── acadia_val/
    ├── core.py           ← result model, reporter, runner, reports
    ├── target.py         ← SSH transport, TCP tunnel, unix-socket probes
    ├── protogen.py       ← on-the-fly Python stubs from the live HAL protos
    ├── mock_server.py    ← mock Acadia-Server (Connect bidi) + scenarios
    ├── suite_env.py      ← Suite 1: environment
    ├── suite_process.py  ← Suite 2: process & service health
    ├── suite_diag.py     ← Suite 3: diag socket
    ├── suite_hal.py      ← Suite 4: HalService gRPC
    └── suite_stream.py   ← Suite 5: outbound stream vs mock Acadia-Server
```

## Setup (on the x86 host)

```sh
python3 -m pip install -r validation/requirements.txt
```

`grpcio`/`grpcio-tools` generate the Python gRPC stubs at run time **from the
live `Acadia-HAL/protos`** (sibling directory, auto-detected), so the suite
always validates the current proto contract. `paramiko` is preferred for SSH;
without it the suite falls back to the `ssh`/`sshpass` binaries.

## Quickstart

```sh
# everything, key-based SSH
python3 validation/validate.py --host 192.168.1.50 --user acadia --key ~/.ssh/id_ed25519

# password auth (env conventions match the repo's deploy tooling)
export ACADIA_TARGET_ADDRESS=192.168.1.50 ACADIA_TARGET_USER=acadia
export ACADIA_TARGET_PASS='secret'
python3 validation/validate.py

# quick pass (skips the 300 s state-report heartbeat and the session-cap test)
python3 validation/validate.py --host 192.168.1.50 --quick

# only some suites
python3 validation/validate.py --host 192.168.1.50 --only env,diag

# reports (CI-friendly)
python3 validation/validate.py --host 192.168.1.50 --junit report.xml --json report.json
```

Exit codes: `0` all checks passed (skips are fine), `1` at least one FAIL,
`2` setup/connection error.

### Suites

| Suite | What it does |
|---|---|
| `env` | SSH reachability, aarch64 check, binary presence + CLI contract, `client.conf` sanity (tomllib), `modules.yaml` population, client UUID, SQLite spool, systemd unit active/enabled, disk/memory |
| `proc` | MainPID, NRestarts, uptime, RSS/threads/fds, mapped libs (grpc/sqlite), idle CPU sample, coredumps, journal error scan, HalService port + bind check |
| `diag` | Socket file, probe via socat → nc → python3 (SSH cannot forward unix sockets), JSON contract, version match, newline termination, empty-query behaviour, 5 concurrent probes, `connected` flag |
| `hal` | Everything in the HalService table above, over an SSH tunnel to the device's loopback port |
| `stream` | Full outbound stream protocol against a mock Acadia-Server on this host (see below) |

### Stream suite preconditions

The device dials **out** to `server_address:server_port` from
`/etc/acadia/client.conf`. To validate the stream end-to-end the suite runs a
mock Acadia-Server on this host; the device must therefore be configured to
connect here with `insecure = true` (the client refuses plaintext otherwise).

* If the device conf already points at this host → the mock starts and the
  suite runs.
* Otherwise the suite **SKIPs with an actionable message**. To let the suite
  reconfigure the device temporarily, run with `--auto-provision-stream`:
  it backs up `client.conf` to `client.conf.valbak`, rewrites
  `server_address`/`server_port`/`insecure` (commenting out TLS keys),
  restarts the service, runs the scenarios, and **restores the original
  config + restarts the service** when done. This is opt-in because it
  restarts the daemon on a real device.

Scenario coverage (three connections):

1. **First connection** — hello (board_id, client_version, run states),
   stream token metadata, single-shot run end-to-end, unknown-module and
   invalid-mode error acks, bounded long-term run, pause/resume/stop with
   `CANCELLED`+`ABORTED` terminal batch, duplicate StartRun rejection, ghost
   StartRun tolerance. The server then drops the connection.
2. **Reconnect** — hello within the backoff window, persisted terminal run
   states, `StateReport` heartbeat (janitor default 300 s — the slowest
   check, skipped by `--quick`), `LogBatch` validation.
3. **Persistence** — hello again reports the `last_batch_seq` of a run whose
   batches were deliberately **never acked**.

## Options reference

```
--host/--user/--password/--key/--ssh-port    target connection
--binary/--config-path/--modules-yaml/--spool-path/--client-id-path/--diag-socket
--hal-port (50052) / --stream-port (50051)   device ports
--service (acadia-client.service)            systemd unit name
--module (cpu)                               module used for run/stream tests
--proto-dir                                  override HAL proto location
--only env,proc,diag,hal,stream              suite selection
--skip session_cap,pause_resume,...          skip named checks
--quick                                      skip slow checks
--state-report-timeout (330)                 StateReport wait budget
--auto-provision-stream                      opt-in temporary conf rewrite
--junit FILE / --json FILE                   reports
-v / --verbose                               show details for passing checks
```

Named skippable checks: `session_cap`, `pause_resume`, `nonloopback_auth`,
`state_report` (also skipped by `--quick`).

## Self-test (no device)

```sh
python3 validation/selftest.py
```

Runs the mock Acadia-Server against a fake acadia-client (hello, acks, batch
cadence, reconnect persistence, state reports, logs) and a fake HalService
against the real `run_hal_checks` — 68 assertions total. If this passes on
your host, the tooling itself is sound and any later failures are genuine
device findings.

## Notes and limitations

* The SSH tunnel to `127.0.0.1:50052` appears as a **loopback peer** on the
  device, which the HalService auth policy accepts when no
  `hal_service_auth_token` is configured. The semantic non-loopback
  rejection (CLI-102) is enforced by `require_auth` in the client and
  probed only informationally from the suite.
* The `StateReport` heartbeat test waits up to ~330 s because the janitor's
  default `sweep_interval` is 300 s.
* The suite only ever *reads* device state, except for the explicitly
  opt-in `--auto-provision-stream` (temporary, restored on exit, backup
  kept) and the stream suite's use of `systemctl restart`.
* Tests that execute HAL runs default to the `cpu` module; override with
  `--module` if your bench device requires a different safe module.
* Validation only: no `src/` changes are made by this tooling, per the
  leaf-repo agent boundary in `.claude/CLAUDE.md`.
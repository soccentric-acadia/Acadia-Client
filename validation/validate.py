#!/usr/bin/env python3
"""validate.py — remote validation suite for Acadia-Client.

Runs on an x86 Linux host and validates a deployed acadia-client on a
Raspberry Pi target entirely over SSH:

  env    — device environment, binary CLI contract, config, registry, spool
  proc   — systemd service & process health (memory, threads, coredumps…)
  diag   — diagnostic unix socket (status JSON contract)
  hal    — HalService gRPC API (via SSH tunnel; stubs from live HAL protos)
  stream — outbound ClientService stream against a mock Acadia-Server

Usage:
  python3 validation/validate.py --host 192.168.1.50 --user acadia
  python3 validation/validate.py --host 192.168.1.50 --only env,diag --quick
  python3 validation/validate.py --help

Credentials follow the repo conventions:
  ACADIA_TARGET_ADDRESS / ACADIA_TARGET_USER / ACADIA_TARGET_SSH_KEY /
  ACADIA_TARGET_PASS
"""

from __future__ import annotations

import argparse
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from acadia_val.core import Context, Runner, print_summary, write_json, write_junit
from acadia_val.target import Target

SUITES = [
    ("env", "acadia_val.suite_env", "EnvSuite"),
    ("proc", "acadia_val.suite_process", "ProcessSuite"),
    ("diag", "acadia_val.suite_diag", "DiagSuite"),
    ("hal", "acadia_val.suite_hal", "HalSuite"),
    ("stream", "acadia_val.suite_stream", "StreamSuite"),
]


def env_default(name: str, default: str = "") -> str:
    return os.environ.get(name, default)


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        prog="validate.py",
        description="Remote validation of acadia-client over SSH from an x86 host")
    tgt = p.add_argument_group("target (SSH)")
    tgt.add_argument("--host", default=env_default("ACADIA_TARGET_ADDRESS"),
                     help="target IP/hostname (env ACADIA_TARGET_ADDRESS)")
    tgt.add_argument("--user", default=env_default("ACADIA_TARGET_USER", "acadia"),
                     help="SSH user (env ACADIA_TARGET_USER, default acadia)")
    tgt.add_argument("--password", default=env_default("ACADIA_TARGET_PASS"),
                     help="SSH password (env ACADIA_TARGET_PASS)")
    tgt.add_argument("--key", default=env_default("ACADIA_TARGET_SSH_KEY"),
                     help="SSH private key path (env ACADIA_TARGET_SSH_KEY)")
    tgt.add_argument("--ssh-port", type=int, default=22)

    dev = p.add_argument_group("device paths & services")
    dev.add_argument("--binary", default="/usr/bin/acadia-client",
                     help="acadia-client binary path on device")
    dev.add_argument("--config-path", default="/etc/acadia/client.conf")
    dev.add_argument("--modules-yaml", default="/etc/acadia/modules.yaml")
    dev.add_argument("--spool-path", default="/var/lib/acadia/spool/spool.db")
    dev.add_argument("--client-id-path", default="/var/lib/acadia/client_id")
    dev.add_argument("--diag-socket", default="/run/acadia/client.sock")
    dev.add_argument("--hal-port", type=int, default=50052,
                     help="HalService port on device (tunnelled)")
    dev.add_argument("--stream-port", type=int, default=50051,
                     help="expected Acadia-Server port in device conf")
    dev.add_argument("--service", default="acadia-client.service")
    dev.add_argument("--module", default="cpu",
                     help="safe module id used for run/stream tests")
    dev.add_argument("--proto-dir", default="",
                     help="path to Acadia-HAL protos (default: auto-detect sibling)")

    sel = p.add_argument_group("selection & reporting")
    sel.add_argument("--only", default="",
                     help="comma list of suites to run (env,proc,diag,hal,stream)")
    sel.add_argument("--skip", default="",
                     help="comma list of named checks to skip "
                          "(e.g. session_cap,state_report,pause_resume,nonloopback_auth)")
    sel.add_argument("--quick", action="store_true",
                     help="skip slow checks (state report heartbeat, session cap)")
    sel.add_argument("--state-report-timeout", type=float, default=330.0,
                     help="seconds to wait for the 300s StateReport heartbeat")
    sel.add_argument("--auto-provision-stream", action="store_true",
                     help="temporarily rewrite device client.conf to point at this "
                          "host for the stream suite (backup + restore included)")
    sel.add_argument("--junit", default="", help="write JUnit XML report to file")
    sel.add_argument("--json", default="", help="write JSON report to file")
    sel.add_argument("--list", action="store_true", help="list suites and exit")
    sel.add_argument("-v", "--verbose", action="store_true")
    return p.parse_args(argv)


def load_suite_classes(only: str):
    import importlib
    names = [s.strip() for s in only.split(",") if s.strip()] if only \
        else [n for n, _m, _c in SUITES]
    classes = []
    for name in names:
        match = next((c for n, m, c in SUITES if n == name), None)
        if match is None:
            raise SystemExit(f"unknown suite '{name}' (valid: "
                             f"{', '.join(n for n, _, _ in SUITES)})")
        module_path = next(m for n, m, c in SUITES if n == name)
        mod = importlib.import_module(module_path)
        classes.append(getattr(mod, match))
    return names, classes


def main(argv=None) -> int:
    opts = parse_args(argv)
    if opts.list:
        for name, module, cls in SUITES:
            print(f"  {name:<6} {module}.{cls}")
        return 0

    if not opts.host:
        print("error: --host (or ACADIA_TARGET_ADDRESS) is required", file=sys.stderr)
        return 2

    opts.skip = [s.strip() for s in opts.skip.split(",") if s.strip()]
    names, suite_classes = load_suite_classes(opts.only)

    print(f"Acadia-Client remote validation — target {opts.user}@{opts.host} "
          f"(suites: {', '.join(names)})")

    from acadia_val.target import Target
    try:
        target = Target(opts.host, opts.user, password=opts.password or None,
                        key_file=opts.key or None, port=opts.ssh_port)
    except Exception as e:
        print(f"FATAL: cannot connect to {opts.user}@{opts.host}: {e}", file=sys.stderr)
        return 2

    ctx = Context(opts, target)
    results = []
    try:
        runner = Runner(ctx, suite_classes)
        results = runner.run()
    finally:
        target.close()

    ok = print_summary(results)
    if opts.json:
        from acadia_val.core import write_json
        write_json(results, opts.json)
        print(f"  JSON report: {opts.json}")
    if opts.junit:
        from acadia_val.core import write_junit
        write_junit(results, opts.junit)
        print(f"  JUnit report: {opts.junit}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
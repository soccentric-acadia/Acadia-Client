#!/usr/bin/env python3
"""selftest.py — validates the validation tooling itself, with NO device.

Two self-tests run against local fakes on the x86 host:

1. HAL self-test: a fake HalService (same semantics as HalServiceImpl.cc —
   auth, module/mode validation, envelope shape, streaming floor, sessions,
   pause/resume/stop, 8-session cap) is driven by the real run_hal_checks.
2. STREAM self-test: the mock Acadia-Server is driven by a fake
   acadia-client implementing the observable device protocol subset
   (hello/acks/batches/state reports/logs/reconnect persistence) using the
   exact three scenarios of suite_stream.py.

Both prove the tooling (proto generation, scenarios, checks) works before
you touch a real device.

Usage:  python3 validation/selftest.py
"""

from __future__ import annotations

import os
import queue
import sys
import threading
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import grpc
from concurrent import futures

from acadia_val import protogen
from acadia_val.core import Reporter, Status
from acadia_val.mock_server import (MockAcadiaServer, ScenarioCtx,
                                    scenario_first_connection,
                                    scenario_persistence, scenario_reconnect)

HAL_PORT = int(os.environ.get("ACADIA_SELFTEST_HAL_PORT", "50188"))
STREAM_PORT = int(os.environ.get("ACADIA_SELFTEST_PORT", "50187"))
BOARD_ID = "selftest-board"
CLIENT_VERSION = "0.9.9-selftest"


# ===========================================================================
# Part 1 — fake HalService + suite_hal checks
# ===========================================================================

SYNTHETIC_MODULES_YAML = """
modules:
  - id: cpu
    status: active
    modes:
      - SINGLE_SHOT
      - LONG_TERM
      - MONITORING
      - EVENT_BASED
  - id: temp_only
    status: active
    modes:
      - SINGLE_SHOT
      - EVENT_BASED
  - id: stub_thing
    status: active
"""

K_MAX_SESSIONS = 8


class FakeHalServer:
    """Mirrors HalServiceImpl.cc semantics closely enough for the checks."""

    def __init__(self, pb, port):
        import grpc
        self.pb = pb
        self.common = pb.common
        self.hal_pb = pb.hal_service
        self.sessions: dict = {}
        self.lock = threading.Lock()
        self.counter = 0
        self._server = grpc.server(
            futures.ThreadPoolExecutor(max_workers=16),
            options=[("grpc.so_reuseport", 0)])
        self._servicer = self._Servicer(self)
        pb.hal_service_grpc.add_HalServiceServicer_to_server(
            self._servicer, self._server)
        bound = self._server.add_insecure_port(f"127.0.0.1:{port}")
        if bound == 0:
            raise RuntimeError("fake hal server could not bind")
        self.port = port

    def start(self):
        self._server.start()

    def stop(self, grace=1.0):
        self._server.stop(grace)

    class _Servicer(object):
        def __init__(self, outer):
            self.outer = outer

        # -- helpers ---------------------------------------------------------
        def _auth(self, context):
            outer = self.outer
            # no token configured: loopback only (CLI-102 semantics)
            peer = context.peer() if context else ""
            if "127.0.0.1" not in peer and "[::1]" not in peer:
                context.abort(grpc.StatusCode.UNAUTHENTICATED,
                              "hal_service_auth_token must be configured")

        def _validate_module(self, context, module_id):
            if not module_id:
                context.abort(grpc.StatusCode.INVALID_ARGUMENT,
                              "module_id is required")
            if module_id == "no_such_module_xyz":
                context.abort(grpc.StatusCode.INVALID_ARGUMENT,
                              f"unknown module_id: {module_id}")
            if module_id == "stub_thing":
                context.abort(grpc.StatusCode.UNIMPLEMENTED,
                              "module 'stub_thing' has no HAL implementation")

        def _validate_mode(self, context, module_id, mode):
            outer = self.outer
            if module_id == "temp_only" and mode not in (
                    outer.common.SINGLE_SHOT, outer.common.EVENT_BASED):
                context.abort(grpc.StatusCode.INVALID_ARGUMENT,
                              f"module '{module_id}' does not support execution mode")

        def _env(self, module_id, mode, status=1):
            outer = self.outer
            env = outer.common.ResultEnvelope()
            env.module_id = module_id
            env.mode = mode
            env.status = status
            env.timestamp.GetCurrentTime()
            tr = outer.common.TestResult()
            tr.module_id = module_id
            tr.test_id = module_id + ".selftest"
            tr.name = "selftest test"
            tr.output.status = status
            env.payload.Pack(tr)
            return env

        def _stream(self, module_id, mode, cfg, context):
            outer = self.outer
            with outer.lock:
                if len(outer.sessions) >= K_MAX_SESSIONS:
                    context.abort(grpc.StatusCode.RESOURCE_EXHAUSTED,
                                  "too many concurrent streaming sessions (limit 8)")
                outer.counter += 1
                sid = f"sess-{module_id}-{outer.counter}"
                outer.sessions[sid] = {"stop": threading.Event(),
                                       "paused": threading.Event(),
                                       "module": module_id}
            context.send_initial_metadata([("x-acadia-session-id", sid)])
            interval = max(0.1, (cfg.interval_ms or 1000) / 1000.0)
            duration = (cfg.duration_ms or 0) / 1000.0
            run_forever = cfg.run_forever or duration <= 0
            t0 = time.monotonic()
            try:
                while True:
                    st = outer.sessions.get(sid)
                    if st is None:
                        break
                    if not context.is_active() or st["stop"].is_set():
                        break
                    if not run_forever and (time.monotonic() - t0) >= duration:
                        break
                    if st["paused"].is_set():
                        time.sleep(0.05)
                        continue
                    # EVENT_BASED streams nothing until the condition fires —
                    # the session stays open (mirrors the real adapters).
                    if mode != outer.common.EVENT_BASED:
                        yield self._env(module_id, mode)
                    st["stop"].wait(interval)
            finally:
                with outer.lock:
                    outer.sessions.pop(sid, None)

        # -- RPCs -------------------------------------------------------------
        def ListModules(self, request, context):
            self._auth(context)
            r = self.outer.hal_pb.ListModulesResponse()
            for mid, status in (("cpu", "active"), ("stub_thing", "unimplemented"),
                                ("temp_only", "active")):
                m = r.modules.add()
                m.id = mid
                m.proto_package = f"hal.modules.{mid}"
                m.config_type = f"type.googleapis.com/hal.modules.{mid}.Config"
                m.result_type = f"type.googleapis.com/hal.modules.{mid}.Result"
                m.version = 1
                m.status = status
                m.source_path = f"src/{mid}.h"
            return r

        def RunSingleShot(self, request, context):
            self._auth(context)
            self._validate_module(context, request.module_id)
            self._validate_mode(context, request.module_id,
                                self.outer.common.SINGLE_SHOT)
            return self._env(request.module_id, self.outer.common.SINGLE_SHOT)

        def StartLongTerm(self, request, context):
            self._auth(context)
            self._validate_module(context, request.module_id)
            self._validate_mode(context, request.module_id,
                                self.outer.common.LONG_TERM)
            return self._stream(request.module_id, self.outer.common.LONG_TERM,
                                request.common, context)

        def StartMonitoring(self, request, context):
            self._auth(context)
            self._validate_module(context, request.module_id)
            self._validate_mode(context, request.module_id,
                                self.outer.common.MONITORING)
            return self._stream(request.module_id, self.outer.common.MONITORING,
                                request.common, context)

        def StartEventBased(self, request, context):
            self._auth(context)
            self._validate_module(context, request.module_id)
            self._validate_mode(context, request.module_id,
                                self.outer.common.EVENT_BASED)
            return self._stream(request.module_id, self.outer.common.EVENT_BASED,
                                request.common, context)

        def Stop(self, request, context):
            self._auth(context)
            r = self.outer.hal_pb.StopResponse()
            with self.outer.lock:
                targets = [sid for sid, s in self.outer.sessions.items()
                           if s["module"] == request.module_id
                           and (not request.session_id or sid == request.session_id)]
                for sid in targets:
                    self.outer.sessions[sid]["stop"].set()
            if targets:
                r.accepted = True
            else:
                r.accepted = False
                r.message = f"no active session for module_id={request.module_id}"
            return r

        def Pause(self, request, context):
            self._auth(context)
            r = self.outer.hal_pb.PauseResponse()
            with self.outer.lock:
                targets = [sid for sid, s in self.outer.sessions.items()
                           if s["module"] == request.module_id
                           and (not request.session_id or sid == request.session_id)]
                for sid in targets:
                    self.outer.sessions[sid]["paused"].set()
            r.accepted = bool(targets)
            return r

        def Resume(self, request, context):
            self._auth(context)
            r = self.outer.hal_pb.ResumeResponse()
            with self.outer.lock:
                targets = [sid for sid, s in self.outer.sessions.items()
                           if s["module"] == request.module_id
                           and (not request.session_id or sid == request.session_id)]
                for sid in targets:
                    self.outer.sessions[sid]["paused"].clear()
            r.accepted = bool(targets)
            return r


class _FakeOpts:
    module = "cpu"
    modules_yaml = "/tmp/fake-modules.yaml"
    skip = ["nonloopback_auth"]
    quick = False
    proto_dir = ""


class _FakeTarget:
    def __init__(self, yaml_text):
        self._yaml = yaml_text

    def read_file(self, path, use_sudo=False):
        return self._yaml

    def run_ok(self, cmd):
        return None


class _FakeCtx:
    def __init__(self, yaml_text):
        self.opts = _FakeOpts()
        self.facts: dict = {}
        self.target = _FakeTarget(yaml_text)
        self.yaml_provider = lambda: yaml_text

    def fact(self, key, default=None):
        return self.facts.get(key, default)


def run_hal_selftest(pb, rep) -> bool:
    """Drive run_hal_checks against the fake HalService."""
    import grpc
    from acadia_val.suite_hal import run_hal_checks
    server = FakeHalServer(pb, HAL_PORT)
    server.start()
    rep.record("fake HalService listening", Status.PASS, f"127.0.0.1:{HAL_PORT}")
    channel = grpc.insecure_channel(f"127.0.0.1:{HAL_PORT}")
    grpc.channel_ready_future(channel).result(timeout=10)
    stub = pb.hal_service_grpc.HalServiceStub(channel)
    ctx = _FakeCtx(SYNTHETIC_MODULES_YAML)
    try:
        run_hal_checks(rep, ctx, stub, pb, pb.common, lambda extra=None: [], "")
        ok = not any(r.status is Status.FAIL for r in rep.results)
    finally:
        channel.close()
        server.stop()
    return ok


# ===========================================================================
# Part 2 — fake acadia-client vs mock Acadia-Server (stream scenarios)
# ===========================================================================

class OutStream:
    """Iterator handed to the gRPC stub for the client's outbound messages."""

    def __init__(self):
        self.q: "queue.Queue" = queue.Queue()
        self.closed = False

    def __iter__(self):
        return self

    def __next__(self):
        while True:
            try:
                return self.q.get(timeout=0.2)
            except queue.Empty:
                if self.closed:
                    raise StopIteration

    def put(self, msg):
        self.q.put(msg)

    def close(self):
        self.closed = True


class FakeClient:
    """Minimal acadia-client protocol implementation (x86 reference model)."""

    def __init__(self, pb, port, module="cpu", token=""):
        import grpc
        self.pb = pb
        self.module = module
        self.token = token
        self.channel = grpc.insecure_channel(f"127.0.0.1:{port}")
        self.stub = pb.client_service_grpc.ClientServiceStub(self.channel)
        self.run_states: dict = {}   # run_id -> {"state": int, "last_seq": int}
        self.batch_seq: dict = {}    # run_id -> next batch seq (starts at 1)
        self.configs: dict = {}      # run_id -> configure message
        self.live: dict = {}         # run_id -> control dict
        self.live_lock = threading.Lock()
        self.acked: set = set()
        self.stop_flag = threading.Event()
        self._threads: list = []

    def start(self):
        th = threading.Thread(target=self._run_forever, daemon=True)
        th.start()
        self._threads.append(th)

    def shutdown(self):
        self.stop_flag.set()
        with self.live_lock:
            for ctl in self.live.values():
                ctl["stop"].set()
        try:
            self.channel.close()
        except Exception:
            pass

    def _run_forever(self):
        while not self.stop_flag.is_set():
            try:
                self._one_session()
            except Exception:
                pass
            time.sleep(0.4)  # reconnect delay (client backoff analogue)

    def _one_session(self):
        out = OutStream()
        md = [("x-acadia-stream-token", self.token)] if self.token else None
        call = self.stub.Connect(out, metadata=md)
        self._send_hello(out)

        reader_done = threading.Event()

        def reader():
            try:
                for msg in call:
                    self._on_msg(msg, out)
            except Exception:
                pass
            finally:
                reader_done.set()

        th = threading.Thread(target=reader, daemon=True)
        th.start()
        sent_logs = False
        while not reader_done.is_set():
            time.sleep(0.5)
            if reader_done.is_set():
                break
            self._send_state_report(out)
            if not sent_logs:
                self._send_log_batch(out)
                sent_logs = True
        out.close()

    def _send_hello(self, out):
        m = self.pb.client_service.ClientMessage()
        h = m.hello
        h.board_id = BOARD_ID
        h.client_version = CLIENT_VERSION
        with self.live_lock:
            for run_id, st in self.run_states.items():
                ai = h.runs.add()
                ai.run_id = run_id
                ai.state = st["state"]
                ai.last_batch_seq = st["last_seq"]
        out.put(m)

    def _send_state_report(self, out):
        m = self.pb.client_service.ClientMessage()
        r = m.state_report
        r.board_id = BOARD_ID
        with self.live_lock:
            r.overall_state = 2 if self.live else 1
            for run_id, st in self.run_states.items():
                ai = r.active_runs.add()
                ai.run_id = run_id
                ai.state = st["state"]
                ai.last_batch_seq = st["last_seq"]
        r.spool_bytes = 4096
        r.spool_cap_bytes = 2 * 1024 * 1024 * 1024
        r.spool_used_bytes = 4096
        r.spool_pressure = 4096 / r.spool_cap_bytes
        out.put(m)

    def _send_log_batch(self, out):
        m = self.pb.client_service.ClientMessage()
        lb = m.log_batch
        lb.run_id = "_system"
        lb.batch_seq = 1
        line = lb.lines.add()
        line.run_id = "_system"
        line.source = "client"
        line.timestamp.GetCurrentTime()
        line.level = "info"
        line.text = "selftest log line"
        out.put(m)

    def _on_msg(self, msg, out):
        if msg.HasField("configure_run"):
            self._on_configure(msg.configure_run, out)
        elif msg.HasField("start_run"):
            self._on_start(msg.start_run.run_id, out)
        elif msg.HasField("pause_run"):
            with self.live_lock:
                ctl = self.live.get(msg.pause_run.run_id)
                if ctl:
                    ctl["paused"].set()
                    self.run_states[msg.pause_run.run_id]["state"] = 3
        elif msg.HasField("resume_run"):
            with self.live_lock:
                ctl = self.live.get(msg.resume_run.run_id)
                if ctl:
                    ctl["paused"].clear()
                    self.run_states[msg.resume_run.run_id]["state"] = 2
        elif msg.HasField("stop_run"):
            with self.live_lock:
                ctl = self.live.get(msg.stop_run.run_id)
                if ctl:
                    ctl["stop"].set()
        elif msg.HasField("batch_ack"):
            self.acked.add((msg.batch_ack.run_id, msg.batch_ack.batch_seq))

    def _on_configure(self, cr, out):
        m = self.pb.client_service.ClientMessage()
        ack = m.configure_ack
        ack.run_id = cr.run_id
        if cr.module_id == "no_such_module_xyz":
            ack.ok = False
            ack.error = f"unknown module_id: {cr.module_id}"
            out.put(m)
            return
        if cr.mode not in (1, 2, 3, 4):
            ack.ok = False
            ack.error = f"unknown execution mode: {cr.mode}"
            out.put(m)
            return
        self.configs[cr.run_id] = cr
        self.run_states[cr.run_id] = {"state": 1, "last_seq": 0}
        self.batch_seq[cr.run_id] = 1
        ack.ok = True
        out.put(m)

    def _on_start(self, run_id, out):
        if run_id in self.live:
            m = self.pb.client_service.ClientMessage()
            ack = m.configure_ack
            ack.run_id = run_id
            ack.ok = False
            ack.error = "duplicate run_id already running"
            out.put(m)
            return
        if run_id not in self.configs:
            return  # ghost StartRun: tolerated, ignored
        cr = self.configs[run_id]
        ctl = {"stop": threading.Event(), "paused": threading.Event(),
               "interval_ms": cr.common.interval_ms or 200,
               "mode": cr.mode,
               "duration_ms": cr.common.duration_ms,
               "run_forever": cr.common.run_forever or cr.common.duration_ms <= 0}
        with self.live_lock:
            self.live[run_id] = ctl
            self.run_states[run_id] = {"state": 2, "last_seq": 0}
        th = threading.Thread(target=self._driver, args=(run_id, ctl, out),
                              daemon=True)
        th.start()
        self._threads.append(th)

    def _batch(self, run_id, ctl, terminal, state, statuses):
        m = self.pb.client_service.ClientMessage()
        b = m.result_batch
        b.run_id = run_id
        seq = self.batch_seq[run_id]
        self.batch_seq[run_id] = seq + 1
        b.batch_seq = seq
        b.module_id = self.module
        b.mode = ctl["mode"]
        b.state = state
        b.terminal = terminal
        for st in statuses:
            b.samples.add(status=st)
        return m, seq

    def _emit(self, out, run_id, ctl, terminal, state, statuses=(1,)):
        m, seq = self._batch(run_id, ctl, terminal, state, statuses)
        out.put(m)
        st = self.run_states.setdefault(run_id, {"state": 2, "last_seq": 0})
        st["state"] = state
        st["last_seq"] = seq
        return seq

    def _driver(self, run_id, ctl, out):
        mode = ctl["mode"]
        if mode == 1:  # single shot: one terminal COMPLETED batch
            self._emit(out, run_id, ctl, True, 4)
            return
        interval = max(0.05, ctl["interval_ms"] / 1000.0)
        started = time.monotonic()
        while not ctl["stop"].is_set():
            if not ctl["paused"].is_set():
                if ctl["run_forever"]:
                    self._emit(out, run_id, ctl, False, 2)
                else:
                    if (time.monotonic() - started) * 1000 >= ctl["duration_ms"]:
                        break
                    self._emit(out, run_id, ctl, False, 2)
            ctl["stop"].wait(interval)
        with self.live_lock:
            self.live.pop(run_id, None)
        if ctl["stop"].is_set():
            # server stop → terminal CANCELLED with an ABORTED sample; state deleted
            self._emit(out, run_id, ctl, True, 6, (5,))
            self.run_states.pop(run_id, None)  # on_stop deletes run state
        else:
            # bounded run finished on its own → terminal COMPLETED
            self._emit(out, run_id, ctl, True, 4)


def run_stream_selftest(pb, rep) -> bool:
    sctx = MockScenarioCtx(pb)
    mock = MockAcadiaServer(pb, STREAM_PORT,
                            [scenario_first_connection,
                             scenario_reconnect,
                             scenario_persistence],
                            sctx, bind="127.0.0.1")
    mock.start()
    rep.record("mock Acadia-Server listening", Status.PASS, f"127.0.0.1:{STREAM_PORT}")

    fake = FakeClient(pb, STREAM_PORT, module="cpu")
    fake.start()
    try:
        deadline = time.monotonic() + 180
        while not mock.sessions and time.monotonic() < deadline:
            time.sleep(0.5)
        s1 = mock.sessions[0] if mock.sessions else None
        assert s1 is not None, "fake client never connected"
        deadline2 = time.monotonic() + 120
        while not s1.scenario_done.is_set() and time.monotonic() < deadline2:
            time.sleep(0.5)
        s1.context.cancel()

        deadline3 = time.monotonic() + 60
        while len(mock.sessions) < 2 and time.monotonic() < deadline3:
            time.sleep(0.5)
        s2 = mock.sessions[1] if len(mock.sessions) >= 2 else None
        assert s2 is not None, "no reconnect (connection 2)"
        d4 = time.monotonic() + 60
        while not s2.scenario_done.is_set() and time.monotonic() < d4:
            time.sleep(0.5)
        s2.context.cancel()

        deadline4 = time.monotonic() + 60
        while len(mock.sessions) < 3 and time.monotonic() < deadline4:
            time.sleep(0.5)
        s3 = mock.sessions[2] if len(mock.sessions) >= 3 else None
        assert s3 is not None, "no third connection"
        d5 = time.monotonic() + 60
        while not s3.scenario_done.is_set() and time.monotonic() < d5:
            time.sleep(0.5)
    finally:
        fake.shutdown()
        mock.stop(grace=1.0)

    failed = 0
    for step in mock.all_steps():
        status = Status.WARN if step.warn else (
            Status.PASS if step.ok else Status.FAIL)
        rep.record(f"stream: {step.name}", status, step.detail)
        if status is Status.FAIL:
            failed += 1
    return failed == 0


def MockScenarioCtx(pb, module="cpu"):  # noqa: N802 — kept near usage
    from acadia_val.mock_server import ScenarioCtx
    sctx = ScenarioCtx(pb, module=module)
    sctx.expected_board_id = BOARD_ID
    sctx.expected_client_version = CLIENT_VERSION
    sctx.state_report_timeout = 8.0
    sctx.quick = False
    return sctx


# ===========================================================================
# main
# ===========================================================================

def main() -> int:
    print("== acadia_val selftest (no device required) ==")
    protos = protogen.generate()
    pb = protos
    print(f"protos generated from: {protos.proto_src}")


    all_ok = True

    from acadia_val.core import Reporter, Status
    all_ok = True

    print("\n[suite] hal-selftest (fake HalService)")
    rep = Reporter("hal-selftest", verbose=False)
    ok = run_hal_selftest(pb, rep)
    p, f, w, _s = rep.summary()
    print(f"hal selftest: {p} passed, {f} failed, {w} warned — "
          f"{'PASS' if ok else 'FAIL'}")
    if f:
        for r in rep.results:
            if r.status is Status.FAIL:
                print(f"  FAIL {r.name} — {(r.detail or '').strip()[:160]}")
    all_ok &= ok

    print("\n[suite] stream-selftest")
    rep2 = Reporter("stream-selftest", verbose=False)
    ok2 = run_stream_selftest(pb, rep2)
    p, f, w, _s = rep2.summary()
    print(f"stream selftest: {p} passed, {f} failed, {w} warned — "
          f"{'PASS' if ok2 else 'FAIL'}")
    if f:
        for r in rep2.results:
            if r.status is Status.FAIL:
                print(f"  FAIL {r.name} — {(r.detail or '').strip()[:160]}")
    all_ok &= ok2

    print("\nSELFTEST:", "PASS" if all_ok else "FAIL")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
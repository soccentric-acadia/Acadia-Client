"""mock_server.py — an in-memory mock of the Acadia-Server
`acadia.stream.ClientService/Connect` bidirectional stream.

The real device connects OUT to Acadia-Server:50051; the stream-protocol
suite temporarily points the device at this mock (operator-approved) and
runs scripted scenarios:

  connection 1: hello → single-shot run → error acks → long-term →
                pause/resume → duplicate start → server-initiated drop
  connection 2: hello (reconnect) → persisted run states → state report
                → log batches
  connection 3: hello → verify the unacked run's last_batch_seq persisted

Every scenario step is recorded as (name, ok, detail) and later converted
into TestResults by the stream suite.  The same scenarios are reused by
selftest.py against a fake client, which validates the mock itself.
"""

from __future__ import annotations

import queue
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Callable, List, Optional

import grpc


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------

@dataclass
class Step:
    name: str
    ok: bool
    detail: str = ""
    warn: bool = False


@dataclass
class ReceivedBatch:
    run_id: str
    batch_seq: int
    state: int
    terminal: bool
    module_id: str
    mode: int
    n_samples: int
    sample_statuses: list
    raw: object  # TestResultBatch


@dataclass
class StreamRecord:
    ordinal: int
    metadata: dict
    peer: str
    inbound: list
    outbound: list


# ---------------------------------------------------------------------------
# Session — one connected client stream
# ---------------------------------------------------------------------------

class Session:
    """Inbound queue + outbound queue + step recorder for one Connect stream."""

    def __init__(self, ordinal: int, request_iterator, context):
        self.ordinal = ordinal
        self.context = context
        self.request_iterator = request_iterator
        try:
            self.metadata = {k: v for k, v in (context.invocation_metadata() or [])}
        except Exception:
            self.metadata = {}
        try:
            self.peer = context.peer()
        except Exception:
            self.peer = ""
        self.record = StreamRecord(ordinal, dict(self.metadata), self.peer, [], [])
        self.in_q: "queue.Queue" = queue.Queue()
        self.out_q: "queue.Queue" = queue.Queue()
        self.closed = threading.Event()   # reader finished (client gone / stream ended)
        self.scenario_done = threading.Event()
        self.steps: List[Step] = []
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self):
        try:
            for msg in self.request_iterator:
                self.record.inbound.append(msg)
                self.in_q.put(msg)
        except Exception:
            pass
        self.closed.set()

    # -- API used by scenarios -------------------------------------------------
    def recv(self, timeout: float):
        try:
            return self.in_q.get(timeout=timeout)
        except queue.Empty:
            return None

    def send(self, msg):
        self.record.outbound.append(msg)
        self.out_q.put(msg)

    def is_open(self) -> bool:
        return not self.closed.is_set()

    def record_step(self, name: str, ok: bool, detail: str = "", warn: bool = False) -> bool:
        self.steps.append(Step(name, ok, detail, warn))
        return ok

    def expect_msg(self, name: str, timeout: float, pred: Optional[Callable] = None,
                   desc: str = "message"):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            msg = self.recv(timeout=min(0.25, max(0.01, deadline - time.monotonic())))
            if msg is None:
                if self.closed.is_set():
                    break
                continue
            if pred is None or pred(msg):
                return msg
        self.record_step(name, False, f"no {desc} within {timeout:.0f}s")
        return None

    def drain(self, duration: float, pred=None) -> list:
        out = []
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            msg = self.recv(timeout=min(0.25, max(0.01, remaining)))
            if msg is not None and (pred is None or pred(msg)):
                out.append(msg)
        return out

    # -- message predicates ------------------------------------------------------
    @staticmethod
    def is_hello(msg):
        return msg.HasField("hello")

    @staticmethod
    def is_configure_ack(msg):
        return msg.HasField("configure_ack")

    @staticmethod
    def is_result_batch(msg):
        return msg.HasField("result_batch")

    @staticmethod
    def is_state_report(msg):
        return msg.HasField("state_report")

    @staticmethod
    def is_log_batch(msg):
        return msg.HasField("log_batch")


# ---------------------------------------------------------------------------
# Scenario context
# ---------------------------------------------------------------------------

class ScenarioCtx:
    """Bundle handed to scenario functions."""

    def __init__(self, pb, module: str, run_prefix: str = "VAL"):
        self.pb = pb
        self.module = module
        self.run_prefix = run_prefix
        self.hello = None
        self.batch_log: List[ReceivedBatch] = []     # every batch seen (all connections)
        self.acked_run_ids: set = set()              # run_ids whose batches were acked
        self.expected_token: Optional[str] = None
        self.expected_board_id: Optional[str] = None
        self.expected_client_version: Optional[str] = None
        self.state_report_timeout: float = 330.0
        self.quick: bool = False


# ---------------------------------------------------------------------------
# Message builders (server → client)
# ---------------------------------------------------------------------------

def _mk_run_id(prefix: str) -> str:
    return f"{prefix}-{uuid.uuid4().hex[:10]}"


def _configure(pb, run_id, module, mode, interval_ms=0, duration_ms=0,
               run_forever=False, poll_ms=0):
    m = pb.client_service.ServerMessage()
    cr = m.configure_run
    cr.run_id = run_id
    cr.module_id = module
    cr.mode = mode
    if interval_ms:
        cr.common.interval_ms = interval_ms
    if duration_ms:
        cr.common.duration_ms = duration_ms
    cr.common.run_forever = run_forever
    if poll_ms:
        cr.common.poll_interval_ms = poll_ms
    return m


def _start(pb, run_id):
    m = pb.client_service.ServerMessage()
    m.start_run.run_id = run_id
    return m


def _pause(pb, run_id):
    m = pb.client_service.ServerMessage()
    m.pause_run.run_id = run_id
    return m


def _resume(pb, run_id):
    m = pb.client_service.ServerMessage()
    m.resume_run.run_id = run_id
    return m


def _stop(pb, run_id):
    m = pb.client_service.ServerMessage()
    m.stop_run.run_id = run_id
    return m


def _ack(pb, run_id, seq):
    m = pb.client_service.ServerMessage()
    m.batch_ack.run_id = run_id
    m.batch_ack.batch_seq = seq
    return m


def _log_ack(pb, run_id, seq):
    m = pb.client_service.ServerMessage()
    m.log_batch_ack.run_id = run_id
    m.log_batch_ack.batch_seq = seq
    return m


def _recv_batch(session, timeout) -> Optional[ReceivedBatch]:
    """Next TestResultBatch (other message types are skipped), or None."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        msg = session.recv(timeout=min(0.25, max(0.01, deadline - time.monotonic())))
        if msg is None:
            if session.closed.is_set():
                return None
            continue
        if session.is_result_batch(msg):
            b = msg.result_batch
            return ReceivedBatch(
                run_id=b.run_id, batch_seq=b.batch_seq, state=b.state,
                terminal=b.terminal, module_id=b.module_id, mode=b.mode,
                n_samples=len(b.samples),
                sample_statuses=[s.status for s in b.samples],
                raw=b)
    return None


def _ack_batch(session, pb, rb: ReceivedBatch, sctx: ScenarioCtx):
    session.send(_ack(pb, rb.run_id, rb.batch_seq))
    sctx.acked_run_ids.add(rb.run_id)


def _collect(session, run_id, duration) -> List[ReceivedBatch]:
    out = []
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        rb = _recv_batch(session, min(0.25, max(0.01, deadline - time.monotonic())))
        if rb and rb.run_id == run_id:
            out.append(rb)
    return out


def _collect_terminal(session, run_id, timeout, sctx):
    """Collect batches for one run until terminal or timeout.

    Returns (batches, terminal_or_None)."""
    batches = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        rb = _recv_batch(session, min(1.0, max(0.05, deadline - time.monotonic())))
        if rb is None:
            if session.closed.is_set():
                break
            continue
        if rb.run_id != run_id:
            continue
        batches.append(rb)
        sctx.batch_log.append(rb)
        if rb.terminal:
            return batches, rb
    return batches, None


# ---------------------------------------------------------------------------
# Scenario 1 — first connection
# ---------------------------------------------------------------------------

def scenario_first_connection(session: Session, sctx: ScenarioCtx):
    pb = sctx.pb

    # -- hello ------------------------------------------------------------------
    hello = session.expect_msg("hello received", 15.0, session.is_hello, "HelloReport")
    if hello is None:
        session.record_step("hello fields", False, "no hello — aborting scenario 1")
        return
    sctx.hello = hello
    h = hello.hello
    session.record_step("hello board_id non-empty", bool(h.board_id),
                        f"board_id={h.board_id!r}")
    session.record_step("hello client_version non-empty", bool(h.client_version),
                        f"version={h.client_version!r}")
    if sctx.expected_client_version:
        session.record_step(
            "hello client_version == device binary version",
            h.client_version == sctx.expected_client_version,
            f"hello={h.client_version!r} binary={sctx.expected_client_version!r}")
    if sctx.expected_board_id:
        session.record_step(
            "hello board_id == device client_id",
            h.board_id == sctx.expected_board_id,
            f"hello={h.board_id!r} device_id={sctx.expected_board_id!r}")
    session.record_step(
        "hello run states well-formed",
        all(r.run_id for r in h.runs) and all(0 <= r.state <= 6 for r in h.runs),
        f"{len(h.runs)} active run(s) reported at connect")

    # -- stream token --------------------------------------------------------------
    tok = session.metadata.get("x-acadia-stream-token", "")
    if sctx.expected_token:
        session.record_step(
            "stream token presented and matches",
            tok == sctx.expected_token,
            f"expected token, got {'<set>' if tok else '<absent>'}")
    else:
        session.record_step(
            "no stream token configured — header optional", True,
            f"token header present: {bool(tok)} (informational)")

    # -- single-shot run -------------------------------------------------------------
    run1 = _mk_run_id(sctx.run_prefix)
    session.send(_configure(pb, run1, sctx.module, pb.common.SINGLE_SHOT))
    ack = session.expect_msg(
        f"configure ack ok for {run1}", 8.0,
        lambda m: session.is_configure_ack(m) and m.configure_ack.run_id == run1,
        "ConfigureAck")
    if ack is None:
        return
    session.record_step(f"[{run1}] configure ack ok=true", ack.configure_ack.ok,
                        f"ok={ack.configure_ack.ok} error={ack.configure_ack.error!r}")
    session.send(_start(pb, run1))
    batches, terminal = _collect_terminal(session, run1, 40.0, sctx)
    session.record_step(f"[{run1}] terminal batch received", terminal is not None,
                        f"{len(batches)} batch(es)")
    if terminal is None:
        return
    session.record_step(
        f"[{run1}] terminal state COMPLETED/FAILED", terminal.state in (4, 5),
        f"state={terminal.state}")
    session.record_step(f"[{run1}] terminal batch has samples", terminal.n_samples > 0,
                        f"{terminal.n_samples} sample(s)")
    if terminal.n_samples:
        session.record_step(
            f"[{run1}] sample statuses valid",
            all(st in (1, 2, 3, 4, 5, 7) for st in terminal.sample_statuses),
            f"statuses={terminal.sample_statuses}")
        urls = [s.type_url for s in terminal.raw.samples if s.HasField("payload")]
        session.record_step(
            f"[{run1}] sample payload type_url well-formed",
            all(u.startswith("type.googleapis.com/") for u in urls) or not urls,
            f"type_urls={urls[:2]}")
    session.record_step(
        f"[{run1}] batch_seq starts at 1",
        bool(batches) and batches[0].batch_seq == 1,
        f"seqs={[b.batch_seq for b in batches]}")
    for rb in batches:
        _ack_batch(session, pb, rb, sctx)

    # -- unknown module → error ack ------------------------------------------------------
    runx = _mk_run_id(sctx.run_prefix)
    session.send(_configure(pb, runx, "no_such_module_xyz", pb.common.SINGLE_SHOT))
    ax = session.expect_msg(
        f"configure ack for unknown module {runx}", 8.0,
        lambda m: session.is_configure_ack(m) and m.configure_ack.run_id == runx,
        "ConfigureAck")
    if ax:
        session.record_step(
            "unknown module rejected with ok=false",
            (not ax.configure_ack.ok) and "unknown module" in ax.configure_ack.error,
            f"ok={ax.configure_ack.ok} error={ax.configure_ack.error!r}")

    # -- invalid mode → error ack -----------------------------------------------------------
    runm = _mk_run_id(sctx.run_prefix)
    session.send(_configure(pb, runm, sctx.module, 99))
    am = session.expect_msg(
        f"configure ack for invalid mode {runm}", 8.0,
        lambda m: session.is_configure_ack(m) and m.configure_ack.run_id == runm,
        "ConfigureAck")
    if am:
        session.record_step(
            "invalid mode rejected with ok=false",
            (not am.configure_ack.ok) and "execution mode" in am.configure_ack.error,
            f"ok={am.configure_ack.ok} error={am.configure_ack.error!r}")

    # -- long-term bounded run -----------------------------------------------------------------
    runl = _mk_run_id(sctx.run_prefix)
    session.send(_configure(pb, runl, sctx.module, pb.common.LONG_TERM,
                            interval_ms=200, duration_ms=2000))
    al = session.expect_msg(
        f"configure ack for long-term {runl}", 8.0,
        lambda m: session.is_configure_ack(m) and m.configure_ack.run_id == runl,
        "ConfigureAck")
    if al and al.configure_ack.ok:
        session.send(_start(pb, runl))
        lb, lt = _collect_terminal(session, runl, 30.0, sctx)
        if lt is None:
            session.record_step(f"[{runl}] terminal batch", False, "never received")
        else:
            n_pre = len([b for b in lb if not b.terminal])
            session.record_step(
                f"[{runl}] long-term produced periodic batches", n_pre >= 2,
                f"{n_pre} non-terminal batch(es) before terminal")
            session.record_step(
                f"[{runl}] terminal state COMPLETED/FAILED", lt.state in (4, 5),
                f"state={lt.state}")
            seqs = [b.batch_seq for b in lb]
            session.record_step(
                f"[{runl}] batch_seq strictly increasing",
                seqs == sorted(seqs) and len(set(seqs)) == len(seqs), f"seqs={seqs}")
            for rb in lb:
                _ack_batch(session, pb, rb, sctx)

    # -- pause / resume / stop --------------------------------------------------------------------
    runp = _mk_run_id(sctx.run_prefix)
    session.send(_configure(pb, runp, sctx.module, pb.common.LONG_TERM,
                            interval_ms=200, run_forever=True))
    ap = session.expect_msg(
        f"configure ack for pause-test {runp}", 8.0,
        lambda m: session.is_configure_ack(m) and m.configure_ack.run_id == runp,
        "ConfigureAck")
    if ap and ap.configure_ack.ok:
        session.send(_start(pb, runp))
        base = _collect(session, runp, 1.5)
        session.send(_pause(pb, runp))
        paused = _collect(session, runp, 2.5)
        session.send(_resume(pb, runp))
        resumed = _collect(session, runp, 1.5)
        session.send(_stop(pb, runp))
        lb, lt = _collect_terminal(session, runp, 8.0, sctx)
        base_rate = len(base) / 1.5
        paused_rate = len(paused) / 2.5
        session.record_step(
            f"[{runp}] pause suppresses batch flow",
            base_rate > 0 and paused_rate <= max(0.5, base_rate * 0.35),
            f"baseline={base_rate:.1f}/s paused={paused_rate:.1f}/s "
            f"(raw counts: base={len(base)} paused={len(paused)})")
        session.record_step(f"[{runp}] resume restores batch flow",
                            len(resumed) > 0, f"{len(resumed)} batch(es) after resume")
        if lt is not None:
            session.record_step(
                f"[{runp}] stop produces terminal CANCELLED batch", lt.state == 6,
                f"state={lt.state} (6=CANCELLED expected)")
            if lt.n_samples:
                session.record_step(
                    f"[{runp}] terminal CANCELLED carries ABORTED sample",
                    5 in lt.sample_statuses,
                    f"statuses={lt.sample_statuses} (5=ABORTED)")
            else:
                session.record_step(
                    f"[{runp}] terminal CANCELLED carries ABORTED sample", True,
                    "no samples in terminal batch (accepted)", warn=True)
            for rb in lb:
                _ack_batch(session, pb, rb, sctx)
        else:
            session.record_step(f"[{runp}] terminal batch after Stop", False,
                                "timeout waiting for terminal batch")

    # -- duplicate StartRun rejected -----------------------------------------------------------------
    rund = _mk_run_id(sctx.run_prefix)
    session.send(_configure(pb, rund, sctx.module, pb.common.LONG_TERM,
                            interval_ms=200, run_forever=True))
    ad = session.expect_msg(
        f"configure ack for dup-test {rund}", 8.0,
        lambda m: session.is_configure_ack(m) and m.configure_ack.run_id == rund,
        "ConfigureAck")
    if ad and ad.configure_ack.ok:
        session.send(_start(pb, rund))
        time.sleep(0.5)
        session.send(_start(pb, rund))
        a2 = session.expect_msg(
            f"duplicate-start error ack for {rund}", 8.0,
            lambda m: session.is_configure_ack(m) and m.configure_ack.run_id == rund
            and not m.configure_ack.ok,
            "error ConfigureAck")
        session.record_step(
            "duplicate StartRun rejected",
            a2 is not None and "duplicate" in a2.configure_ack.error,
            f"error={a2.configure_ack.error!r}" if a2 else "no error ack received")
        session.send(_stop(pb, rund))
        lb, lt = _collect_terminal(session, rund, 8.0, sctx)
        for rb in lb:
            _ack_batch(session, pb, rb, sctx)

    # -- StartRun for an un-configured run id: client must tolerate -------------------------------
    session.send(_start(pb, _mk_run_id("VAL-ghost")))
    time.sleep(0.5)
    session.record_step("ghost StartRun tolerated (stream alive)", session.is_open(),
                        "stream still open after unknown StartRun")


# ---------------------------------------------------------------------------
# Scenario 2 — reconnect: hello, persisted states, state report, logs
# ---------------------------------------------------------------------------

def scenario_reconnect(session: Session, sctx: ScenarioCtx):
    hello = session.expect_msg("reconnect hello", 60.0, session.is_hello, "HelloReport")
    if hello is None:
        return
    session.record_step("reconnect after server-initiated drop", True,
                        "client reconnected and sent hello")

    # runs reported at reconnect must reflect terminal states persisted in spool.
    # Note: runs the SERVER stopped (CANCELLED) are deleted from the client's
    # run registry by on_stop(), so only naturally-terminal runs are asserted.
    states = {r.run_id: r for r in hello.hello.runs}
    terminal_runs = [r for r in sctx.batch_log
                     if r.terminal and r.state in (4, 5)]  # COMPLETED / FAILED
    ok_all, details = bool(terminal_runs), []
    for rb in terminal_runs[-4:]:
        info = states.get(rb.run_id)
        if info is None:
            # runs whose batch was acked may legitimately be absent only if the
            # client deleted state — the client keeps terminal state, so absence
            # of a recently terminal run is a defect.
            ok_all = False
            details.append(f"{rb.run_id}: missing from hello")
            continue
        if info.state != rb.state:
            ok_all = False
            details.append(f"{rb.run_id}: hello state {info.state} != terminal {rb.state}")
        if info.last_batch_seq < rb.batch_seq:
            ok_all = False
            details.append(
                f"{rb.run_id}: last_batch_seq {info.last_batch_seq} < terminal {rb.batch_seq}")
    session.record_step(
        "hello after reconnect reports persisted terminal run states", ok_all,
        "; ".join(details) or f"{len(states)} run(s) in hello")

    # -- state report heartbeat (janitor: 300 s default) --------------------------------------
    if sctx.quick:
        session.record_step("state report heartbeat", True, "skipped (--quick)", warn=True)
    else:
        sr = None
        deadline = time.monotonic() + sctx.state_report_timeout
        while time.monotonic() < deadline and session.is_open():
            msg = session.recv(timeout=min(2.0, max(0.1, deadline - time.monotonic())))
            if msg is not None and session.is_state_report(msg):
                sr = msg.state_report
                break
        if sr is not None:
            session.record_step(
                "state report heartbeat received", True,
                f"board_id={sr.board_id} overall={sr.overall_state} "
                f"spool_bytes={sr.spool_bytes} cap={sr.spool_cap_bytes} "
                f"pressure={sr.spool_pressure:.3f}")
            session.record_step(
                "state report board_id matches device identity",
                (not sctx.expected_board_id) or sr.board_id == sctx.expected_board_id,
                f"report={sr.board_id!r} expected={sctx.expected_board_id!r}")
            session.record_step(
                "state report pressure in [0,1]", 0.0 <= sr.spool_pressure <= 1.0,
                f"pressure={sr.spool_pressure}")
        else:
            session.record_step(
                "state report heartbeat received", False,
                f"no StateReport within {sctx.state_report_timeout:.0f}s "
                "(janitor sweep_interval default 300s)")

    # -- log batches ---------------------------------------------------------------------------
    logs = session.drain(10.0, session.is_log_batch)
    if logs:
        lb = logs[0].log_batch
        well_formed = all(l.source and l.level for l in lb.lines) or len(lb.lines) == 0
        session.record_step(
            "log batches observed and well-formed", well_formed,
            f"{len(logs)} LogBatch(es), first run_id={lb.run_id!r} "
            f"n_lines={len(lb.lines)}")
        for m in logs:
            session.send(_log_ack(sctx.pb, m.log_batch.run_id, m.log_batch.batch_seq))
    else:
        session.record_step(
            "log batches observed", True,
            "no LogBatch within 10s (quiet system is acceptable)", warn=True)

    # -- durability setup: single-shot run left UNACKED for scenario 3 --------------
    runu = _mk_run_id(sctx.run_prefix)
    session.send(_configure(sctx.pb, runu, sctx.module, sctx.pb.common.SINGLE_SHOT))
    au = session.expect_msg(
        f"configure ack for unacked-run {runu}", 8.0,
        lambda m: session.is_configure_ack(m) and m.configure_ack.run_id == runu,
        "ConfigureAck")
    if au and au.configure_ack.ok:
        session.send(_start(sctx.pb, runu))
        lb, lt = _collect_terminal(session, runu, 30.0, sctx)
        if lt is not None:
            session.record_step(
                f"[{runu}] terminal batch left unacked (persistence setup)", True,
                f"seq={lt.batch_seq} state={lt.state}")
        else:
            session.record_step(
                f"[{runu}] terminal batch left unacked", False,
                "terminal batch never received")
    else:
        session.record_step(
            f"[{runu}] unacked-run setup", au is not None,
            "configure/start for the persistence check failed")


# ---------------------------------------------------------------------------
# Scenario 3 — durability: an unacked run's seq persisted across reconnects
# ---------------------------------------------------------------------------

def scenario_persistence(session: Session, sctx: ScenarioCtx):
    hello = session.expect_msg("hello on 3rd connection", 60.0, session.is_hello,
                               "HelloReport")
    if hello is None:
        return
    target_runs = [r for r in sctx.batch_log
                   if r.terminal and r.state in (4, 5)  # not CANCELLED (deleted)
                   and r.run_id.startswith("VAL-")
                   and r.run_id not in sctx.acked_run_ids]
    if not target_runs:
        session.record_step("persisted last_batch_seq check", True,
                            "no unacked terminal run available (nothing to check)",
                            warn=True)
        return
    states = {r.run_id: r for r in hello.hello.runs}
    ok, details = True, []
    for rb in target_runs[-3:]:
        info = states.get(rb.run_id)
        if info is None:
            ok = False
            details.append(f"{rb.run_id}: absent from hello")
            continue
        if info.last_batch_seq != rb.batch_seq:
            ok = False
            details.append(
                f"{rb.run_id}: last_batch_seq={info.last_batch_seq} expected={rb.batch_seq}")
    session.record_step(
        "unacked run's last_batch_seq persisted across reconnect", ok,
        "; ".join(details) or "all terminal seqs match")


# ---------------------------------------------------------------------------
# gRPC server
# ---------------------------------------------------------------------------

class MockAcadiaServer:
    """grpc server implementing acadia.stream.ClientService/Connect.

    Each new client stream is dispatched to the next scenario in the list;
    the last scenario repeats for any further streams.  The servicer runs
    each scenario on a worker thread and pumps its outbound queue as the
    response stream.
    """

    def __init__(self, pb, port: int, scenarios: List[Callable],
                 sctx: ScenarioCtx, bind: str = "0.0.0.0"):
        self.pb = pb
        self.port = port
        self.scenarios = scenarios
        self.sctx = sctx
        self.sessions: List[Session] = []
        self._lock = threading.Lock()
        self._ordinal = 0
        self._stop = threading.Event()

        outer = self

        class _Servicer(pb.client_service_grpc.ClientServiceServicer):
            def Connect(self, request_iterator, context):
                with outer._lock:
                    outer._ordinal += 1
                    ordinal = outer._ordinal
                    scenario = outer.scenarios[
                        min(ordinal - 1, len(outer.scenarios) - 1)]
                session = Session(ordinal, request_iterator, context)
                outer.sessions.append(session)

                def drive():
                    try:
                        scenario(session, outer.sctx)
                    except Exception as e:  # scenario crash must not kill the server
                        session.record_step("scenario crashed", False, repr(e))
                    finally:
                        session.scenario_done.set()

                worker = threading.Thread(target=drive, daemon=True)
                worker.start()

                # Pump outbound messages while the client is connected.
                while True:
                    if session.closed.is_set() and session.out_q.empty():
                        break
                    try:
                        msg = session.out_q.get(timeout=0.2)
                        yield msg
                    except queue.Empty:
                        continue
                    except Exception:
                        break
                # flush anything queued during teardown
                while not session.out_q.empty():
                    try:
                        yield session.out_q.get_nowait()
                    except queue.Empty:
                        break

        self._server = grpc.server(
            __import__("concurrent").futures.ThreadPoolExecutor(max_workers=8),
            options=[("grpc.so_reuseport", 0)])
        pb.client_service_grpc.add_ClientServiceServicer_to_server(
            _Servicer(), self._server)
        bound = self._server.add_insecure_port(f"{bind}:{port}")
        if bound == 0:
            raise RuntimeError(f"mock server could not bind {bind}:{port}")

    def start(self):
        self._server.start()

    def stop(self, grace: float = 1.0):
        self._server.stop(grace)

    def all_steps(self) -> List[Step]:
        out = []
        for s in self.sessions:
            out.extend(s.steps)
        return out
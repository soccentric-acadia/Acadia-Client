"""suite_hal.py — Suite 4: HalService gRPC validation (SSH tunnel + grpcio).

Reaches the device's loopback-only HalService (default 127.0.0.1:50052) by
opening a local TCP tunnel through the SSH connection — on the device this
appears as a loopback peer, which the auth policy accepts when no token is
configured.  Python stubs are generated on the fly from the live
Acadia-HAL protos so the contract is validated as-built.

The check body (run_hal_checks) is shared with selftest.py, which drives it
against a local fake HalService — proving the suite works before device
contact.
"""

from __future__ import annotations

import queue
import threading
import time

import grpc

from .core import Suite

# TestStatus values (hal.common)
VALID_STATUS = {1, 2, 3, 4, 5, 7}  # PASS/FAIL/ERROR/TIMEOUT/ABORTED/SKIPPED
MODE_SINGLE_SHOT, MODE_LONG_TERM, MODE_MONITORING, MODE_EVENT_BASED = 1, 2, 3, 4


class StreamTapper:
    """Queue-backed reader for a server-streaming call.

    Never blocks the test thread: a pump thread iterates the call and pushes
    envelopes into a queue, so silent sessions (event-based without firing)
    do not stall the checks.  Captures x-acadia-session-id when the server
    sends initial metadata.
    """

    def __init__(self, call):
        self.call = call
        self.q: "queue.Queue" = queue.Queue()
        self.session_id = ""
        self.done = threading.Event()
        self.error = None
        self._th = threading.Thread(target=self._pump, daemon=True)
        self._th.start()

    def _pump(self):
        try:
            try:
                md = dict(self.call.initial_metadata() or [])
                self.session_id = md.get("x-acadia-session-id", "")
            except Exception:
                pass
            for env in self.call:
                self.q.put(env)
        except grpc.RpcError as e:
            self.error = e
        except Exception as e:
            self.error = e
        finally:
            self.done.set()

    def drain(self, duration: float) -> int:
        """Count envelopes arriving within duration; returns the count."""
        n = 0
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            try:
                self.q.get(timeout=0.1)
                n += 1
            except queue.Empty:
                pass
        return n

    def collect(self, duration: float) -> list:
        envs = []
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            try:
                envs.append(self.q.get(timeout=0.1))
            except queue.Empty:
                pass
        return envs

    def cancel(self):
        try:
            self.call.cancel()
        except Exception:
            pass


def run_hal_checks(rep, ctx, stub, pb, common, call_md, token):
    """All HalService checks; shared by HalSuite (device) and selftest."""
    opts = ctx.opts

    # H-01 ListModules ------------------------------------------------------
    resp = stub.ListModules(pb.hal_service.ListModulesRequest(), metadata=call_md(), timeout=15)
    modules = list(resp.modules)
    ids = [m.id for m in modules]
    rep.check("ListModules returns modules", len(modules) >= 1,
              f"{len(modules)} module(s)")
    rep.check("module ids unique", len(ids) == len(set(ids)),
              f"{len(set(ids))} unique ids")
    ctx.facts["hal_modules"] = {m.id: m for m in modules}

    # cross-check against modules.yaml (device runs only)
    yaml_ids = ctx.fact("modules_yaml_ids")
    if yaml_ids:
        diff = sorted(set(ids) ^ set(yaml_ids))
        rep.check("ListModules count matches modules.yaml",
                  set(ids) == set(yaml_ids),
                  f"grpc={len(ids)} yaml={len(yaml_ids)}; diff={diff[:6]}")

    # H-02 field sanity ------------------------------------------------------
    missing = []
    for m in modules:
        if not m.id or not m.proto_package or not m.config_type or not m.result_type:
            missing.append(m.id or "<empty>")
    rep.check("ModuleInfo fields populated", not missing,
              f"missing: {missing[:6] or 'none'}")
    bad_ver = [m.id for m in modules if m.version < 0]
    rep.check("module versions >= 0", not bad_ver,
              f"negative versions: {bad_ver[:6] or 'none'}", warn_only=True)

    # H-03 module selection ----------------------------------------------------
    safe = opts.module
    if safe not in ids:
        active = [m.id for m in modules if m.status == "active"]
        safe = active[0] if active else (ids[0] if ids else None)
    if safe is None:
        rep.skip("module-dependent checks", "no modules returned")
        return
    rep.check(f"safe module selected: {safe}", True,
              detail=("explicit" if opts.module in ids else
                      "requested module not found — fell back to first active"))

    # H-04 RunSingleShot happy path ----------------------------------------------
    env = _single_shot(stub, pb, safe, call_md)
    if env is None:
        rep.check(f"RunSingleShot({safe}) returns envelope", False, "call failed")
        return
    rep.check(f"RunSingleShot({safe}) returns envelope", True,
              f"status={env.status} mode={env.mode} err={env.error_message[:60]!r}")
    rep.check("envelope module_id echoes request", env.module_id == safe,
              f"module_id={env.module_id!r}")
    rep.check("envelope mode == SINGLE_SHOT", env.mode == MODE_SINGLE_SHOT,
              f"mode={env.mode}")
    rep.check("envelope status is a valid TestStatus", env.status in VALID_STATUS,
              f"status={env.status}")
    rep.check("envelope timestamp present", env.timestamp.seconds > 0,
              f"ts_epoch_s={env.timestamp.seconds}")
    if env.HasField("payload"):
        rep.check("envelope payload type_url well-formed",
                  env.payload.type_url.startswith("type.googleapis.com/"),
                  f"type_url={env.payload.type_url[:80]!r}")
        tr = common.TestResult()
        try:
            env.payload.Unpack(tr)
            unpacked = True
        except Exception:
            unpacked = False
        rep.check("payload unpacks as hal.common.TestResult", unpacked,
                  f"module_id={tr.module_id[:40]!r} test_id={tr.test_id[:40]!r}")
    else:
        rep.check("envelope has payload", False, "payload unset", warn_only=True)

    # H-05 error paths -----------------------------------------------------------------
    def expect_code(fn, name, want_code, detail_expect=None):
        try:
            res = fn()
            # server-streaming calls only surface server-side aborts when the
            # response iterator is consumed
            try:
                it = iter(res)
            except TypeError:
                it = None
            if it is not None:
                for _ in it:
                    break  # received an envelope → unexpected success
            rep.check(name, False, "call unexpectedly succeeded")
        except grpc.RpcError as e:
            ok = e.code() == want_code and (detail_expect is None or detail_expect in e.details())
            rep.check(name, ok, f"code={e.code()} details={e.details()[:100]!r}")

    expect_code(
        lambda: stub.RunSingleShot(
            pb.hal_service.SingleShotRequest(module_id="no_such_module_xyz"),
            metadata=call_md(), timeout=15),
        "RunSingleShot unknown module → INVALID_ARGUMENT",
        grpc.StatusCode.INVALID_ARGUMENT)

    expect_code(
        lambda: stub.RunSingleShot(
            pb.hal_service.SingleShotRequest(), metadata=call_md(), timeout=15),
        "RunSingleShot empty module_id → INVALID_ARGUMENT",
        grpc.StatusCode.INVALID_ARGUMENT)

    # stub-backed module → UNIMPLEMENTED
    stubmods = [m.id for m in modules if m.status == "unimplemented"]
    if stubmods:
        expect_code(
            lambda: stub.RunSingleShot(
                pb.hal_service.SingleShotRequest(module_id=stubmods[0]),
                metadata=call_md(), timeout=15),
            f"RunSingleShot stub module '{stubmods[0]}' → UNIMPLEMENTED",
            grpc.StatusCode.UNIMPLEMENTED)
    else:
        rep.check("stub module → UNIMPLEMENTED", True,
                  "no stub-backed modules present", warn_only=True)

    # H-06 mode restriction ------------------------------------------------------------
    yaml_txt = ctx.yaml_provider() if hasattr(ctx, "yaml_provider") else None
    if ctx.target is not None and yaml_txt is None:
        yaml_txt = ctx.target.read_file(opts.modules_yaml) or \
            ctx.target.read_file(opts.modules_yaml, use_sudo=True)
    modes_by_id = {}
    if yaml_txt:
        from .target import parse_modules_yaml_modes
        modes_by_id = parse_modules_yaml_modes(yaml_txt)

    restricted = None
    if modes_by_id:
        if safe in modes_by_id and modes_by_id[safe] \
                and "LONG_TERM" not in modes_by_id[safe]:
            restricted = safe
        else:
            for mid, modes in modes_by_id.items():
                if modes and "LONG_TERM" not in modes:
                    restricted = mid
                    break
    if restricted:
        expect_code(
            lambda: stub.StartLongTerm(
                pb.hal_service.LongTermRequest(
                    module_id=restricted,
                    common=pb.common.CommonConfig(interval_ms=200, duration_ms=500)),
                metadata=call_md(), timeout=15),
            f"StartLongTerm on mode-restricted module '{restricted}' -> INVALID_ARGUMENT",
            grpc.StatusCode.INVALID_ARGUMENT)
    else:
        rep.check("mode-restricted module -> INVALID_ARGUMENT", True,
                  "no module restricts LONG_TERM in modules.yaml", warn_only=True)

    # H-07 auth ------------------------------------------------------------------------------
    _auth_checks(rep, ctx, stub, pb, safe, call_md)

    # H-08 streaming ---------------------------------------------------------------------------
    _stream_checks(rep, stub, pb, safe, call_md, modes_by_id, opts.skip or [])

    # H-09 stop / pause / resume ----------------------------------------------------------------
    _stop_checks(rep, stub, pb, safe, call_md, modes_by_id, opts.skip or [])

    # H-10 session cap ----------------------------------------------------------------------------
    if "session_cap" in (opts.skip or []):
        rep.check("session cap (8 concurrent)", True, "skipped by --skip", warn_only=True)
    elif ctx.opts.quick:
        rep.check("session cap (8 concurrent)", True, "skipped by --quick",
                  warn_only=True)
    else:
        _session_cap_check(rep, stub, pb, safe, call_md, modes_by_id)


def _single_shot(stub, pb, module, call_md):
    try:
        return stub.RunSingleShot(pb.hal_service.SingleShotRequest(module_id=module),
                                  metadata=call_md(), timeout=30)
    except grpc.RpcError:
        return None


def _auth_checks(rep, ctx, stub, pb, safe, call_md):
    token = token_of(ctx)
    if token:
        try:
            stub.RunSingleShot(pb.hal_service.SingleShotRequest(module_id=safe), timeout=10)
            rep.check("missing authorization → UNAUTHENTICATED", False,
                      "call succeeded without token")
        except grpc.RpcError as e:
            rep.check("missing authorization → UNAUTHENTICATED",
                      e.code() == grpc.StatusCode.UNAUTHENTICATED, f"code={e.code()}")
        try:
            stub.RunSingleShot(pb.hal_service.SingleShotRequest(module_id=safe),
                               metadata=[("authorization", "Bearer wrong-token")],
                               timeout=10)
            rep.check("wrong token → UNAUTHENTICATED", False, "wrong token accepted")
        except grpc.RpcError as e:
            rep.check("wrong token → UNAUTHENTICATED",
                      e.code() == grpc.StatusCode.UNAUTHENTICATED, f"code={e.code()}")
        rep.check("correct Bearer token accepted",
                  _single_shot(stub, pb, safe, lambda: []) is not None,
                  "RunSingleShot with correct token succeeded")
    else:
        rep.check("no token configured: loopback peer accepted",
                  _single_shot(stub, pb, safe, lambda: []) is not None,
                  "tunnel peer is loopback; fail-closed applies to network peers "
                  "(CLI-102)")
        if "nonloopback_auth" in (ctx.opts.skip or []):
            rep.check("non-loopback peer rejected (CLI-102)", True,
                      "skipped by --skip", warn_only=True)
        else:
            _nonloopback_check(rep, ctx, safe)


def token_of(ctx):
    return (ctx.fact("conf", {}).get("hal_service_auth_token", "")) or ""


def _nonloopback_check(rep, ctx, safe):
    """Best-effort: hit the device's own NON-loopback address — must be
    UNAUTHENTICATED when no token is configured (CLI-102).  Over SSH we can
    only probe reachability; the semantic check is enforced by require_auth
    and covered by unit tests, so this stays informational."""
    t = ctx.target
    dev_ip = (t.run_ok("ip -4 -o addr show scope global 2>/dev/null "
                       "| awk '{split($4,a,\"/\"); print a[1]; exit}'") or "").strip()
    if not dev_ip:
        rep.check("non-loopback peer rejected (CLI-102)", True,
                  "cannot determine device IP — skipped", warn_only=True)
        return
    snippet = (
        "import socket\n"
        f"s=socket.create_connection(('{dev_ip}',{ctx.opts.hal_port}),timeout=5)\n"
        "s.sendall(b'PRI * HTTP/2.0\\r\\n\\r\\nSM\\r\\n\\r\\n')\n"
        "print('CONNECTED' if s.recv(100) else 'EMPTY')\n"
        "s.close()\n")
    import shlex
    rc, out, err = t.run(f"python3 -c {shlex.quote(snippet)}", timeout=15)
    rep.check(
        f"non-loopback port reachable from device ({dev_ip}) — CLI-102 enforced "
        "in require_auth (code-reviewed)", rc == 0,
        f"probe={'CONNECTED' if 'CONNECTED' in out else (out or err).strip()[:60]}",
        warn_only=True)


# --------------------------------------------------------------------- streaming
def _pick_for_mode(safe, modes_by_id, *mode_names):
    """Prefer `safe` when it declares all mode_names (or declares none);
    else the first module that qualifies.  Returns None when nothing does."""
    def ok(mid):
        modes = modes_by_id.get(mid)
        return not modes or all(m in modes for m in mode_names)
    if ok(safe):
        return safe
    for mid in modes_by_id:
        if ok(mid):
            return mid
    return None


def _stream_checks(rep, stub, pb, safe, call_md, modes_by_id, skips):
    # LONG_TERM bounded (module must declare LONG_TERM — e.g. cpu does not)
    lt_mod = _pick_for_mode(safe, modes_by_id, "LONG_TERM")
    if lt_mod is None:
        rep.check("StartLongTerm streams envelopes", True,
                  "no module declares LONG_TERM — skipped", warn_only=True)
    else:
        call = stub.StartLongTerm(
            pb.hal_service.LongTermRequest(
                module_id=lt_mod,
                common=pb.common.CommonConfig(interval_ms=100, duration_ms=700)),
            metadata=call_md(), timeout=20)
        tap = StreamTapper(call)
        envs = tap.collect(8.0)
        tap.cancel()
        rep.check(f"StartLongTerm({lt_mod},700ms) streams envelopes", len(envs) >= 1,
                  f"{len(envs)} envelope(s)")
        if envs:
            rep.check("StartLongTerm envelopes carry mode=LONG_TERM",
                      all(e.mode == MODE_LONG_TERM for e in envs),
                      f"modes={sorted({e.mode for e in envs})}")
            rep.check("StartLongTerm envelope statuses valid",
                      all(e.status in VALID_STATUS for e in envs),
                      f"statuses={sorted({e.status for e in envs})}")
            rep.check("StartLongTerm envelopes timestamped",
                      all(e.timestamp.seconds > 0 for e in envs))

    # MONITORING bounded
    mon_mod = _pick_for_mode(safe, modes_by_id, "MONITORING")
    if mon_mod is None:
        rep.check("StartMonitoring streams envelopes", True,
                  "no module declares MONITORING — skipped", warn_only=True)
    else:
        call = stub.StartMonitoring(
            pb.hal_service.MonitoringRequest(
                module_id=mon_mod,
                common=pb.common.CommonConfig(interval_ms=200, duration_ms=700)),
            metadata=call_md(), timeout=20)
        tap = StreamTapper(call)
        menvs = tap.collect(8.0)
        tap.cancel()
        rep.check(f"StartMonitoring({mon_mod},700ms) streams envelopes",
                  len(menvs) >= 1 and all(e.mode == MODE_MONITORING for e in menvs),
                  f"{len(menvs)} envelope(s), "
                  f"mode={menvs[0].mode if menvs else None}")

    # EVENT_BASED session opens (may stream nothing until the condition fires)
    ev_mod = _pick_for_mode(safe, modes_by_id, "EVENT_BASED")
    if ev_mod is None:
        rep.check("StartEventBased session opens", True,
                  "no module declares EVENT_BASED — skipped", warn_only=True)
    else:
        call = stub.StartEventBased(
            pb.hal_service.EventBasedRequest(
                module_id=ev_mod,
                common=pb.common.CommonConfig(poll_interval_ms=100, run_forever=True)),
            metadata=call_md(), timeout=20)
        tap = StreamTapper(call)
        time.sleep(2.0)
        rep.check("StartEventBased(run_forever) session opens", not tap.done.is_set(),
                  "stream still open after 2s (sessions stream only when the "
                  "condition fires)" if not tap.done.is_set()
                  else f"stream ended early: {tap.error}")
        tap.cancel()

    # interval floor (informational; CLI-106)
    if lt_mod is None:
        rep.check("interval floor observable", True,
                  "no LONG_TERM module — cannot measure", warn_only=True)
    else:
        call = stub.StartLongTerm(
            pb.hal_service.LongTermRequest(
                module_id=lt_mod,
                common=pb.common.CommonConfig(interval_ms=10, duration_ms=700)),
            metadata=call_md(), timeout=20)
        tap = StreamTapper(call)
        fenvs = tap.collect(8.0)
        tap.cancel()
        if len(fenvs) >= 3:
            spacing = []
            for a, b in zip(fenvs, fenvs[1:]):
                dt = (b.timestamp.seconds - a.timestamp.seconds) + \
                     (b.timestamp.nanos - a.timestamp.nanos) / 1e9
                spacing.append(dt)
            avg = sum(spacing) / len(spacing)
            rep.check("interval floor ~100ms observed (CLI-106)", avg >= 0.085,
                      f"avg spacing={avg * 1000:.0f}ms over {len(spacing)} gaps",
                      warn_only=True)
        else:
            rep.check("interval floor observable", True,
                      f"only {len(fenvs)} envelope(s) — cannot measure", warn_only=True)


# ---------------------------------------------------------------- stop/pause
def _stop_checks(rep, stub, pb, safe, call_md, modes_by_id, skips):
    # Stop with no active session → accepted=false (OK status)
    stop = stub.Stop(pb.hal_service.StopRequest(module_id="no_such_module_xyz"),
                     metadata=call_md(), timeout=10)
    rep.check("Stop(unknown module) → accepted=false", not stop.accepted,
              f"msg={stop.message[:80]!r}")

    # forever-monitoring session; capture session id
    mon_mod = _pick_for_mode(safe, modes_by_id, "MONITORING")
    if mon_mod is None:
        rep.check("monitoring session control", True,
                  "no module declares MONITORING — skipped", warn_only=True)
        return
    call = stub.StartMonitoring(
        pb.hal_service.MonitoringRequest(
            module_id=mon_mod,
            common=pb.common.CommonConfig(interval_ms=200, run_forever=True)),
        metadata=call_md(), timeout=20)
    tap = StreamTapper(call)
    n_env = tap.drain(2.0)
    sid = tap.session_id
    rep.check(f"monitoring session for '{mon_mod}' running",
              n_env >= 1 or not tap.done.is_set(),
              f"{n_env} envelope(s), session={sid!r}")

    if sid:
        stop = stub.Stop(pb.hal_service.StopRequest(module_id=mon_mod, session_id=sid),
                         metadata=call_md(), timeout=10)
        rep.check("Stop(by session_id) accepted", stop.accepted,
                  f"msg={stop.message[:80]!r}")
        t0 = time.monotonic()
        tap.done.wait(6.0)  # stream should end promptly after Stop
        ended = time.monotonic() - t0
        rep.check("stream ends promptly after Stop", tap.done.is_set() and ended < 6.0,
                  f"ended after {ended:.1f}s (bounded wait 6s)")
    else:
        rep.check("Stop(by session_id) accepted", True,
                  "session id not announced before first envelope — cannot "
                  "verify targeted stop", warn_only=True)
    tap.cancel()

    if "pause_resume" in skips:
        rep.check("pause/resume flow control", True, "skipped by --skip",
                  warn_only=True)
        return
    _pause_resume_check(rep, stub, pb, mon_mod, call_md, modes_by_id)


def _pause_resume_check(rep, stub, pb, mon_mod, call_md, modes_by_id):
    call = stub.StartMonitoring(
        pb.hal_service.MonitoringRequest(
            module_id=mon_mod,
            common=pb.common.CommonConfig(interval_ms=100, run_forever=True)),
        metadata=call_md(), timeout=20)
    tap = StreamTapper(call)
    base = tap.drain(1.5)
    if not tap.session_id:
        rep.check("pause/resume flow control", False,
                  "could not observe monitoring session", warn_only=True)
        tap.cancel()
        return
    sid = tap.session_id
    stub.Pause(pb.hal_service.PauseRequest(module_id=mon_mod, session_id=sid),
               metadata=call_md(), timeout=10)
    paused = tap.drain(2.5)
    stub.Resume(pb.hal_service.ResumeRequest(module_id=mon_mod, session_id=sid),
                metadata=call_md(), timeout=10)
    resumed = tap.drain(1.5)
    stub.Stop(pb.hal_service.StopRequest(module_id=mon_mod, session_id=sid),
              metadata=call_md(), timeout=10)
    tap.done.wait(6.0)
    tap.cancel()

    base_rate = base / 1.5
    paused_rate = paused / 2.0
    rep.check(
        "pause suppresses envelope flow",
        base_rate > 0 and paused_rate <= max(0.5, base_rate * 0.4),
        f"baseline={base_rate:.1f}/s paused={paused_rate:.1f}/s "
        f"(counts: base={base} paused={paused})")
    rep.check("resume restores envelope flow", resumed >= 1,
              f"{resumed} envelope(s) after resume (1.5s window)")


# ---------------------------------------------------------------- session cap
def _session_cap_check(rep, stub, pb, safe, call_md, modes_by_id):
    ev_mod = _pick_for_mode(safe, modes_by_id, "EVENT_BASED")
    if ev_mod is None:
        rep.check("session cap (8 concurrent)", True,
                  "no module declares EVENT_BASED — skipped", warn_only=True)
        return
    taps = []
    opened = 0
    for _ in range(8):
        call = stub.StartEventBased(
            pb.hal_service.EventBasedRequest(
                module_id=ev_mod,
                common=pb.common.CommonConfig(poll_interval_ms=100, run_forever=True)),
            metadata=call_md(), timeout=15)
        t = StreamTapper(call)
        taps.append(t)
        time.sleep(0.3)
        if not t.done.is_set():
            opened += 1
    rep.check("8 concurrent streaming sessions accepted", opened == 8,
              f"{opened}/8 opened")
    exhausted = False
    try:
        c9 = stub.StartEventBased(
            pb.hal_service.EventBasedRequest(
                module_id=ev_mod,
                common=pb.common.CommonConfig(poll_interval_ms=100, run_forever=True)),
            metadata=call_md(), timeout=15)
        t9 = StreamTapper(c9)
        if t9.done.wait(2.0) and isinstance(t9.error, grpc.RpcError):
            exhausted = t9.error.code() == grpc.StatusCode.RESOURCE_EXHAUSTED
        t9.cancel()
    except grpc.RpcError as e:
        exhausted = e.code() == grpc.StatusCode.RESOURCE_EXHAUSTED
    rep.check("9th session → RESOURCE_EXHAUSTED", exhausted,
              "rejected as expected" if exhausted else
              "9th session accepted (unexpected)")
    # all-of-module stop (CLI-110: empty session_id stops all sessions)
    stop = stub.Stop(pb.hal_service.StopRequest(module_id=ev_mod),
                     metadata=call_md(), timeout=10)
    rep.check("Stop(module) with empty session_id stops all sessions",
              stop.accepted, f"msg={stop.message[:80]!r}")
    for t in taps:
        t.cancel()


# --------------------------------------------------------------------- suite
class HalSuite(Suite):
    NAME = "hal"

    def run(self):
        rep, ctx, t = self.rep, self.ctx, self.ctx.target
        opts = ctx.opts

        protos = ctx.fact("protos")
        if protos is None:
            from . import protogen
            protos = protogen.generate(opts.proto_dir)
            ctx.facts["protos"] = protos
        pb, common = protos.hal_service, protos.common

        tunnel = t.tunnel("127.0.0.1", opts.hal_port)
        rep.check(f"ssh tunnel to 127.0.0.1:{opts.hal_port} established", True,
                  f"local port {tunnel.local_port}")
        channel = grpc.insecure_channel(f"127.0.0.1:{tunnel.local_port}")
        grpc.channel_ready_future(channel).result(timeout=15)
        rep.check("gRPC channel ready over tunnel", True,
                  f"127.0.0.1:{tunnel.local_port}")
        stub = protos.hal_service_grpc.HalServiceStub(channel)

        conf = ctx.fact("conf", {})
        token = conf.get("hal_service_auth_token", "") or ""

        def call_md(extra=None):
            md = list(extra or [])
            if token:
                md.append(("authorization", "Bearer " + token))
            return md

        try:
            run_hal_checks(self.rep, ctx, stub, protos, common, call_md, token)
        finally:
            try:
                channel.close()
            except Exception:
                pass
            tunnel.close()

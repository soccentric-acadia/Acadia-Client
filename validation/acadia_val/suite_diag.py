"""suite_diag.py — Suite 3: diagnostic unix-socket checks (SSH).

The client exposes /run/acadia/client.sock; local queries receive one JSON
line: {"connected":…,"spool_bytes":…,"active_runs":…,"client_version":"…"}.
SSH cannot forward unix sockets, so probes are executed on-device via
socat / nc -U / a python3 helper (see Target.unix_probe).
"""

from __future__ import annotations

import json
import threading

from .core import Status, Suite


class DiagSuite(Suite):
    NAME = "diag"

    def run(self):
        rep, ctx, t = self.rep, self.ctx, self.ctx.target
        path = ctx.opts.diag_socket

        rep.check("diag socket file exists", t.is_socket(path), f"path={path}")

        resp = t.unix_probe(path, b"status\n")
        rep.check("probe receives a response", resp is not None,
                  detail=(resp or "no response (tried socat, nc, python3)"))

        data = None
        if resp:
            try:
                data = json.loads(resp.strip())
            except json.JSONDecodeError as e:
                rep.check("response is valid JSON", False, f"{e}: {resp!r:.120}")
        if data is not None:
            rep.check("response is valid JSON", True)
            for key, typ in (("connected", bool), ("spool_bytes", int),
                             ("active_runs", int), ("client_version", str)):
                ok = key in data and (isinstance(data[key], typ)
                                      or (typ is int and isinstance(data[key], float)))
                rep.check(f"response field '{key}' ({typ.__name__})", ok,
                          detail=f"{key}={data.get(key)!r}")

            ver = ctx.fact("version", "")
            if ver:
                rep.check("client_version matches binary --version",
                          data.get("client_version") == ver,
                          f"diag={data.get('client_version')!r} binary={ver!r}")

            rep.check("spool_bytes is a non-negative number",
                      isinstance(data.get("spool_bytes"), (int, float))
                      and data["spool_bytes"] >= 0,
                      f"spool_bytes={data.get('spool_bytes')}")
            rep.check("active_runs non-negative",
                      isinstance(data.get("active_runs"), int) and data["active_runs"] >= 0,
                      f"active_runs={data.get('active_runs')}")

        rep.check("response newline-terminated",
                  bool(resp) and resp.endswith("\n"),
                  detail=f"ends-with-newline={bool(resp) and resp.endswith(chr(10))}")

        # empty query must behave like "status"
        resp2 = t.unix_probe(path, b"\n")
        ok2 = False
        if resp2:
            try:
                json.loads(resp2.strip())
                ok2 = True
            except json.JSONDecodeError:
                ok2 = False
        rep.check("empty query treated as status", ok2,
                  detail=(resp2 or "no response").strip()[:120])

        # concurrent probes
        results = [None] * 5

        def worker(i):
            results[i] = t.unix_probe(path, b"status\n")

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(5)]
        for th in threads:
            th.start()
        for th in threads:
            th.join()
        got = sum(1 for r in results if r)
        rep.check("5 concurrent probes all served", got == 5,
                  f"{got}/5 responses")

        # connected flag — meaningful when a server address is configured
        if data is not None:
            conf = ctx.fact("conf", {})
            has_server = bool(conf.get("server_address"))
            if has_server:
                rep.check("'connected' is true (server reachable)",
                          data.get("connected") is True,
                          f"connected={data.get('connected')} "
                          f"(server_address={conf.get('server_address')})",
                          warn_only=data.get("connected") is not True)
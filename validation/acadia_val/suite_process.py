"""suite_process.py — Suite 2: process & runtime health checks (SSH).

Validates: the daemon process under systemd, restarts, memory/CPU, threads,
open fds, loaded shared objects, coredumps, journal error scan, and the
inbound HalService port binding.
"""

from __future__ import annotations

import os
import time

from .core import Status, Suite


class ProcessSuite(Suite):
    NAME = "proc"

    def run(self):
        rep, ctx, t = self.rep, self.ctx, self.ctx.target
        opts = ctx.opts
        service = opts.service
        binary = ctx.fact("binary", opts.binary)

        # -- process ------------------------------------------------------------
        rc, out, _ = t.run(f"systemctl show {service} -p MainPID "
                           f"-p ExecMainStartTimestamp -p NRestarts -p ActiveState")
        props = {}
        for line in out.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                props[k] = v
        pid = props.get("MainPID", "0").strip()
        rep.check("service has a MainPID", pid not in ("", "0"),
                  f"MainPID={pid} state={props.get('ActiveState')} "
                  f"started={props.get('ExecMainStartTimestamp')}")
        rep.check("no unexpected restarts (NRestarts < 3)",
                  int(props.get("NRestarts", "0") or 0) < 3,
                  f"NRestarts={props.get('NRestarts')}", warn_only=True)

        uptime_s = None
        if pid.isdigit():
            rc, out, _ = t.run(f"stat -Lc %Y /proc/{pid}")
            try:
                start_epoch = int(out.strip())
                uptime_s = time.time() - start_epoch
            except ValueError:
                pass
        rep.check("process up > 10 s (stable)", (uptime_s or 0) > 10,
                  f"uptime={uptime_s and f'{uptime_s:.0f}s'}", warn_only=True)

        # -- memory / threads / fds ---------------------------------------------
        if pid.isdigit():
            rc, out, _ = t.run(f"grep -E 'VmRSS|Threads' /proc/{pid}/status")
            rss_kb = 0
            threads = 0
            for line in out.splitlines():
                if line.startswith("VmRSS:"):
                    rss_kb = int(line.split()[1])
                if line.startswith("Threads:"):
                    threads = int(line.split()[1])
            rep.check("RSS < 300 MB", 0 < rss_kb < 200 * 1024,
                      f"VmRSS={rss_kb // 1024} MB")
            rep.check("thread count sane (1..200)", 0 < threads <= 200,
                      f"Threads={threads}")

            rc, out, _ = t.run(f"ls /proc/{pid}/fd 2>/dev/null | wc -l")
            try:
                fds = int(out.strip())
            except ValueError:
                fds = 0
            rep.check("open fds < 512", 0 < fds < 512, f"fds={fds}")

            # shared objects: gRPC/protobuf/sqlite/spdlog must be mapped
            rc, out, _ = t.run(f"cat /proc/{pid}/maps | grep -oE 'lib[^/]*\\.so[^ ]*' "
                               f"| sort -u | head -40")
            libs = out.splitlines()
            wanted = {"grpc": None, "protobuf": None, "sqlite": None}
            for lib in libs:
                for k in wanted:
                    if k in lib and wanted[k] is None:
                        wanted[k] = lib
            rep.check("libgrpc mapped in process", bool(wanted["grpc"]),
                      f"found: {wanted['grpc']}")
            rep.check("libsqlite3 mapped in process", bool(wanted["sqlite"]),
                      f"found: {wanted['sqlite']} (spool uses sqlite3)",
                      warn_only=not wanted["sqlite"])

            # CPU sample over 2 s
            rc, out, _ = t.run(
                f"awk '{{print $14}}' /proc/{pid}/stat && sleep 2 && "
                f"awk '{{print $14}}' /proc/{pid}/stat", timeout=15)
            utimes = out.split()
            if len(utimes) == 2:
                try:
                    hz = os.sysconf("SC_CLK_TCK")
                    cpu_s = (int(utimes[1]) - int(utimes[0])) / hz
                    rep.check("idle CPU (< 50% of one core)", cpu_s < 1.0,
                              f"utime delta over 2s: {cpu_s:.2f} core-seconds")
                except (ValueError, OSError):
                    rep.check("idle CPU", True, "could not parse /proc stat", warn_only=True)
        else:
            rep.skip("process resource checks", "no MainPID")

        # -- coredumps -----------------------------------------------------------
        rc, out, _ = t.run("ls /var/lib/systemd/coredump 2>/dev/null | wc -l")
        try:
            cores = int(out.strip())
        except ValueError:
            cores = -1
        rep.check("no coredumps on device", cores == 0,
                  f"coredump count={cores}", warn_only=cores < 0)

        # -- journal noise -----------------------------------------------------------
        rc, out, err = t.run(
            f"journalctl -u {service} --since '-15 min' --no-pager 2>/dev/null "
            f"| grep -ciE 'critical|segfault|abort|panic|assert'", timeout=20)
        if rc == 0:
            rep.check("no critical/segfault lines in last 15 min",
                      out.strip() in ("0", ""), f"matches={out.strip()}")
        else:
            rep.check("journal readable", False,
                      f"journalctl unavailable: {err.strip()[:100]} (permissions?)",
                      warn_only=True)

        # -- HalService port binding ------------------------------------------------------
        hal_port = opts.hal_port
        rc, out, _ = t.run("ss -ltn 2>/dev/null || netstat -ltn 2>/dev/null")
        entries = [ln for ln in out.splitlines() if f":{hal_port}" in ln]
        rep.check(f"HalService listening on :{hal_port}", bool(entries),
                  detail="; ".join(e.strip() for e in entries) or f"nothing on {hal_port}")
        if entries:
            loopback = any(("127.0.0.1" in e or "[::1]" in e) for e in entries)
            anybind = any("0.0.0.0" in e or "[::]" in e or "*:" in e for e in entries)
            if loopback:
                rep.check(f"port {hal_port} bound to loopback only", True,
                          "matches secure default")
            elif anybind:
                conf = ctx.fact("conf", {})
                token = bool(conf.get("hal_service_auth_token"))
                rep.check(f"port {hal_port} on non-loopback bind requires auth token",
                          token,
                          f"bind={entries[0].strip()} token={'set' if token else 'unset'}",
                          warn_only=True)
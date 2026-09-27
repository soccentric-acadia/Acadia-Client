"""suite_stream.py — Suite 5: outbound stream protocol validation using a
mock Acadia-Server (ClientService/Connect bidi stream).

The device dials OUT to Acadia-Server:server_port.  To validate the stream
protocol end-to-end the suite runs a mock Acadia-Server on this x86 host and
requires the device's client.conf to point here (server_address == this
host as seen from the device, insecure = true for plaintext).

Precondition handling:
  * If the device conf already points at this host → mock starts immediately.
  * Otherwise the suite SKIPs with an actionable remediation message, unless
    --auto-provision-stream is given, in which case the conf is rewritten
    (backup kept), the service restarted, and everything restored on exit.
"""

from __future__ import annotations

import base64
import re
import shlex
import socket
import time

from .core import Status, Suite
from .mock_server import (MockAcadiaServer, ScenarioCtx,
                          scenario_first_connection, scenario_persistence,
                          scenario_reconnect)


def find_free_port(preferred: int, bind: str = "0.0.0.0") -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind((bind, preferred))
        return preferred
    except OSError:
        pass
    finally:
        s.close()
    s = socket.socket()
    s.bind((bind, 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_for(fn, timeout: float, interval: float = 0.5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        v = fn()
        if v:
            return v
        time.sleep(interval)
    return None


def _conf_points_at_host(conf, host_ip) -> bool:
    server_addr = str(conf.get("server_address", ""))
    return server_addr in (host_ip, "127.0.0.1", "localhost")


class StreamSuite(Suite):
    NAME = "stream"

    def run(self):
        rep, ctx, t = self.rep, self.ctx, self.ctx.target
        opts = ctx.opts

        protos = ctx.fact("protos")
        if protos is None:
            from . import protogen
            protos = protogen.generate(opts.proto_dir)
            ctx.facts["protos"] = protos
        pb = protos

        conf = ctx.fact("conf", {})
        server_addr = str(conf.get("server_address", ""))
        server_port = int(conf.get("server_port", opts.stream_port) or 50051)
        insecure = bool(conf.get("insecure"))

        # ---- host IP as seen from the device --------------------------------
        ssh_out = t.run_ok("echo ${SSH_CLIENT%% *} ${SSH_CONNECTION%% *}") or ""
        host_ip = ssh_out.strip().split()[0] if ssh_out.strip() else ""
        rep.check("host IP visible from device", bool(host_ip),
                  f"device sees this host as {host_ip!r}")

        # ---- precondition ------------------------------------------------------
        self._provisioned = False
        if _conf_points_at_host(conf, host_ip) and insecure:
            rep.check("device conf points at this host (precondition)", True,
                      f"server_address={server_addr!r} port={server_port} "
                      f"insecure={insecure}")
        elif opts.auto_provision_stream:
            self._provision_conf(host_ip, server_port)
            self._provisioned = True
        else:
            rep.skip(
                "stream protocol suite",
                f"device conf server_address={server_addr!r} does not point at this "
                f"host ({host_ip!r}) with insecure=true. Remediation: set "
                "server_address = <this-host-ip> and insecure = true in "
                f"{opts.config_path}, then restart {opts.service}; or re-run with "
                "--auto-provision-stream")
            return

        # ---- module selection ----------------------------------------------------
        module = self._pick_module()

        sctx = ScenarioCtx(pb, module=module)
        sctx.expected_board_id = ctx.fact("client_id") or None
        sctx.expected_client_version = ctx.fact("version") or None
        sctx.expected_token = conf.get("server_stream_token") or None
        sctx.quick = opts.quick or "state_report" in (opts.skip or [])
        sctx.state_report_timeout = opts.state_report_timeout

        # ---- bind the mock server on the configured port ------------------------
        port = find_free_port(server_port)
        if port != server_port and not self._provisioned:
            rep.check(f"host port {server_port} free for mock server", False,
                      f"port busy on this host; use --auto-provision-stream to also "
                      "rewrite server_port")
            return

        mock = None
        try:
            mock = MockAcadiaServer(pb, port,
                                    [scenario_first_connection,
                                     scenario_reconnect,
                                     scenario_persistence],
                                    sctx, bind="0.0.0.0")
            mock.start()
            rep.check(f"mock Acadia-Server listening on 0.0.0.0:{port}", True)
            if self._provisioned and port != server_port:
                self._rewrite_conf_value("server_port", str(port))
                self._restart_and_wait()

            self._drive_connections(mock)
        finally:
            if mock is not None:
                mock.stop(grace=1.0)
            if self._provisioned:
                try:
                    self._restore_conf()
                except Exception as e:
                    rep.record("restore original conf", Status.WARN, f"{e}")

        # -- convert recorded steps into results -----------------------------------
        if mock is not None:
            for step in mock.all_steps():
                status = Status.WARN if step.warn else (
                    Status.PASS if step.ok else Status.FAIL)
                self.rep.record(f"stream: {step.name}", status, step.detail)

    # ------------------------------------------------------------------ helpers
    def _pick_module(self):
        """Scenarios need SINGLE_SHOT and LONG_TERM; prefer --module when it
        declares them, else the first capable module from ListModules/modules.yaml."""
        opts = self.ctx.opts
        yaml_txt = self.ctx.target.read_file(opts.modules_yaml) or \
            self.ctx.target.read_file(opts.modules_yaml, use_sudo=True)
        from .target import parse_modules_yaml_modes
        modes_by_id = parse_modules_yaml_modes(yaml_txt) if yaml_txt else {}
        needed = {"SINGLE_SHOT", "LONG_TERM"}

        def ok(mid, mod_obj=None):
            if mod_obj is not None and mod_obj.status == "unimplemented":
                return False
            modes = modes_by_id.get(mid)
            return not modes or needed <= set(modes)

        mods = self.ctx.fact("hal_modules", {})
        if opts.module and ok(opts.module, mods.get(opts.module)):
            return opts.module
        for mid, m in mods.items():
            if ok(mid, m):
                return mid
        for mid in modes_by_id:
            if ok(mid):
                return mid
        yaml_ids = self.ctx.fact("modules_yaml_ids") or []
        return opts.module or (yaml_ids[0] if yaml_ids else "cpu")

    def _drive_connections(self, mock: MockAcadiaServer):
        t0 = time.monotonic()

        # connection 1
        s1 = wait_for(lambda: mock.sessions[0] if mock.sessions else None, 60)
        self.rep.check("device connects to mock server (connection 1)", s1 is not None,
                       f"after {time.monotonic() - t0:.0f}s")
        if s1 is None:
            return
        wait_for(lambda: s1.scenario_done.is_set(), 300, 1.0)
        self.rep.check("connection-1 scenario completed", s1.scenario_done.is_set(),
                       f"took {time.monotonic() - t0:.0f}s; steps={len(s1.steps)}")

        # server-initiated drop → client must reconnect
        try:
            s1.context.cancel()
        except Exception:
            pass

        s2 = wait_for(lambda: next(iter(mock.sessions[1:]), None), 90, 1.0)
        self.rep.check("device reconnects after server drop (connection 2)",
                       s2 is not None, "expected within 90s backoff window")
        if s2 is None:
            return
        wait_for(lambda: s2.scenario_done.is_set(),
                 self.ctx.opts.state_report_timeout + 30, 1.0)
        self.rep.check("connection-2 scenario completed", s2.scenario_done.is_set(),
                       f"steps={len(s2.steps)}")
        try:
            s2.context.cancel()
        except Exception:
            pass

        s3 = wait_for(lambda: next(iter(mock.sessions[2:]), None), 90, 1.0)
        self.rep.check("device reconnects again (connection 3)", s3 is not None)
        if s3 is None:
            return
        wait_for(lambda: s3.scenario_done.is_set(), 120, 1.0)
        self.rep.check("connection-3 scenario completed", s3.scenario_done.is_set(),
                       f"steps={len(s3.steps)}")

    # ---------------------------------------------------------------- provisioning
    def _provision_conf(self, host_ip, server_port):
        rep, t, opts = self.rep, self.ctx.target, self.ctx.opts
        conf_path = opts.config_path
        txt = t.read_file(conf_path) or t.read_file(conf_path, use_sudo=True)
        rep.check("read device conf for provisioning", txt is not None, f"{conf_path}")
        if txt is None:
            raise RuntimeError("cannot read client.conf for provisioning")

        rc, _, _ = t.run(f"cp {shlex.quote(conf_path)} {shlex.quote(conf_path)}.valbak",
                         timeout=10, use_sudo=True)
        rep.check("conf backed up for restoration", rc == 0,
                  f"backup={conf_path}.valbak")
        self._conf_original = txt

        new_txt = self._rewrite_conf_text(txt, host_ip, server_port)
        self._write_conf(conf_path, new_txt)
        rep.check("conf rewritten for mock server", True,
                  f"server_address={host_ip} server_port={server_port} insecure=true "
                  "(tls_* commented out for plaintext)")
        self._restart_and_wait()

    @staticmethod
    def _rewrite_conf_text(txt: str, host_ip: str, server_port: int) -> str:
        new_lines, seen = [], set()
        for line in txt.splitlines():
            m = re.match(r"^\s*(?:#\s*)?([A-Za-z_][\w]*)\s*=", line)
            key = m.group(1) if m else None
            if key == "server_address":
                new_lines.append(f'server_address = "{host_ip}"')
                seen.add("server_address")
            elif key == "server_port":
                new_lines.append(f"server_port = {server_port}")
                seen.add("server_port")
            elif key == "insecure":
                new_lines.append("insecure = true")
                seen.add("insecure")
            elif key in ("tls_ca", "tls_cert", "tls_key"):
                new_lines.append("# " + line.lstrip())  # plaintext mock: no TLS
            else:
                new_lines.append(line)
        for key, val in (("server_address", f'"{host_ip}"'),
                         ("server_port", str(server_port)),
                         ("insecure", "true")):
            if key not in seen:
                new_lines.append(f"{key} = {val}")
        return "\n".join(new_lines) + "\n"

    def _write_conf(self, path, content):
        t = self.ctx.target
        if not t.write_file(path, content, use_sudo=True):
            b64 = base64.b64encode(content.encode()).decode()
            t.run(f"printf %s {b64} | base64 -d > {shlex.quote(path)}",
                  timeout=10, use_sudo=True)

    def _rewrite_conf_value(self, key, value):
        t, opts = self.ctx.target, self.ctx.opts
        path = opts.config_path
        txt = t.read_file(path) or t.read_file(path, use_sudo=True)
        if txt is None:
            return
        txt = re.sub(rf"^(\s*{key}\s*=\s*).*$", rf"\g<1>{value}", txt, flags=re.M)
        self._write_conf(path, txt)

    def _restart_and_wait(self):
        rep, t, opts = self.rep, self.ctx.target, self.ctx.opts
        rc, _, _ = t.run(f"systemctl restart {opts.service}", timeout=30, use_sudo=True)
        rep.check("service restarted for stream test", rc == 0, f"rc={rc}")
        up = wait_for(
            lambda: t.run(f"systemctl is-active {opts.service}", timeout=5)[1].strip()
            == "active", 30)
        rep.check("service active after restart", up is not None)

    def _restore_conf(self):
        rep, t, opts = self.rep, self.ctx.target, self.ctx.opts
        conf_path = opts.config_path
        rc, _, _ = t.run(f"mv {conf_path}.valbak {conf_path}",
                         timeout=10, use_sudo=True)
        rc2, _, _ = t.run(f"systemctl restart {opts.service}", timeout=30, use_sudo=True)
        up = wait_for(lambda: t.run(f"systemctl is-active {opts.service}",
                                    timeout=5)[1].strip() == "active", 30)
        rep.check("original conf restored, service active again",
                  rc == 0 and rc2 == 0 and up is not None, f"{conf_path}")
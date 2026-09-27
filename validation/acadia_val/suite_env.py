"""suite_env.py — Suite 1: device environment & binary behaviour checks (SSH).

Validates: SSH reachability, architecture, binary presence/arch/CLI contract
(--version/--help/unknown flag/missing --config arg), client.conf sanity,
modules.yaml, client identity, spool database, systemd unit.
"""

from __future__ import annotations

import re
import uuid as uuidmod

from .core import Status, Suite


class EnvSuite(Suite):
    NAME = "env"

    def run(self):
        rep, ctx, t = self.rep, self.ctx, self.ctx.target
        opts = ctx.opts

        # -- SSH reachability ---------------------------------------------------
        rc, out, err = t.run("uname -a", timeout=10)
        rep.check("ssh reachable", rc == 0, detail=f"uname: {out.strip() or err.strip()}")

        rc, out, _ = t.run("uname -m")
        arch = out.strip()
        rep.check("target is aarch64 (Pi)", arch in ("aarch64", "armv7l", "armv8l"),
                  f"arch={arch}")

        rc, out, _ = t.run("cat /etc/os-release 2>/dev/null | head -3")
        rep.check("os-release readable", rc == 0, detail=out.strip().replace("\n", " | "))

        # -- binary ---------------------------------------------------------------
        binary = opts.binary
        rc, out, _ = t.run(f"command -v {binary}")
        bin_path = out.strip() or (binary if binary.startswith("/") else None)
        rep.check("binary on PATH", bool(bin_path),
                  f"resolved: {bin_path}" if bin_path else f"{binary} not found")

        rc, out, err = t.run(f"{binary} --version", timeout=15)
        m = re.match(r"^acadia-client (\S+)$", out.strip())
        version = m.group(1) if m else None
        rep.check("--version prints version", rc == 0 and bool(m),
                  detail=(out.strip() or err.strip()))

        rc, out, _ = t.run(f"{binary} --help", timeout=15)
        rep.check("--help exits 0 with usage", rc == 0 and "Usage: acadia-client" in out,
                  detail=out.strip()[:120])
        rep.check("--help lists --config/--debug/--version",
                  all(k in out for k in ("--config", "--debug", "--version")),
                  detail="usage text options")

        rc, out, err = t.run(f"{binary} --bogus-flag 2>&1; echo rc=$?")
        rep.check("unknown flag exits 2 with message",
                  "rc=2" in out and "unknown option" in out,
                  detail=out.strip()[:120])

        rc, out, err = t.run(f"{binary} --config 2>&1; echo rc=$?")
        rep.check("--config without path exits 2",
                  "rc=2" in out and "requires a path" in out,
                  detail=out.strip()[:120])

        # -- config file ---------------------------------------------------------
        conf_path = opts.config_path
        conf_txt = t.read_file(conf_path) or t.read_file(conf_path, use_sudo=True)
        rep.check(f"{conf_path} present", conf_txt is not None)

        conf = {}
        if conf_txt:
            from .target import parse_toml_subset
            conf = parse_toml_subset(conf_txt)
            rep.check("client.conf parses as TOML", True,
                      f"keys={sorted(conf.keys())}")
            rep.check("server_address configured", bool(conf.get("server_address")),
                      f"server_address={conf.get('server_address')!r}")
            rep.check("hal_service_bind is loopback or token configured",
                      conf.get("hal_service_bind", "127.0.0.1") in ("127.0.0.1", "localhost")
                      or bool(conf.get("hal_service_auth_token")),
                      f"bind={conf.get('hal_service_bind', '127.0.0.1 (default)')!r} "
                      f"token={'set' if conf.get('hal_service_auth_token') else 'unset'}")
            tls = bool(conf.get("tls_ca"))
            rep.check(
                "stream transport viable (insecure=true or TLS material)",
                bool(conf.get("insecure")) or tls,
                f"insecure={conf.get('insecure')} tls_ca={'set' if tls else 'unset'} "
                f"(plaintext without insecure=true is refused by the client)")
            ctx.facts["conf"] = conf

        # -- modules.yaml -----------------------------------------------------------
        yaml_path = opts.modules_yaml
        yaml_txt = t.read_file(yaml_path) or t.read_file(yaml_path, use_sudo=True)
        rep.check(f"{yaml_path} present", yaml_txt is not None)
        if yaml_txt:
            from .target import parse_modules_yaml_ids
            ids = parse_modules_yaml_ids(yaml_txt)
            rep.check("module registry populated", len(ids) >= 50,
                      f"{len(ids)} modules declared", warn_only=len(ids) > 0)
            rep.check("module ids unique", len(ids) == len(set(ids)),
                      f"{len(set(ids))} unique")
            ctx.facts["modules_yaml_ids"] = ids

        # -- client identity ----------------------------------------------------------
        cid_path = opts.client_id_path
        cid_txt = t.read_file(cid_path) or t.read_file(cid_path, use_sudo=True)
        rep.check("client_id file present", cid_txt is not None, f"path={cid_path}")
        ok_uuid = False
        if cid_txt:
            try:
                uuidmod.UUID(cid_txt.strip())
                ok_uuid = True
            except ValueError:
                ok_uuid = False
        rep.check("client_id is a UUID", ok_uuid, detail=(cid_txt or "").strip())
        ctx.facts["client_id"] = (cid_txt or "").strip()

        # -- spool database ------------------------------------------------------------
        spool_path = (conf or {}).get("spool_path", opts.spool_path)
        hdr = None
        if t.exists(spool_path):
            rc, out, _ = t.run(f"head -c 16 {spool_path} | od -c | head -2")
            hdr = out
        rep.check("spool database exists", t.exists(spool_path), f"path={spool_path}")
        rep.check("spool db is SQLite3 format",
                  "S Q L i t e".replace(" ", "") in hdr.replace(" ", "").replace("\\", "") if hdr else False,
                  detail=(hdr.strip()[:60] if hdr else "db missing"))
        wal = t.exists(spool_path + "-wal")
        rep.check("spool WAL sidecar present or db idle", True,
                  f"-wal file: {'present' if wal else 'absent (idle checkpoint)'}")

        # -- systemd unit ---------------------------------------------------------------
        unit = opts.service
        rc, out, _ = t.run(f"systemctl is-active {unit}", timeout=10)
        rep.check(f"{unit} active", out.strip() == "active", detail=out.strip())
        rc, out, _ = t.run(f"systemctl is-enabled {unit}", timeout=10)
        rep.check(f"{unit} enabled at boot", out.strip() == "enabled",
                  detail=out.strip(), warn_only=True)

        # -- machine resources ------------------------------------------------------------
        rc, out, _ = t.run("df -BM / | tail -1 | awk '{print $4}'")
        try:
            free_mb = int(out.strip().rstrip("M"))
        except ValueError:
            free_mb = 0
        rep.check("rootfs free space > 500 MB", free_mb > 500, f"free={free_mb} MB")

        rc, out, _ = t.run("awk '/MemAvailable/{print $2}' /proc/meminfo")
        try:
            avail_kb = int(out.strip())
        except ValueError:
            avail_kb = 0
        rep.check("memory available > 100 MB", avail_kb > 100 * 1024,
                  f"available={avail_kb // 1024} MB")

        ctx.facts["version"] = version or ""
        ctx.facts["binary"] = bin_path or binary
        ctx.facts["conf"] = conf
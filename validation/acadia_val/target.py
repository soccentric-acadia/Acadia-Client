"""target.py — SSH transport, TCP tunnelling and on-device helpers.

The suite runs on the x86 host; every interaction with the Raspberry Pi
target goes through this module:

  * ``run()``        — execute a shell command, return (rc, stdout, stderr)
  * ``tunnel()``     — local TCP forwarder (direct-tcpip) to reach the
                       device's loopback-only HalService with grpcio
  * ``unix_probe()`` — talk to the diag unix socket via socat/nc/python3
  * ``read_file`` / ``write_file`` — SFTP (with sudo fallback)

Credential conventions match the repo's deploy tooling:
  ACADIA_TARGET_ADDRESS / ACADIA_TARGET_USER /
  ACADIA_TARGET_SSH_KEY  / ACADIA_TARGET_PASS
"""

from __future__ import annotations

import base64
import os
import shlex
import socket
import subprocess
import threading
import time
from typing import Optional, Tuple


# ---------------------------------------------------------------------------
# Fallback transport when paramiko is not installed: use the OpenSSH binary.
# ---------------------------------------------------------------------------

class _SubprocessTransport:
    """Minimal transport built on the `ssh` binary (used only when paramiko
    is unavailable). Supports run()/read_file(); tunnelling is provided by
    `ssh -L` subprocesses; unix_probe and sudo paths are unchanged."""

    def __init__(self, host, user, password=None, key_file=None, port=22,
                 connect_timeout=10.0):
        self.host, self.user, self.port = host, user, port
        self.password = password
        base = ["ssh", "-p", str(port), "-o", "StrictHostKeyChecking=no",
                "-o", "ConnectTimeout=%d" % int(connect_timeout)]
        base += ["-o", "BatchMode=yes"] if not password else ["-o", "BatchMode=no"]
        if key_file:
            base += ["-i", os.path.expanduser(key_file)]
        self._base = base + [f"{user}@{host}"]
        rc, out, err = self.run("echo __ok__", timeout=connect_timeout + 5)
        if rc != 0 or "__ok__" not in out:
            raise RuntimeError(f"ssh connect to {user}@{host}:{port} failed: {err.strip()}")

    # -- command execution ----------------------------------------------------
    def run(self, cmd, timeout=30.0, use_sudo=False) -> Tuple[int, str, str]:
        full = self._sudo_wrap(cmd) if use_sudo else cmd
        try:
            p = subprocess.run(self._base + [full], capture_output=True,
                               text=True, timeout=timeout)
            return p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired:
            return 124, "", f"timeout after {timeout}s"

    def _sudo_wrap(self, cmd: str) -> str:
        if self.password:
            b64 = base64.b64encode(cmd.encode()).decode()
            # feed password on stdin via a subshell is unsafe to log; use sudo -S
            inner = (f"printf %s {shlex.quote(self.password)} | sudo -S -p '' "
                     f"bash -c \"echo {b64} | base64 -d | bash\"")
            return inner
        return f"sudo -n bash -c {shlex.quote(cmd)}"

    # -- file access ----------------------------------------------------------
    def read_file(self, path, use_sudo=False):
        rc, out, _ = self.run(f"cat {shlex.quote(path)}", timeout=10, use_sudo=use_sudo)
        return out if rc == 0 else None

    def write_file(self, path, content, use_sudo=False):
        b64 = base64.b64encode(content.encode()).decode()
        rc, _, _ = self.run(f"printf %s {b64} | base64 -d > {shlex.quote(path)}",
                            timeout=10, use_sudo=use_sudo)
        return rc == 0

    def exists(self, path) -> bool:
        rc, _, _ = self.run(f"test -e {shlex.quote(path)} && echo Y")
        return rc == 0

    # -- tunnel: fall back to ssh -L -------------------------------------------
    def tunnel(self, remote_host="127.0.0.1", remote_port=50052, bind="127.0.0.1"):
        # Reserve a free local port for ssh -L, then close it so ssh can bind.
        probe = socket.socket()
        probe.bind((bind, 0))
        lport = probe.getsockname()[1]
        probe.close()
        base = ["ssh", "-p", str(self.port), "-o", "StrictHostKeyChecking=no",
                "-o", "ExitOnForwardFailure=yes", "-N", "-L",
                f"{lport}:{remote_host}:{remote_port}",
                f"{self.user}@{self.host}"]
        if self.password:
            # sshpass is required for password tunnels
            if subprocess.run(["which", "sshpass"], capture_output=True).returncode != 0:
                raise RuntimeError("tunnel with password auth requires sshpass(1)")
            base = ["sshpass", "-p", self.password] + base
        proc = subprocess.Popen(base, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # wait until ssh accepts connections on the forwarded port
        deadline = time.time() + 10.0
        while time.time() < deadline:
            if proc.poll() is not None:
                raise RuntimeError("ssh -L tunnel exited immediately "
                                   "(auth or forward failure)")
            try:
                c = socket.create_connection((bind, lport), timeout=1.0)
                c.close()
                break
            except OSError:
                time.sleep(0.3)
        else:
            proc.terminate()
            raise RuntimeError("ssh -L tunnel did not come up within 10s")
        return _ProcTunnel(lport, proc)


class _ProcTunnel:
    """Handle for an `ssh -N -L` tunnel; close() terminates the process."""

    def __init__(self, local_port: int, proc):
        self._lport = local_port
        self.proc = proc

    @property
    def local_port(self):
        return self._lport

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=3)
        except Exception:
            self.proc.kill()


# ---------------------------------------------------------------------------
# Paramiko transport (preferred)
# ---------------------------------------------------------------------------

class Target:
    """SSH connection to the validation target (Raspberry Pi)."""

    def __init__(self, host: str, user: str, password: Optional[str] = None,
                 key_file: Optional[str] = None, port: int = 22,
                 connect_timeout: float = 10.0, command_timeout: float = 30.0):
        self.host = host
        self.user = user
        self.password = password
        self._command_timeout = command_timeout
        self._tunnels = []
        self._transport = None
        try:
            import paramiko  # noqa: PLC0415
        except ImportError:
            # graceful degradation to the OpenSSH binary
            self._ssh = _SubprocessTransport(host, user, password, key_file, port,
                                             connect_timeout)
            self._mode = "subprocess"
            return
        self._mode = "paramiko"
        self._client = paramiko.SSHClient()
        self._client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        kwargs = dict(
            hostname=host, port=port, username=user,
            timeout=connect_timeout, banner_timeout=connect_timeout,
            auth_timeout=connect_timeout,
            allow_agent=True,
            look_for_keys=(key_file is None and password is None),
        )
        if key_file:
            kwargs["key_filename"] = os.path.expanduser(key_file)
        if password:
            kwargs["password"] = password
        self._client.connect(**kwargs)
        self._sftp = self._client.open_sftp()
        self._tunnels = []

    # ------------------------------------------------------------------ run
    def run(self, cmd: str, timeout: Optional[float] = None,
            use_sudo: bool = False) -> Tuple[int, str, str]:
        """Execute `cmd` on the target. Returns (rc, stdout, stderr)."""
        timeout = timeout or self._command_timeout
        if self._mode == "subprocess":
            return self._ssh.run(cmd, timeout=timeout, use_sudo=use_sudo)
        if use_sudo:
            cmd = self._sudo_cmd(cmd)
        chan = self._client.get_transport().open_session(timeout=10)
        chan.settimeout(timeout)
        chan.exec_command(cmd)
        stdout = stderr = b""
        t0 = time.monotonic()
        while True:
            if chan.recv_ready():
                stdout += chan.recv(65536)
            if chan.recv_stderr_ready():
                stderr += chan.recv_stderr(65536)
            if chan.exit_status_ready() and not chan.recv_ready() \
                    and not chan.recv_stderr_ready():
                break
            if time.monotonic() - t0 > timeout:
                chan.close()
                return 124, stdout.decode(errors="replace"), \
                    stderr.decode(errors="replace") + f"\n[timeout after {timeout}s]"
            time.sleep(0.01)
        while chan.recv_ready():
            stdout += chan.recv(65536)
        while chan.recv_stderr_ready():
            stderr += chan.recv_stderr(65536)
        rc = chan.recv_exit_status()
        chan.close()
        return rc, stdout.decode(errors="replace"), stderr.decode(errors="replace")

    def _sudo_cmd(self, cmd: str) -> str:
        if self.password:
            b64 = base64.b64encode(cmd.encode()).decode()
            # -S reads the password from stdin; -p '' silences the prompt
            return (f"echo {shlex.quote(self.password)} | sudo -S -p '' "
                    f"bash -c {shlex.quote(f'echo {b64} | base64 -d | bash')}")
        return f"sudo -n bash -c {shlex.quote(cmd)}"

    def run_ok(self, cmd, timeout=None, use_sudo=False) -> Optional[str]:
        rc, out, err = self.run(cmd, timeout, use_sudo)
        return out if rc == 0 else None

    # ----------------------------------------------------------------- files
    def read_file(self, path: str, use_sudo: bool = False) -> Optional[str]:
        if self._mode == "paramiko" and not use_sudo:
            try:
                with self._sftp.open(path, "r") as fh:
                    return fh.read().decode(errors="replace")
            except IOError:
                return None
        rc, out, _ = self.run(f"cat {shlex.quote(path)}", timeout=10, use_sudo=use_sudo)
        return out if rc == 0 else None

    def write_file(self, path: str, content: str, use_sudo: bool = False) -> bool:
        b64 = base64.b64encode(content.encode()).decode()
        pipe = f"printf %s {b64} | base64 -d > {shlex.quote(path)}"
        if use_sudo:
            rc, _, _ = self.run(pipe, timeout=10, use_sudo=True)
            return rc == 0
        if self._mode == "paramiko":
            try:
                with self._sftp.open(path, "w") as fh:
                    fh.write(content)
                return True
            except IOError:
                return False
        rc, _, _ = self.run(pipe, timeout=10)
        return rc == 0

    def exists(self, path: str) -> bool:
        rc, _, _ = self.run(f"test -e {shlex.quote(path)} && echo yes", timeout=5)
        return rc == 0

    def is_socket(self, path: str) -> bool:
        rc, _, _ = self.run(f"test -S {shlex.quote(path)} && echo yes", timeout=5)
        return rc == 0

    # --------------------------------------------------------------- tunnel
    def tunnel(self, remote_host: str = "127.0.0.1", remote_port: int = 50052,
               bind: str = "127.0.0.1") -> "Tunnel":
        """Open a local TCP tunnel to remote_host:remote_port via the SSH
        connection (direct-tcpip).  On the device this appears as a loopback
        connection, which is exactly what the HalService auth policy allows."""
        if self._mode == "subprocess":
            return self._ssh.tunnel(remote_host, remote_port, bind)
        t = Tunnel(self._client.get_transport(), remote_host, remote_port, bind)
        self._tunnels.append(t)
        return t

    # ----------------------------------------------------------- unix socket
    def unix_probe(self, socket_path: str, payload: bytes = b"status\n",
                   timeout: float = 8.0) -> Optional[str]:
        """Connect to a unix socket on the device by shelling out through SSH
        (SSH cannot forward unix sockets directly).  Tries socat, then nc -U,
        then a tiny python3 helper.  Returns the response text or None."""
        q = shlex.quote(socket_path)
        payload_cmd = "printf " + shlex.quote(payload.decode() + chr(10))
        py_snippet = (
            "import socket,sys\n"
            "s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)\n"
            f"s.settimeout({int(timeout)})\n"
            f"s.connect({socket_path!r})\n"
            f"s.sendall({payload!r})\n"
            "try:\n    s.shutdown(socket.SHUT_WR)\n"
            "except Exception:\n    pass\n"
            "d=b''\n"
            "while True:\n"
            "    try:\n"
            "        c=s.recv(65536)\n"
            "    except Exception:\n"
            "        break\n"
            "    if not c: break\n"
            "    d+=c\n"
            "sys.stdout.write(d.decode('utf-8','replace'))"
        )
        attempts = [
            ("socat",
             f"command -v socat >/dev/null 2>&1 && "
             f"{payload_cmd} | socat -T5 - UNIX-CONNECT:{q}"),
            ("nc",
             f"command -v nc >/dev/null 2>&1 && "
             f"{payload_cmd} | nc -q1 -U {q}"),
            ("python3",
             "python3 -c " + shlex.quote(py_snippet)),
        ]
        for name, cmd in attempts:
            rc, out, err = self.run(cmd, timeout=timeout + 5)
            if rc == 0 and out:
                return out
        return None

    # ---------------------------------------------------------------- close
    def close(self) -> None:
        for t in self._tunnels:
            t.close()
        if self._mode == "paramiko":
            try:
                self._sftp.close()
            except Exception:
                pass
            try:
                self._client.close()
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Tunnel over paramiko
# ---------------------------------------------------------------------------

class Tunnel:
    """Local TCP listener forwarding to a remote host:port over the SSH
    transport (direct-tcpip channel).  Used so grpcio on the host can reach
    the device's loopback-only HalService."""

    def __init__(self, transport, remote_host: str, remote_port: int, bind="127.0.0.1"):
        import paramiko  # local import so module loads without paramiko

        self._transport = transport
        self._remote = (remote_host, remote_port)
        self._closed = threading.Event()
        self._srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._srv.bind((bind, 0))
        self._srv.listen(16)
        self.local_port = self._srv.getsockname()[1]
        self._threads: list = []
        self._accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._accept_thread.start()

    def _accept_loop(self):
        self._srv.settimeout(0.5)
        while not self._closed.is_set():
            try:
                sock, _ = self._srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            th = threading.Thread(target=self._handle, args=(sock,), daemon=True)
            th.start()
            self._threads.append(th)

    def _handle(self, sock):
        chan = None
        try:
            chan = self._transport.open_channel(
                "direct-tcpip", self._remote, sock.getpeername(), timeout=10.0)
        except Exception as e:
            try:
                sock.sendall(b"HTTP/1.1 502 tunnel-error\r\n\r\n" + str(e).encode())
            except Exception:
                pass
            sock.close()
            return
        self._pump(sock, chan)

    @staticmethod
    def _pump(sock, chan):
        def s2c():
            try:
                while True:
                    data = sock.recv(16384)
                    if not data:
                        break
                    chan.sendall(data)
            except Exception:
                pass
            finally:
                try:
                    chan.shutdown_write()
                except Exception:
                    pass

        def c2s():
            chan.settimeout(0.5)
            try:
                while True:
                    try:
                        data = chan.recv(16384)
                    except socket.timeout:
                        continue
                    if not data:
                        break
                    sock.sendall(data)
            except Exception:
                pass
            finally:
                try:
                    sock.shutdown(socket.SHUT_WR)
                except Exception:
                    pass

        t1 = threading.Thread(target=s2c, daemon=True)
        t2 = threading.Thread(target=c2s, daemon=True)
        t1.start()
        t2.start()
        t1.join()
        t2.join()
        try:
            chan.close()
        except Exception:
            pass
        try:
            sock.close()
        except Exception:
            pass

    def close(self):
        self._closed.set()
        try:
            self._srv.close()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Small helpers shared by suites
# ---------------------------------------------------------------------------

def parse_toml_subset(text: str) -> dict:
    """Parse the device client.conf using Python's tomllib (3.11+); falls
    back to a simple regex extraction for older hosts."""
    try:
        import tomllib  # noqa: PLC0415
        import io
        return tomllib.load(io.StringIO(text))
    except Exception:
        out = {}
        for m in __import__("re").finditer(
                r'^\s*([A-Za-z_][\w]*)\s*=\s*"([^"]*)"', text, __import__("re").M):
            out[m.group(1)] = m.group(2)
        for m in __import__("re").finditer(
                r'^\s*([A-Za-z_][\w]*)\s*=\s*(true|false|\d+)\s*$', text,
                __import__("re").M):
            if m.group(1) not in out:
                v = m.group(2)
                out[m.group(1)] = (v == "true") if v in ("true", "false") else int(v)
        return out


def parse_modules_yaml_ids(text: str) -> list:
    """Extract module ids from modules.yaml text (no yaml dep needed)."""
    import re
    return re.findall(r"^\s+- id:\s*(\S+)", text, re.M)


def parse_modules_yaml_modes(text: str) -> dict:
    """Extract {module_id: [MODE, ...]} from modules.yaml text.

    Modules without a modes list map to an empty list (no declared
    restriction)."""
    import re
    out = {}
    blocks = re.split(r"(?m)^\s+- id:\s*", text)[1:]
    for block in blocks:
        mid = block.split(None, 1)[0].strip()
        m = re.search(r"^\s+modes:\s*\n((?:\s+- .*\n)+)", block, re.M)
        out[mid] = [x.upper() for x in re.findall(r"-\s*(\S+)", m.group(1))] if m else []
    return out


def parse_modules_yaml_entry(text: str, module_id: str) -> Optional[dict]:
    """Return the yaml block of one module (id..next entry) as a dict of
    simple keys (id, status, modes list...)."""
    import re
    m = re.search(rf"^\s+- id:\s*{re.escape(module_id)}\s*$(.*?)(?=^\s+- id:|\Z)",
                  text, re.M | re.S)
    if not m:
        return None
    block = "- id: " + module_id + m.group(1)
    entry = {"id": module_id}
    mm = re.search(r"^\s+status:\s*(\S+)", block, re.M)
    if mm:
        entry["status"] = mm.group(1)
    modes = re.search(r"^\s+modes:\s*\n((?:\s+- .*\n)+)", block, re.M)
    if modes:
        entry["modes"] = re.findall(r"-\s*(\S+)", modes.group(1))
    return entry
"""protogen.py — on-the-fly Python gRPC stub generation from the Acadia-HAL
protos, so the validation suite always validates the live proto contract
(rather than a checked-in copy that can drift).

Generates python modules for:
  common.proto           (hal.common — ResultEnvelope, TestStatus, ...)
  hal_service.proto      (hal.HalService — the on-device inbound API)
  client_service.proto   (acadia.stream.ClientService — the outbound stream,
                          used by the mock Acadia-Server)
"""

from __future__ import annotations

import importlib
import os
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace

_HERE = os.path.dirname(os.path.abspath(__file__))
_VALIDATION_DIR = os.path.dirname(_HERE)                     # .../Acadia-Client/validation
_REPO_ROOT = os.path.dirname(_VALIDATION_DIR)                # .../Acadia-Client
_PROJECT_DIR = os.path.dirname(_REPO_ROOT)                   # .../Project

PROTOS = ("common.proto", "hal_service.proto", "client_service.proto")

_CANDIDATE_PROTO_DIRS = [
    os.environ.get("ACADIA_PROTO_DIR", ""),
    os.path.join(_PROJECT_DIR, "Acadia-HAL", "protos"),
    os.path.join(_REPO_ROOT, "..", "Acadia-HAL", "protos"),
    os.path.join(_REPO_ROOT, "protos"),
]


def find_proto_dir(override: str = "") -> str:
    """Locate the directory containing the three contract protos."""
    candidates = [override] if override else []
    candidates += [c for c in _CANDIDATE_PROTO_DIRS if c]
    for c in candidates:
        c = os.path.abspath(c)
        if all(os.path.isfile(os.path.join(c, p)) for p in PROTOS):
            return c
    raise FileNotFoundError(
        "cannot locate Acadia-HAL protos (need " + ", ".join(PROTOS) + "). "
        "Use --proto-dir to point at Acadia-HAL/protos.")


def generate(proto_dir: str = ""):
    """Compile the three protos into a temp dir and import them.

    Returns a namespace with attributes:
      common, client_service, hal_service, client_service_grpc, hal_service_grpc
    """
    src = find_proto_dir(proto_dir)

    try:
        from grpc_tools import protoc as grpc_protoc  # noqa: F401
        have_grpc_tools = True
    except ImportError:
        have_grpc_tools = False

    work = tempfile.mkdtemp(prefix="acadia-val-proto-")
    protos_in_tmp = []
    for p in PROTOS:
        shutil.copyfile(os.path.join(src, p), os.path.join(work, p))
        protos_in_tmp.append(p)

    if have_grpc_tools:
        argv = ["protoc", f"-I{work}",
                f"--python_out={work}", f"--grpc_python_out={work}"] + protos_in_tmp
        rc = grpc_protoc.main(argv)
        if rc != 0:
            raise RuntimeError(f"grpc_tools.protoc failed rc={rc}")
    else:
        for p in protos_in_tmp:
            cmd = ["protoc", f"-I{work}",
                   f"--python_out={work}", f"--grpc_python_out={work}",
                   os.path.join(work, p)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError(
                    f"protoc failed for {p}: {r.stderr}\n"
                    "install grpcio-tools (pip install -r validation/requirements.txt)")

    if work not in sys.path:
        sys.path.insert(0, work)

    ns = SimpleNamespace(
        workdir=work,
        proto_src=src,
        common=importlib.import_module("common_pb2"),
        client_service=importlib.import_module("client_service_pb2"),
        hal_service=importlib.import_module("hal_service_pb2"),
        client_service_grpc=importlib.import_module("client_service_pb2_grpc"),
        hal_service_grpc=importlib.import_module("hal_service_pb2_grpc"),
    )
    return ns
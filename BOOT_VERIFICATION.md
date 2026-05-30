# Acadia Client — Boot Verification Checklist

## Purpose

Confirm that after flashing a new image the Acadia Client starts automatically
and both gRPC listeners are reachable.  This is bench work that requires physical
access to the target device (Raspberry Pi CM / ARM Linux).

---

## Prerequisites

| Item | Detail |
|---|---|
| Target device | Raspberry Pi 5 / CM (BCM2712) running the Acadia OS image |
| Host workstation | Linux or macOS with `ssh` and `grpc_cli` (or `grpcurl`) |
| Network | Target reachable from workstation (same LAN or VPN) |
| Target IP | Set `TARGET_IP=<ip>` in your shell before running commands |

---

## Step 1 — Flash and boot

```bash
# (standard Yocto image flashing — adjust for your method)
balenaEtcher acadia-image-raspberrypi5.wic.bz2   # or use rpi-imager
```

Power on and wait ~30 s for first boot (rootfs expand + systemd initialisation).

---

## Step 2 — Confirm Client service is running

```bash
ssh acadia@$TARGET_IP systemctl is-active acadia-client
# Expected: active
```

If `inactive` or `failed`:
```bash
ssh acadia@$TARGET_IP journalctl -u acadia-client -n 50
```

---

## Step 3 — Confirm both listeners are bound

```bash
ssh acadia@$TARGET_IP ss -tlnp | grep acadia-client
# Expected two lines like:
#   LISTEN  0  4096  0.0.0.0:50051  ...  acadia-client
#   LISTEN  0  4096  0.0.0.0:50052  ...  acadia-client
```

Default ports:
- `50051` — ClientService (stream path → Acadia Server)
- `50052` — HalService   (PySDK / E2E path)

Override via `client.conf` (`hal_service_port`, `hal_service_bind`).

---

## Step 4 — Probe HalService with grpcurl

```bash
grpcurl -plaintext $TARGET_IP:50052 list
# Expected: hal.HalService

grpcurl -plaintext $TARGET_IP:50052 hal.HalService/ListModules
# Expected: JSON array of module objects (51 modules)
```

---

## Step 5 — Run a single-shot test via PySDK

From the workstation (PySDK installed):
```bash
cd Acadia-PySDK
ACADIA_CLIENT_ADDR=$TARGET_IP:50052 .venv/bin/python - <<'PY'
import acadia_sdk as ivv
with ivv.connect("$TARGET_IP:50052", timeout=10) as dev:
    modules = dev.peripherals()
    print(f"Found {len(modules)} modules: {modules[:3]}...")
    p = dev.peripheral(modules[0])
    r = p.run_oneshot()
    print(f"Result: {r}")
PY
```

Expected: prints module list and a result envelope (Status may be SKIPPED for
HARDWARE-UNVERIFIED modules — that is expected on first boot before hardware
verification).

---

## Step 6 — Confirm logs are flowing

```bash
ssh acadia@$TARGET_IP journalctl -u acadia-client -f
```

Expected log lines:
```
app: HalService listening on 0.0.0.0:50052 (no DB writes)
app: ClientService connecting to <server>:50051
```

---

## Pass criteria

- [ ] `systemctl is-active acadia-client` → `active`
- [ ] Both ports (50051, 50052) bound and listening
- [ ] `grpcurl ListModules` returns 51 modules
- [ ] Single-shot test returns a result envelope (SKIPPED is acceptable)
- [ ] No `CRITICAL` or `ERROR` lines in journal (excluding hardware-unverified info messages)

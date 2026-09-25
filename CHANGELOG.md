# Changelog

## 2026-09-25 — Task 010: Client hardware-access privileges (VB-1)

- **Parent task:** `soccentric-acadia/Acadia` `tasks/010-hal-hardware-access-privileges/`.
- **User-visible behaviour:** none in this repo directly — `systemd/acadia-client.service`
  is a dev-reference copy of the unit, not the one shipped in the image
  (that copy lives in `meta-acadia`). This change keeps the reference copy
  from drifting out of sync on the hardware-access fix.
- **Key files:** `systemd/acadia-client.service` — adds
  `SupplementaryGroups=i2c spi gpio dialout video audio disk` and
  `AmbientCapabilities=`/`CapabilityBoundingSet=CAP_NET_ADMIN CAP_NET_RAW
  CAP_SYSLOG`, matching the shipped `meta-acadia` unit.
- **Validation:** `systemd-analyze verify` — parses cleanly, no warnings.

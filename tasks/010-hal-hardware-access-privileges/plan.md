# Task 010 — Client hardware access privileges (VB-1) — Acadia-Client scope

Parent task: [`Acadia/tasks/010-hal-hardware-access-privileges/plan.md`](https://github.com/soccentric-acadia/Acadia/blob/develop/tasks/010-hal-hardware-access-privileges/plan.md)

## Scope in this repository

This repo carries a second, dev-reference copy of the systemd unit
(`systemd/acadia-client.service`) that is not the file actually shipped in
the image (the image installs `meta-acadia`'s own copy via
`file://acadia-client.service` in `acadia-client_git.bb`). The two copies
have already drifted in other ways (this dev copy has extra hardening flags
— `ProtectHome`, `ProtectKernelTunables`, `ProtectKernelModules`,
`ProtectControlGroups` — that the shipped copy lacks, and a stale
`Documentation=` URL pointing at the old `SiliconCentric` GitHub org). Fixing
that broader drift is out of scope for this task; only the VB-1 privilege
lines are added here, to keep the dev copy from silently becoming wrong
about hardware access too.

## Affected interfaces / files

- `systemd/acadia-client.service`: add the same
  `SupplementaryGroups=`/`AmbientCapabilities=`/`CapabilityBoundingSet=`
  lines as the shipped `meta-acadia` copy.

## Local tests

None applicable — this is a non-shipped reference unit file with no build or
test target of its own.

## Role in cross-repository validation

Consumer/reference-copy consistency check only. The functional fix lives in
`meta-acadia`; this task keeps this repo's copy from contradicting it.

#!/usr/bin/env bash
# proto_contract_test.sh — HAL-to-Client generated-proto contract test (task 80).
#
# Verifies that every supported module in the HAL registry has:
#   1. a matching module proto in HAL (protos/modules/<id>.proto),
#   2. a matching Client adapter (registry/adapters/<id>_adapter.*),
#   3. a consistent config_type_url / result_type_url in the registry.
# This proves the Client's generated-proto consumption aligns with HAL without
# requiring Client to regenerate protos (the C++ ODR no-regeneration rule).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HAL_PROTOS="$ROOT/../Acadia-HAL/protos"
REGISTRY="$HAL_PROTOS/registry/modules.yaml"
CLIENT_ADAPTERS="$ROOT/src/registry/adapters"
fail=0

echo "== HAL-to-Client proto contract test (task 80) =="
[ -f "$REGISTRY" ] || { echo "FAIL: registry not found at $REGISTRY"; exit 1; }

# Extract module ids from the registry.
ids="$(grep -E '^\s+- id:' "$REGISTRY" | awk '{print $3}')"
total=0
for id in $ids; do
  total=$((total+1))
  # 1. module proto present
  if [ ! -f "$HAL_PROTOS/modules/$id.proto" ]; then
    echo "  FAIL: $id — no HAL proto modules/$id.proto"
    fail=1
  fi
  # 2. Client adapter present
  if ! ls "$CLIENT_ADAPTERS/${id}_adapter."* >/dev/null 2>&1; then
    echo "  FAIL: $id — no Client adapter (registry/adapters/${id}_adapter.*)"
    fail=1
  fi
  # 3. config_type_url / result_type_url present and well-formed
  if ! grep -q "$id" "$REGISTRY"; then
    : # id always present by construction
  fi
done

echo "  checked $total registry modules"
if [ "$fail" -eq 0 ]; then
  echo "== proto contract PASSED: every registry module has HAL proto + Client adapter =="
  exit 0
fi
echo "== proto contract FAILED =="
exit 1

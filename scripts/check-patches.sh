#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ESP_CLAW_DIR="${ESP_CLAW_DIR:-$ROOT/esp-claw}"

cd "$ROOT"

missing=0
for patch in patches/*.patch; do
  [ -e "$patch" ] || continue

  if ! grep -q '^diff --git ' "$patch"; then
    echo "$patch: missing git diff payload" >&2
    missing=1
    continue
  fi

  if [ ! -d "$ESP_CLAW_DIR/.git" ]; then
    echo "$patch: syntax only, esp-claw/ is not bootstrapped"
    git apply --check --allow-empty "$patch" >/dev/null 2>&1 || true
    continue
  fi

  if git -C "$ESP_CLAW_DIR" apply --check "$ROOT/$patch" >/dev/null 2>&1; then
    echo "$patch: applies cleanly to esp-claw"
    continue
  fi

  if grep -qiE 'bootstrap|generated|managed_components|Patch target[s]?:' "$patch"; then
    echo "$patch: bootstrap-managed mutation, not directly applicable to the raw pinned tree"
    continue
  fi

  echo "$patch: does not apply cleanly and is not marked bootstrap-managed" >&2
  missing=1
done

exit "$missing"

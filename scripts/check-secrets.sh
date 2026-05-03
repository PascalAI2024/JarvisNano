#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

patterns=(
  'sk-[A-Za-z0-9_-]{20,}'
  'xox[baprs]-[A-Za-z0-9-]{20,}'
  'ghp_[A-Za-z0-9]{20,}'
  'github_pat_[A-Za-z0-9_]{20,}'
  'AKIA[0-9A-Z]{16}'
  '-----BEGIN ((RSA|EC|OPENSSH) )?PRIVATE KEY-----'
)

status=0
for pattern in "${patterns[@]}"; do
  if git grep -nI -E -e "$pattern" -- \
      ':!dashboard/firmware/*.bin' \
      ':!images/**' \
      ':!**/*.png' \
      ':!**/*.jpg' \
      ':!**/*.jpeg' \
      ':!**/*.gif'; then
    status=1
  fi
done

if [ "$status" -ne 0 ]; then
  echo "Potential secret material found. Review before publishing." >&2
  exit "$status"
fi

echo "No obvious secret patterns found."

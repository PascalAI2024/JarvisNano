#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Patterns that should never appear in a public open-source checkout.
# These are intentionally narrow: matching the literal prefix + enough body
# to avoid catching documentation snippets like "sk-..." in a code comment.
patterns=(
  'sk-[A-Za-z0-9_-]{32,}'
  'sk-ant-[A-Za-z0-9_-]{32,}'
  'sk-proj-[A-Za-z0-9_-]{32,}'
  'xox[baprs]-[A-Za-z0-9-]{20,}'
  'ghp_[A-Za-z0-9]{36}'
  'gho_[A-Za-z0-9]{36}'
  'github_pat_[A-Za-z0-9_]{82}'
  'AKIA[0-9A-Z]{16}'
  'AIza[0-9A-Za-z_-]{35}'
  'eyJ[A-Za-z0-9_-]{30,}\.[A-Za-z0-9_-]{30,}\.[A-Za-z0-9_-]{20,}'
  '[0-9]{8,12}:AA[A-Za-z0-9_-]{30,}'
  '-----BEGIN ((RSA|EC|OPENSSH|DSA|PGP) )?PRIVATE KEY-----'
)

status=0

# 1. Source-tree scan. Skips known-binary paths so we do not get false
# positives from compiled artifacts; those are scanned separately below.
for pattern in "${patterns[@]}"; do
  if git grep -nI -E -e "$pattern" -- \
      ':!dashboard/firmware/*.bin' \
      ':!images/**' \
      ':!**/*.png' \
      ':!**/*.jpg' \
      ':!**/*.jpeg' \
      ':!**/*.gif' \
      ':!android/gradle/wrapper/gradle-wrapper.jar'; then
    status=1
  fi
done

# 2. Firmware blob scan. Anything we ship via WebSerial is a redistributable
# binary — bake-in credentials would be public the moment we publish. We use
# `strings` to extract printable runs and apply the same patterns. The
# `-----BEGIN ... PRIVATE KEY-----` marker is excluded here because mbedTLS
# bundles the literal labels in its PEM parser; matching them is a false
# positive that the source-tree scan above does not have.
firmware_patterns=(
  'sk-[A-Za-z0-9_-]{32,}'
  'sk-ant-[A-Za-z0-9_-]{32,}'
  'sk-proj-[A-Za-z0-9_-]{32,}'
  'xox[baprs]-[A-Za-z0-9-]{20,}'
  'ghp_[A-Za-z0-9]{36}'
  'gho_[A-Za-z0-9]{36}'
  'github_pat_[A-Za-z0-9_]{82}'
  'AKIA[0-9A-Z]{16}'
  'AIza[0-9A-Za-z_-]{35}'
  'eyJ[A-Za-z0-9_-]{30,}\.[A-Za-z0-9_-]{30,}\.[A-Za-z0-9_-]{20,}'
  '[0-9]{8,12}:AA[A-Za-z0-9_-]{30,}'
)

if command -v strings >/dev/null 2>&1; then
  while IFS= read -r blob; do
    [ -f "$blob" ] || continue
    for pattern in "${firmware_patterns[@]}"; do
      if strings -a "$blob" | grep -E -e "$pattern" >/dev/null; then
        echo "secret pattern '$pattern' found in $blob" >&2
        status=1
      fi
    done
  done < <(git ls-files 'dashboard/firmware/*.bin' 'firmware/**/*.bin' 2>/dev/null)
else
  echo "warning: 'strings' not available; firmware blob scan skipped" >&2
fi

if [ "$status" -ne 0 ]; then
  echo "Potential secret material found. Review before publishing." >&2
  exit "$status"
fi

echo "No obvious secret patterns found (source tree + firmware blobs)."

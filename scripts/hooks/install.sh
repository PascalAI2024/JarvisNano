#!/usr/bin/env bash
# Opt-in: point git at the repo hooks. Undo: git config --unset core.hooksPath
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
chmod +x scripts/hooks/pre-push
git config core.hooksPath scripts/hooks
echo "pre-push guard enabled (core.hooksPath=scripts/hooks)"

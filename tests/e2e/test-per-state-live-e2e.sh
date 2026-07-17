#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN=${CBM_E2E_BIN:-"$ROOT/build/codebase-memory-mcp"}
exec python3 "$ROOT/tests/e2e/test-per-state-live-e2e.py" --bin "$BIN" "$@"

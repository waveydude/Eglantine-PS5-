#!/usr/bin/env bash
# Host tests for EDEN_UP_AHEAD_GUARD_SLOTS (patch 0927) against a patched core tree.
# Usage: tools/experiments/ahead-guard-slots/run.sh [EGLANTINE_SRC_DIR]
# EGLANTINE_SRC_DIR: the core with tools/eglantine-patches applied (default: Eglantine/ next to tools/).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
core=${1:-$here/../../../Eglantine}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"${CXX:-clang++}" -std=c++20 -O1 -g -fsanitize=address,undefined -I"$here/stub" -I"$core/src" \
    "$here/test_ahead_guard.cpp" -o "$out/t"
EDEN_UP_AHEAD_GUARD=3 EDEN_UP_AHEAD_GUARD_SLOTS=2 "$out/t" slots2
EDEN_UP_AHEAD_GUARD=2 "$out/t" slots1
EDEN_UP_AHEAD_GUARD=2 EDEN_UP_AHEAD_GUARD_SLOTS=2 EDEN_UP_AHEAD_GUARD_COPY_PAGES=64 "$out/t" pool
EDEN_UP_AHEAD_GUARD=2 EDEN_UP_AHEAD_GUARD_SLOTS=2 "$out/t" stress
EDEN_UP_AHEAD_GUARD=2 "$out/t" stress | tail -2 # the control: one slot must show wrong reads

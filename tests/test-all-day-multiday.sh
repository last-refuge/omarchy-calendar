#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT
cd "$build_dir"
qmake6 "$project_dir/tests/all-day-multiday-test.pro"
make -j2 >/dev/null
./all-day-multiday-test America/New_York 2 2026-03-09
./all-day-multiday-test America/Phoenix 2 2026-03-09
./all-day-multiday-test UTC 1 2026-03-10
./all-day-multiday-test Asia/Tokyo 1 2026-03-10
echo "all-day and multi-day boundary contract: ok"

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
printf 'INFO boot\nWARN low\nERROR fail\nINFO boot\n' > "$t/log"
./loganalyzer -k boot "$t/log" > "$t/out"
grep -q 'Lines: 4' "$t/out"
grep -q 'ERROR lines: 1' "$t/out"
grep -q "Keyword 'boot' lines: 2" "$t/out"
: > "$t/empty"
./loganalyzer "$t/empty" | grep -q 'Lines: 0'
if ./loganalyzer "$t/missing" >/dev/null 2>&1; then exit 1; fi
printf 'loganalyzer tests passed\n'

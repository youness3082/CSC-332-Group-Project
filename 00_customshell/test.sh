#!/bin/bash

# test.sh - Tests the Custom Shell (cshell) and every team command inside it.

set -euo pipefail

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

passed=0
pass() { echo "PASS: $1"; passed=$((passed + 1)); }
fail() { echo "FAIL: $1"; echo "----- output -----"; cat "$tmp/out" 2>/dev/null || true; exit 1; }

# Run one command line in cshell; save output and exit status
sh_run() {
    set +e
    ./cshell -c "$1" > "$tmp/out" 2>&1
    status=$?
    set -e
}

echo "================================"
echo " Starting Custom Shell tests"
echo "================================"

# TEST 1: help lists every team command
echo ""
echo "Test 1: help lists all team commands"
sh_run "help"
for c in iotracker filecrypt processgroup loganalyzer sharedmempipe filediffadvanced; do
    grep -q "$c" "$tmp/out" || fail "help is missing $c"
done
pass "help lists all six commands"

# TEST 2: help NAME shows that command's usage
echo ""
echo "Test 2: help NAME"
sh_run "help loganalyzer"
grep -q "Usage: loganalyzer" "$tmp/out" || fail "help loganalyzer should show its usage"
sh_run "help nosuch"
[ "$status" -ne 0 ] || fail "help for an unknown name should fail"
pass "help NAME works"

# TEST 3: Command 1 iotracker
echo ""
echo "Test 3: iotracker inside the shell"
sh_run "iotracker -q -- dd if=/dev/zero of=$tmp/io.bin bs=1M count=2 status=none"
[ "$status" -eq 0 ] && grep -q "logical write : 2.0 MiB" "$tmp/out" || fail "iotracker should measure 2 MiB"
pass "iotracker runs in the shell"

# TEST 4: Command 3 filecrypt round trip
echo ""
echo "Test 4: filecrypt inside the shell"
printf 'secret message\n' > "$tmp/plain"
sh_run "filecrypt -e -p pw $tmp/plain $tmp/enc"
[ "$status" -eq 0 ] || fail "encrypt failed"
sh_run "filecrypt -d -p pw $tmp/enc $tmp/dec"
[ "$status" -eq 0 ] && cmp -s "$tmp/plain" "$tmp/dec" || fail "decrypted file differs"
pass "filecrypt round trip works"

# TEST 5: Command 4 processgroup
echo ""
echo "Test 5: processgroup inside the shell"
sh_run "processgroup -i 1 -- true ::: echo grouped"
[ "$status" -eq 0 ] && grep -q "Started PID" "$tmp/out" && grep -q "grouped" "$tmp/out" \
    || fail "processgroup should start both commands"
pass "processgroup runs in the shell"

# TEST 6: Command 5 loganalyzer
echo ""
echo "Test 6: loganalyzer inside the shell"
printf 'INFO a\nWARN b\nERROR c\nERROR d\n' > "$tmp/app.log"
sh_run "loganalyzer -k ERROR $tmp/app.log"
grep -q "ERROR lines: 2" "$tmp/out" && grep -q "Lines: 4" "$tmp/out" || fail "wrong log counts"
pass "loganalyzer runs in the shell"

# TEST 7: Command 6 sharedmempipe, including a pipe into it
echo ""
echo "Test 7: sharedmempipe inside the shell"
sh_run "sharedmempipe -m hello"
[ "$(cat "$tmp/out")" = "hello" ] || fail "sharedmempipe -m should print hello"
head -c 50000 /dev/urandom > "$tmp/rand"
sh_run "cat $tmp/rand | sharedmempipe > $tmp/rand.copy"
cmp -s "$tmp/rand" "$tmp/rand.copy" || fail "binary data through a pipe should be identical"
pass "sharedmempipe works, also in a pipeline"

# TEST 8: Command 8 filediffadvanced
echo ""
echo "Test 8: filediffadvanced inside the shell"
sh_run "filediffadvanced $tmp/plain $tmp/dec"
[ "$status" -eq 0 ] && grep -q "IDENTICAL" "$tmp/out" || fail "files should be identical"
sh_run "filediffadvanced $tmp/plain $tmp/app.log"
[ "$status" -eq 1 ] && grep -q "DIFFERENT" "$tmp/out" || fail "files should differ (exit 1)"
pass "filediffadvanced runs and returns its exit status"

# TEST 9: Normal Linux commands, quotes and redirection
echo ""
echo "Test 9: Linux commands, quotes, redirection"
sh_run "echo 'a | b' \"c d\" > $tmp/r.txt"
[ "$(cat "$tmp/r.txt")" = "a | b c d" ] || fail "quotes or > redirection wrong"
sh_run "echo more >> $tmp/r.txt"
[ "$(wc -l < "$tmp/r.txt")" -eq 2 ] || fail ">> should append"
sh_run "wc -l < $tmp/r.txt"
[ "$(tr -d ' ' < "$tmp/out")" = "2" ] || fail "< redirection wrong"
pass "Linux commands, quotes and redirection work"

# TEST 10: Three-stage pipeline
echo ""
echo "Test 10: Pipelines"
sh_run "printf 'x\ny\nz\n' | sharedmempipe | wc -l"
[ "$(tr -d ' ' < "$tmp/out")" = "3" ] || fail "pipeline output wrong"
pass "Three-stage pipeline works"

# TEST 11: Errors
echo ""
echo "Test 11: Error handling"
sh_run "no_such_command_xyz"
[ "$status" -eq 127 ] && grep -q "command not found" "$tmp/out" || fail "unknown command should exit 127"
sh_run "echo hi |"
[ "$status" -eq 2 ] || fail "dangling pipe should be a syntax error"
sh_run "echo 'unclosed"
[ "$status" -eq 2 ] || fail "unclosed quote should be a syntax error"
sh_run "cat < $tmp/missing"
[ "$status" -eq 1 ] || fail "missing input file should fail"
set +e; ./cshell -z > /dev/null 2>&1; status=$?; set -e
[ "$status" -eq 2 ] || fail "bad cshell option should exit 2"
pass "Errors reported with the right exit status"

# TEST 12: Script file, built-ins, running a command twice
echo ""
echo "Test 12: Script file with cd, repeated commands, exit"
cat > "$tmp/script" <<EOF
# comments are ignored
cd $tmp
loganalyzer app.log
loganalyzer -k WARN app.log
pwd
exit 3
echo not reached
EOF
set +e; ./cshell "$tmp/script" > "$tmp/out" 2>&1; status=$?; set -e
[ "$status" -eq 3 ] || fail "exit 3 should set the status"
[ "$(grep -c 'Lines: 4' "$tmp/out")" -eq 2 ] || fail "loganalyzer should run twice"
grep -q "Keyword 'WARN' lines: 1" "$tmp/out" || fail "second run should see -k (getopt reset)"
grep -qx "$tmp" "$tmp/out" || fail "cd should change directory"
grep -q "not reached" "$tmp/out" && fail "exit should stop the script"
pass "Script, cd, repeated commands and exit work"

# TEST 13: Commands piped into the shell's standard input
echo ""
echo "Test 13: Commands from standard input"
printf 'echo one\necho two\n' | ./cshell > "$tmp/out" 2>&1
[ "$(cat "$tmp/out")" = "$(printf 'one\ntwo')" ] || fail "stdin commands should run"
pass "Reads commands from standard input"

# TEST 14: SIGTERM to a command stops only that command
echo ""
echo "Test 14: A killed command does not kill the shell"
cat > "$tmp/script" <<'EOF'
sh -c 'kill -TERM $$'
echo shell still running
EOF
set +e; ./cshell "$tmp/script" > "$tmp/out" 2>&1; set -e
grep -q "Terminated" "$tmp/out" && grep -q "shell still running" "$tmp/out" \
    || fail "shell should report the signal and keep going"
pass "Shell survives a command killed by a signal"

echo ""
echo "================================"
echo " ALL $passed TESTS PASSED"
echo "================================"

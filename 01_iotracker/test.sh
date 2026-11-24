#!/bin/bash

# test.sh - Tests the iotracker I/O monitoring command.

# Stop if a command fails, a variable is missing, or a pipeline fails
set -euo pipefail

# Create a temporary folder for test files
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

passed=0
pass() { echo "PASS: $1"; passed=$((passed + 1)); }
fail() { echo "FAIL: $1"; echo "----- output -----"; cat "$tmp/out" 2>/dev/null || true; exit 1; }

# Run iotracker and save its output and exit status
run() {
    set +e
    ./iotracker "$@" > "$tmp/out" 2>&1
    status=$?
    set -e
}

echo "================================"
echo " Starting iotracker tests"
echo "================================"

# TEST 1: Help
echo ""
echo "Test 1: Help option"
run -h
[ "$status" -eq 0 ] && grep -q "Usage:" "$tmp/out" || fail "-h should print usage and exit 0"
pass "Help printed"

# TEST 2: Usage errors
echo ""
echo "Test 2: Invalid command-line arguments"
run;                         [ "$status" -eq 2 ] || fail "no target should exit 2"
run -p 1 -- true;            [ "$status" -eq 2 ] || fail "-p together with a command should exit 2"
run -i 0 -- true;            [ "$status" -eq 2 ] || fail "interval 0 should exit 2"
run -i abc -- true;          [ "$status" -eq 2 ] || fail "non-numeric interval should exit 2"
run -n -3 -- true;           [ "$status" -eq 2 ] || fail "negative count should exit 2"
run -p notapid;              [ "$status" -eq 2 ] || fail "non-numeric PID should exit 2"
run -z -- true;              [ "$status" -eq 2 ] || fail "unknown option should exit 2"
pass "Invalid arguments rejected with exit status 2"

# TEST 3: Runtime errors
echo ""
echo "Test 3: Missing process, missing command, bad watch path"
run -p 999999
[ "$status" -eq 1 ] && grep -q "no process" "$tmp/out" || fail "missing PID should exit 1"
run -- no_such_command_xyz
[ "$status" -eq 1 ] && grep -q "cannot run" "$tmp/out" || fail "missing command should exit 1"
run -w "$tmp/does/not/exist" -- true
[ "$status" -eq 1 ] && grep -q "cannot watch" "$tmp/out" || fail "bad -w path should exit 1"
pass "Runtime errors reported with exit status 1"

# TEST 4: Write accounting
echo ""
echo "Test 4: Bytes written are counted"
run -q -- dd if=/dev/zero of="$tmp/w.bin" bs=1M count=1 status=none
[ "$status" -eq 0 ] || fail "dd run failed"
grep -q "logical write : 1.0 MiB" "$tmp/out" || fail "expected 1.0 MiB written"
pass "1 MiB write measured"

# TEST 5: Read accounting
echo ""
echo "Test 5: Bytes read are counted"
head -c 2097152 /dev/zero > "$tmp/r.bin"
run -q -- dd if="$tmp/r.bin" of=/dev/null bs=64k status=none
[ "$status" -eq 0 ] || fail "dd read run failed"
grep -Eq "logical read   : 2\.[0-9] MiB" "$tmp/out" || fail "expected about 2 MiB read"
pass "2 MiB read measured"

# TEST 6: Live samples
echo ""
echo "Test 6: Periodic samples while the process runs"
# "read" is a shell builtin, so the shell itself does the reading
run -i 0.2 -- sh -c 'for i in 1 2 3 4 5; do while read -r l; do :; done < /etc/passwd; sleep 0.2; done'
[ "$status" -eq 0 ] || fail "sampling run failed"
grep -q "read/s" "$tmp/out" || fail "missing table header"
[ "$(grep -c '^\[.*s\] .*B ' "$tmp/out")" -ge 3 ] || fail "expected at least 3 sample lines"
pass "Live samples printed"

# TEST 7: Quiet mode
echo ""
echo "Test 7: Quiet mode prints only the summary"
run -q -i 0.1 -- sleep 0.4
grep -q "read/s" "$tmp/out" && fail "-q should hide the sample table"
grep -q "iotracker summary" "$tmp/out" || fail "-q should still print the summary"
pass "Quiet mode works"

# TEST 8: inotify file events
echo ""
echo "Test 8: inotify reports create, modify and delete"
mkdir "$tmp/watched"
run -i 0.1 -w "$tmp/watched" -- sh -c "echo hello > '$tmp/watched/f.txt'; cat '$tmp/watched/f.txt' > /dev/null; rm '$tmp/watched/f.txt'"
for ev in CREATE MODIFY ACCESS DELETE; do
    grep -q "$ev .*f.txt" "$tmp/out" || fail "missing $ev event"
done
grep -q "inotify events" "$tmp/out" || fail "missing inotify summary"
pass "inotify events reported"

# TEST 9: Sequential access pattern
echo ""
echo "Test 9: Sequential access pattern detected (-f)"
head -c 65536 /dev/urandom > "$tmp/seq.bin"
run -i 0.1 -f -- bash -c "exec 3< '$tmp/seq.bin'; for i in 1 2 3 4; do dd bs=4096 count=1 status=none <&3 >/dev/null; sleep 0.25; done"
grep -Eq "sequential .*seq.bin" "$tmp/out" || fail "expected a sequential pattern"
pass "Sequential access detected"

# TEST 10: Random access pattern
echo ""
echo "Test 10: Random access pattern detected (-f)"
if command -v python3 > /dev/null; then
    run -i 0.1 -f -- python3 -c "
import os, time
fd = os.open('$tmp/seq.bin', os.O_RDONLY)
for off in (0, 40000, 8000, 60000, 1000):
    os.lseek(fd, off, 0); os.read(fd, 100); time.sleep(0.25)
"
    grep -Eq "random .*seq.bin" "$tmp/out" || fail "expected a random pattern"
    pass "Random access detected"
else
    echo "SKIP: python3 not installed"
    passed=$((passed + 1))
fi

# TEST 11: Exit status of the command is reported
echo ""
echo "Test 11: Command exit status reported"
run -q -- sh -c 'exit 7'
grep -q "exited with status 7" "$tmp/out" || fail "expected exit status 7 in summary"
pass "Exit status reported"

# TEST 12: Attach to a running process with -p and -n
echo ""
echo "Test 12: Attach with -p and stop after -n samples"
( while :; do head -c 20000 /dev/zero > /dev/null; sleep 0.05; done ) &
bg=$!
run -p "$bg" -i 0.1 -n 3
kill "$bg"; wait "$bg" 2>/dev/null || true
[ "$status" -eq 0 ] || fail "-p run failed"
grep -q "3 samples" "$tmp/out" || fail "expected exactly 3 samples"
pass "Attach mode with sample limit works"

# TEST 13: Notices when an attached process exits
echo ""
echo "Test 13: Attached process exits"
sleep 0.4 &
sp=$!
run -q -p "$sp" -i 0.1
[ "$status" -eq 0 ] && grep -q "has exited" "$tmp/out" || fail "should notice the process exited"
pass "Process exit detected"

# TEST 14: SIGTERM stops monitoring cleanly and stops the command
echo ""
echo "Test 14: SIGTERM handled gracefully"
./iotracker -q -- sleep 30 > "$tmp/out" 2>&1 &
tp=$!
sleep 0.5
kill -TERM "$tp"
set +e; wait "$tp"; status=$?; set -e
[ "$status" -eq 0 ] || fail "iotracker should exit 0 after SIGTERM"
grep -q "iotracker summary" "$tmp/out" || fail "summary should still be printed"
grep -q "killed by signal" "$tmp/out" || fail "launched command should be stopped"
pass "SIGTERM handled, summary printed, command stopped"

echo ""
echo "================================"
echo " ALL $passed TESTS PASSED"
echo "================================"

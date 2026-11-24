#!/bin/bash
# Tests for sharedmempipe. Run with: make test   (or: bash test.sh)
# Each test prints PASS or FAIL. The script exits 1 if any test fails.

PROGRAM=./sharedmempipe
failures=0

check() {   # check "description" <command...>
    local description=$1; shift
    if "$@"; then echo "PASS  $description"
    else          echo "FAIL  $description"; failures=$((failures + 1)); fi
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# 1. A short message from -m comes out unchanged.
check "-m sends a short message" \
    test "$($PROGRAM -m 'hello world')" = 'hello world'

# 2. Lines from standard input come out unchanged.
check "stdin lines pass through" \
    test "$(printf 'one\ntwo\n' | $PROGRAM)" = $'one\ntwo'

# 3. A message longer than one 4096-byte chunk is split and rebuilt exactly.
long=$(printf '%05000d' 0)
check "-m message larger than one chunk (5000 bytes)" \
    test "$($PROGRAM -m "$long")" = "$long"

# 4. 1 MB of random binary data (256 chunks) arrives byte-for-byte identical.
head -c 1048576 /dev/urandom > "$tmp/in"
$PROGRAM < "$tmp/in" > "$tmp/out"
check "1 MB binary file via stdin is identical" cmp -s "$tmp/in" "$tmp/out"

# 5. Empty input produces empty output and exit status 0.
check "empty input gives empty output" \
    test "$($PROGRAM < /dev/null)" = ''

# 6. -h prints help and exits 0.
check "-h exits 0" $PROGRAM -h 2>/dev/null

# 7. An unknown option is rejected with exit status 2.
$PROGRAM -x 2>/dev/null; status=$?
check "unknown option exits 2" test "$status" -eq 2

# 8. -m with no text is rejected with exit status 2.
$PROGRAM -m 2>/dev/null; status=$?
check "-m without text exits 2" test "$status" -eq 2

# 9. Extra arguments are rejected with exit status 2.
$PROGRAM extra 2>/dev/null; status=$?
check "extra argument exits 2" test "$status" -eq 2

echo
if [ "$failures" -eq 0 ]; then echo "ALL TESTS PASSED"; exit 0; fi
echo "$failures TEST(S) FAILED"; exit 1

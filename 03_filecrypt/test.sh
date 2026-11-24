#!/bin/bash

# test.sh - Tests the filecrypt encryption and decryption command.

# Stop if a command fails, a variable is missing, or a pipeline fails
set -euo pipefail

# Create a temporary folder for test files
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "================================"
echo " Starting filecrypt tests"
echo "================================"

# TEST 1: Basic text encryption and decryption
echo ""
echo "Test 1: Text encryption and decryption"

printf 'secret message\n' > "$tmp/plain"

./filecrypt -e -p demo-password "$tmp/plain" "$tmp/enc"
./filecrypt -d -p demo-password "$tmp/enc" "$tmp/dec"

cmp "$tmp/plain" "$tmp/dec"

echo "PASS: Text encryption and decryption"

# TEST 2: Wrong password
echo ""
echo "Test 2: Wrong password"

if ./filecrypt -d -p incorrect "$tmp/enc" "$tmp/bad"; then
    echo "FAIL: Incorrect password was accepted"
    exit 1
fi

[ ! -e "$tmp/bad" ]

echo "PASS: Wrong password rejected"

# TEST 3: Existing output file
echo ""
echo "Test 3: Existing output file"

printf 'existing data\n' > "$tmp/existing"

if ./filecrypt -e -p demo-password "$tmp/plain" "$tmp/existing"; then
    echo "FAIL: Existing output file was overwritten"
    exit 1
fi

[ "$(cat "$tmp/existing")" = "existing data" ]

echo "PASS: Existing output file was protected"

# TEST 4: Same input and output filename
echo ""
echo "Test 4: Same input and output filename"

if ./filecrypt -e -p demo-password "$tmp/plain" "$tmp/plain"; then
    echo "FAIL: Same input and output filename was accepted"
    exit 1
fi

echo "PASS: Same input and output filename rejected"

# TEST 5: Binary file encryption and decryption
echo ""
echo "Test 5: Binary file encryption and decryption"

printf '\0\1\2' > "$tmp/binary"

./filecrypt -e -p demo-password "$tmp/binary" "$tmp/binary.enc"
./filecrypt -d -p demo-password "$tmp/binary.enc" "$tmp/binary.dec"

cmp "$tmp/binary" "$tmp/binary.dec"

echo "PASS: Binary file encryption and decryption"

# TEST 6: Empty file encryption and decryption
echo ""
echo "Test 6: Empty file encryption and decryption"

: > "$tmp/empty"

./filecrypt -e -p demo-password "$tmp/empty" "$tmp/empty.enc"
./filecrypt -d -p demo-password "$tmp/empty.enc" "$tmp/empty.dec"

cmp "$tmp/empty" "$tmp/empty.dec"

echo "PASS: Empty file encryption and decryption"

# TEST 7: Corrupted encrypted file
echo ""
echo "Test 7: Corrupted encrypted file"

cp "$tmp/enc" "$tmp/corrupted"

# Change the last byte of the authentication tag
printf '\x00' | dd of="$tmp/corrupted" \
    bs=1 seek=$(($(stat -c%s "$tmp/corrupted") - 1)) \
    conv=notrunc status=none

if ./filecrypt -d -p demo-password "$tmp/corrupted" "$tmp/corrupted.dec"; then
    echo "FAIL: Corrupted file was accepted"
    exit 1
fi

[ ! -e "$tmp/corrupted.dec" ]

echo "PASS: Corrupted file rejected"

# TEST 8: Missing input file
echo ""
echo "Test 8: Missing input file"

if ./filecrypt -e -p demo-password "$tmp/missing" "$tmp/missing.enc"; then
    echo "FAIL: Missing input file was accepted"
    exit 1
fi

[ ! -e "$tmp/missing.enc" ]

echo "PASS: Missing input file rejected"

# TEST 9: Invalid command-line arguments
echo ""
echo "Test 9: Invalid command-line arguments"

if ./filecrypt -e "$tmp/plain" "$tmp/invalid"; then
    echo "FAIL: Invalid arguments were accepted"
    exit 1
fi

echo "PASS: Invalid arguments rejected"

# TEST 10: Invalid encrypted file format
echo ""
echo "Test 10: Invalid encrypted file format"

# Create a file large enough to pass the minimum size check
printf 'INVALID_FORMAT_12345678901234567890123456789012345678901234567890' > "$tmp/invalid.enc"

if ./filecrypt -d -p demo-password "$tmp/invalid.enc" "$tmp/invalid.dec"; then
    echo "FAIL: Invalid file format was accepted"
    exit 1
fi

[ ! -e "$tmp/invalid.dec" ]

echo "PASS: Invalid file format rejected"

# TEST 11: Output file permissions
echo ""
echo "Test 11: Output file permissions"

./filecrypt -e -p demo-password "$tmp/plain" "$tmp/permissions.enc"

permissions=$(stat -c "%a" "$tmp/permissions.enc")

if [ "$permissions" != "600" ]; then
    echo "FAIL: Output file permissions are incorrect"
    exit 1
fi

echo "PASS: Output file permissions are 600"

# Final test results
echo ""
echo "================================"
echo " ALL 11 TESTS PASSED"
echo "================================"
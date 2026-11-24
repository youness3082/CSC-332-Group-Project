#!/bin/bash
set -euo pipefail
./processgroup -i 1 /bin/true ::: /bin/echo hello | tee /tmp/pgtest_$$
grep -q 'Started PID' /tmp/pgtest_$$
grep -q 'CPU' /tmp/pgtest_$$
rm /tmp/pgtest_$$
if ./processgroup /bin/false >/dev/null; then exit 1; fi
echo PASS

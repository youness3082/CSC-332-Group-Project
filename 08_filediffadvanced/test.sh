#!/usr/bin/env bash
set -eu
p=$(mktemp -d)
trap 'rm -rf "$p"' EXIT
run() {
    expected=$1; pattern=$2; shift 2
    status=0
    ./filediffadvanced "$@" > "$p/out" 2>&1 || status=$?
    test "$status" = "$expected"
    grep -Fq -- "$pattern" "$p/out"
    echo "PASS: $* ($pattern)"
}
printf 'hello\nworld\n' > "$p/a"
cp "$p/a" "$p/b"
run 0 IDENTICAL "$p/a" "$p/b"
printf 'hello\nWorld\n' > "$p/b"
run 1 'Differing lines: 1' -t "$p/a" "$p/b"
printf '\000\377' > "$p/a"; printf '\000\376' > "$p/b"
run 1 'Byte 1: 255 vs 254' -v "$p/a" "$p/b"
: > "$p/a"; : > "$p/b"
run 0 'compared positions: 0' -t "$p/a" "$p/b"
printf abc > "$p/b"
run 1 'differing positions: 3' "$p/a" "$p/b"
run 1 'Differing lines: 1' -t "$p/a" "$p/b"
run 2 'No such file' "$p/a" "$p/missing"
run 2 'unsupported file type' "$p/a" "$p"
run 2 Usage -z
run 2 Usage "$p/a"
run 0 Usage -h
printf 'a\n' > "$p/a"; printf a > "$p/b"
run 1 'Differing lines: 1' -t "$p/a" "$p/b"

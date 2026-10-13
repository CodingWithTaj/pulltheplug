#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

make -s recorder.so

tmp=$(mktemp -d)
trap 'rm -rf "$tmp" examples/checkout/shop.db' EXIT
trace=$tmp/trace

PULLTHEPLUG_TRACE=$trace LD_PRELOAD=$PWD/recorder.so python3 examples/checkout/app.py >/dev/null

fail() { echo "FAIL: $1"; exit 1; }

grep -Eq '^[0-9]+ [0-9]+ open [0-9]+ [^ ]+ .*/examples/checkout/shop\.db$' "$trace" || fail "no open of shop.db"
grep -Eq '^[0-9]+ [0-9]+ write ' "$trace" || fail "no write"
grep -Eq '^[0-9]+ [0-9]+ fsync ' "$trace" || fail "no fsync"

# every recorded write should have its bytes in the data file
want=$(awk '$3 == "write" { s += $6 } END { print s + 0 }' "$trace")
got=$(stat -c %s "$trace.data")
[ "$want" = "$got" ] || fail "data file is $got bytes, trace says $want"

echo ok

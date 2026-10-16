#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

make -s recorder.so

tmp=$(mktemp -d)
trap 'rm -rf "$tmp" examples/checkout/shop.db' EXIT

fail() { echo "FAIL: $1"; exit 1; }

replay() { python3 replay.py "$1" --invariants examples/checkout/invariants.sql; }

# the checkout example has to be flagged
PULLTHEPLUG_TRACE=$tmp/buggy LD_PRELOAD=$PWD/recorder.so python3 examples/checkout/app.py >/dev/null
status=0
replay "$tmp/buggy" >"$tmp/buggy.out" || status=$?
[ "$status" = 1 ] || fail "checkout bug: replay exited $status, wanted 1"
grep -q '^first failure' "$tmp/buggy.out" || fail "checkout bug: no failure reported"
grep -q 'HAVING i.stock' "$tmp/buggy.out" || fail "checkout bug: invariant not shown"

# same app with both writes in one transaction must come out clean
mkdir "$tmp/fixed"
cat >"$tmp/fixed/app.py" <<'PY'
import os, sqlite3, sys
conn = sqlite3.connect(os.path.join(os.path.dirname(__file__), "shop.db"))
conn.executescript("""
    CREATE TABLE inventory (item TEXT PRIMARY KEY, stock INTEGER NOT NULL);
    CREATE TABLE orders (id INTEGER PRIMARY KEY, item TEXT NOT NULL);
    INSERT INTO inventory VALUES ('widget', 100);
""")
for _ in range(5):
    conn.execute("INSERT INTO orders (item) VALUES ('widget')")
    conn.execute("UPDATE inventory SET stock = stock - 1 WHERE item = 'widget'")
    conn.commit()
PY
PULLTHEPLUG_TRACE=$tmp/fixed.trace LD_PRELOAD=$PWD/recorder.so python3 "$tmp/fixed/app.py"
replay "$tmp/fixed.trace" >"$tmp/fixed.out" || fail "single-transaction version was flagged: $(cat "$tmp/fixed.out")"

echo ok

#!/usr/bin/env python3
"""Replay a pulltheplug trace and run invariants against each crash state."""
import argparse
import os
import sqlite3
import sys
import tempfile
from typing import NamedTuple
from urllib.parse import unquote


class Op(NamedTuple):
    pid: int
    name: str
    args: list


class Result(NamedTuple):
    status: str  # ok, fail or skip
    sql: str | None = None
    detail: object = None


def parse_trace(path):
    ops = []
    with open(path) as f:
        for line in f:
            _seq, pid, name, *args = line.split()
            if name == "open":
                args[2] = unquote(args[2])
            elif name in ("unlink", "rename"):
                args = [unquote(a) for a in args]
            ops.append(Op(int(pid), name, args))
    return ops


def write_at(buf, off, chunk):
    if off > len(buf):
        buf.extend(bytes(off - len(buf)))
    buf[off:off + len(chunk)] = chunk


class Fs:
    """The recorded files as they stand after replaying some prefix of the trace."""

    def __init__(self, data):
        self.data = data
        self.files = {}  # path -> contents
        # Open descriptors point at the contents, not the path, so a file that
        # gets renamed or unlinked while open keeps receiving writes like an inode.
        self.fds = {}  # (pid, fd) -> (path at open time, contents)

    def apply(self, op):
        if op.name == "open":
            fd, flags, path = op.args
            if path not in self.files:
                self.files[path] = bytearray()
            elif "trunc" in flags.split(","):
                self.files[path].clear()
            self.fds[op.pid, int(fd)] = (path, self.files[path])
        elif op.name == "unlink":
            self.files.pop(op.args[0], None)
        elif op.name == "rename":
            src, dst = op.args
            if src in self.files:
                self.files[dst] = self.files.pop(src)
        elif op.name == "close":
            self.fds.pop((op.pid, int(op.args[0])), None)
        elif op.name in ("write", "truncate"):
            # a child that inherited an fd never shows us the open
            entry = self.fds.get((op.pid, int(op.args[0])))
            if entry is None:
                return
            buf = entry[1]
            if op.name == "truncate":
                size = int(op.args[1])
                del buf[size:]
                buf.extend(bytes(size - len(buf)))
            else:
                off, length, data_off = map(int, op.args[1:])
                write_at(buf, off, self.data[data_off:data_off + length])

    def dump(self, root):
        for path, buf in self.files.items():
            dest = os.path.join(root, path.lstrip("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(buf)


def split_statements(sql):
    statements, cur = [], ""
    for line in sql.splitlines(keepends=True):
        cur += line
        if sqlite3.complete_statement(cur):
            statements.append(cur.strip())
            cur = ""
    return statements


def find_db(ops, name):
    paths = {op.args[2] for op in ops if op.name == "open"}
    dbs = sorted(p for p in paths if not p.endswith(("-journal", "-wal")))
    if name:
        dbs = [p for p in dbs if p == name or p.endswith("/" + name)]
    if len(dbs) != 1:
        sys.exit(f"replay: can't tell which database to check, use --db (candidates: {dbs or 'none'})")
    return dbs[0]


def check_state(fs, db_path, statements):
    with tempfile.TemporaryDirectory() as root:
        fs.dump(root)
        # Opening the database is what makes SQLite roll back a hot journal or
        # recover the WAL, which is the same thing the app would do on restart.
        # That happens on a throwaway copy, so every crash point starts clean.
        conn = sqlite3.connect(os.path.join(root, db_path.lstrip("/")))
        try:
            for sql in statements:
                rows = conn.execute(sql).fetchall()
                if rows:
                    return Result("fail", sql, rows)
        except sqlite3.OperationalError as e:
            # usually a crash before the schema was created: nothing to check yet
            return Result("skip", detail=str(e))
        except sqlite3.DatabaseError as e:
            return Result("fail", detail=f"database is damaged: {e}")
        finally:
            conn.close()
    return Result("ok")


def crash_points(ops):
    points = [i for i, op in enumerate(ops) if op.name == "fsync"]
    if points[-1:] != [len(ops) - 1]:
        points.append(len(ops) - 1)
    return points


def run(ops, data, db_path, statements):
    """Yield (trace line, label, result) for every crash point."""
    fs = Fs(data)
    points = set(crash_points(ops))
    for i, op in enumerate(ops):
        fs.apply(op)
        if i in points:
            if op.name == "fsync":
                label = "fsync " + os.path.basename(fs.fds[op.pid, int(op.args[0])][0])
            else:
                label = "end of trace"
            yield i + 1, label, check_state(fs, db_path, statements)


def report(results):
    first_fail = None
    for line, label, res in results:
        print(f"  line {line:<5} {label:<28} {res.status}")
        if res.status == "fail" and first_fail is None:
            first_fail = (line, label, res)
    fails = sum(r.status == "fail" for _, _, r in results)
    skips = sum(r.status == "skip" for _, _, r in results)
    print(f"\n{len(results)} crash points, {fails} failed, {skips} skipped")
    if skips == len(results):
        print(f"every point was skipped, last reason: {results[-1][2].detail}")
    if first_fail:
        line, label, res = first_fail
        print(f"\nfirst failure: crash after trace line {line} ({label})")
        if res.sql:
            print("invariant returned rows:\n")
            print("    " + res.sql.replace("\n", "\n    "))
            print("\nrows:")
            for row in res.detail:
                print("   ", row)
        else:
            print(res.detail)
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("trace")
    ap.add_argument("data", nargs="?", help="trace data file (default: <trace>.data)")
    ap.add_argument("--invariants", required=True, help="SQL file; every query should return no rows")
    ap.add_argument("--db", help="database to check, if the trace touches more than one")
    args = ap.parse_args()

    ops = parse_trace(args.trace)
    if not ops:
        sys.exit("replay: empty trace")
    with open(args.data or args.trace + ".data", "rb") as f:
        data = f.read()
    with open(args.invariants) as f:
        statements = split_statements(f.read())

    db_path = find_db(ops, args.db)
    print(f"{len(ops)} operations, checking {db_path}\n")
    results = list(run(ops, data, db_path, statements))
    sys.exit(1 if report(results) else 0)


if __name__ == "__main__":
    main()

# pulltheplug

Pull the plug on your app thousands of times and find out what data it loses, before your users do.

This is an early-stage project. The recorder, replayer and checker work; torn writes and directory fsync are still to come.

## The problem

SQLite itself is crash-safe: a committed transaction survives a power cut. Apps still lose data, though, because they split one logical change across several transactions. If the power dies between two of them, the database is left in a state the app never meant to write. [`examples/checkout`](examples/checkout) is a small real example: it saves an order in one transaction and decrements stock in the next, so a crash in between leaves an order that never came out of stock.

## How it works

1. **Record.** An `LD_PRELOAD` library logs every open, write, truncate, fsync, unlink and rename your program does on SQLite files (`.db`, `.sqlite`, `.sqlite3`, `-wal`, `-journal`), along with the bytes written.
2. **Replay.** Rebuild the files as they look if the power dies right after each fsync in the trace, and after the last operation.
3. **Check.** Run your invariants (plain SQL queries that should return no rows) against each crashed copy and report the ones that fail.


## Try the recorder

```sh
make
LD_PRELOAD=./recorder.so python3 examples/checkout/app.py
head pulltheplug.trace
```

```
1 11816 unlink .../examples/checkout/shop.db
2 11816 open 5 creat .../examples/checkout/shop.db
3 11816 open 6 creat .../examples/checkout/shop.db-journal
4 11816 write 6 0 512 0
5 11816 fsync 6
6 11816 write 6 0 12 512
7 11816 fsync 6
8 11816 write 5 0 4096 524
```

Each line is `seq pid call args`. For `write` the args are fd, file offset, length, and offset into `pulltheplug.trace.data`, where the written bytes are kept. Paths are absolute in the real file; they're shortened here.

Set `PULLTHEPLUG_TRACE` to write the trace somewhere else. New runs append to the trace, so delete it between runs.

## Reproduce the checkout bug by hand

`app.py N` kills itself in the middle of order N. Then check the invariant:

```sh
cd examples/checkout
python3 app.py 3
sqlite3 shop.db < invariants.sql
```

```
98|3
```

Three orders were recorded but only two widgets left stock. Making pulltheplug find this on its own, without you choosing where to crash, is what `replay.py` does.

## Replay and check

```sh
make demo
```

or by hand, after recording a run:

```sh
python3 replay.py pulltheplug.trace --invariants examples/checkout/invariants.sql
```

The data file defaults to `<trace>.data`. If the trace touches more than one database, pick one with `--db`. The exit status is 1 if any crash point breaks an invariant.

On the checkout example it reports the crash points where an order was committed but its stock update wasn't, and prints the first one with the invariant and the rows that violated it:

```
  line 70    fsync shop.db-journal        fail
  ...
first failure: crash after trace line 70 (fsync shop.db-journal)
invariant returned rows:
    ...
rows:
    (100, 1)
```

A crash point is the state of the files after replaying the trace up to an fsync. Each one is written to a fresh temp directory and opened with SQLite, which rolls back a hot journal on its own, as it would when the app restarted. Points where the invariants can't run at all, such as before the tables exist, are marked `skip`.

Replay assumes the recording started with no database files on disk, which is how the checkout example behaves.

## Status

- M0: example app with a known crash bug (done)
- M1: recorder (done)
- M2: replay and check (done)
- M3: torn writes, renames and directory fsync
- M4: run on real projects

## Limits

- Linux only (it works in WSL).
- Only dynamically linked programs. Go binaries and other statically linked programs don't go through libc's `LD_PRELOAD` hooks, so nothing gets recorded.

## License

MIT. See [LICENSE](LICENSE).

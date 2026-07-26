# tiny-lsm-kv

A persistent key-value store in C++17, modeled on an LSM-tree — the
design behind LevelDB and RocksDB, built small on purpose for
learning/reference rather than production use. It has an in-memory
memtable, a write-ahead log for durability, immutable on-disk
SSTables, and background compaction — all four of the moving parts a
real LSM-tree needs, wired together end to end.

For the architecture, on-disk formats, and the durability proof
sketch, see [DESIGN.md](DESIGN.md). For the reasoning behind specific
decisions (why fsync has to happen where it does, why the write-ahead
log's records are framed the way they are, the locking strategy behind
compaction, etc.), see [CLAUDE.md](CLAUDE.md).

## Public API

```cpp
void put(const std::string& key, const std::string& value);
std::optional<std::string> get(const std::string& key) const;
void remove(const std::string& key);
std::vector<std::pair<std::string, std::string>> scan(
    const std::string& start, const std::string& end) const;
```

`scan` returns every live (non-deleted) entry with `start <= key < end`,
in ascending key order.

## Build

Requires CMake 3.20+ and a C++17 compiler.

```bash
cmake -S . -B build
cmake --build build -j
```

Catch2 v3 is fetched automatically via `FetchContent` — no manual
install step.

## Test

```bash
ctest --test-dir build
```

This runs the full unit test suite (memtable, WAL framing and replay,
SSTable read/write, compaction, concurrent stress) plus a short pass
of the crash-recovery harness (below). 69 tests, all passing.

## Run

```bash
./build/src/tinylsm_repl [db-directory]   # defaults to ./tinylsm-data
```

Commands: `PUT <key> <value...>`, `GET <key>`, `DEL <key>`,
`SCAN <start> <end>`, `HELP`, `EXIT`.

### REPL demo

```
$ ./build/src/tinylsm_repl /tmp/tinylsm-demo
tiny-lsm-kv REPL. Data directory: "/tmp/tinylsm-demo"
Type HELP for commands, EXIT to quit.
> PUT user:1 alice
OK
> PUT user:2 bob
OK
> GET user:1
alice
> SCAN user: user;
user:1 = alice
user:2 = bob
> DEL user:1
OK
> GET user:1
(nil)
> SCAN user: user;
user:2 = bob
> EXIT
bye
```

Data survives a restart, no forced save needed — this is a fresh
process pointed at the same directory:

```
$ ./build/src/tinylsm_repl /tmp/tinylsm-demo
tiny-lsm-kv REPL. Data directory: "/tmp/tinylsm-demo"
Type HELP for commands, EXIT to quit.
> GET user:2
bob
> EXIT
bye
```

## Crash-recovery harness

`tools/crash_recovery_harness.cpp` forks a child that streams writes
into a fresh `Database`, acknowledging each one the instant `put()`
returns (i.e. the instant it's `fsync`'d). The parent `SIGKILL`s the
child at a random point — sometimes before the first write, sometimes
mid-flush, sometimes mid-compaction — then reopens the same directory
and checks that every acknowledged write survived and that reopening
itself never throws.

```bash
./build/tools/tinylsm_crash_harness [trials] [records-per-trial]   # defaults: 30 500
```

Sample run (30 trials, 500 records each, killed at a random point in
every trial):

```
PASS: 30/30 trials survived a hard kill with zero acknowledged writes lost.
```

## Benchmarks

```bash
./build/tools/tinylsm_bench [n] [lookups]   # defaults: 20000 5000
```

Numbers below are from a single run on a dev laptop, not a tuned
benchmark environment — quote the shape of the result (writes are
fsync-bound; reads barely care how many SSTables exist) rather than
the exact figures.

```
write / memtable-read throughput (20000 records)
--------------------------------------------------
  writes/sec                      1319.77 ops/s
  reads/sec (memtable)          889683.20 ops/s

point-read latency (average, over 5000 lookups)
-------------------------------------------------
  1 SSTable                          4.07 us/op
  20 SSTables                        4.11 us/op
```

Writes are slow relative to reads because every `put`/`remove`
`fsync`s the write-ahead log before acknowledging — that's the entire
durability guarantee, and it's a deliberate tradeoff, not an
oversight. Point-read latency barely moves between 1 and 20 SSTables:
each table only costs a sparse-index binary search plus a bounded
~16-entry block scan, so the "check every SSTable newest-to-oldest"
read path stays cheap even as the number of tables grows — see
DESIGN.md's read-path section for why.

## Limitations

This is a learning project, and it's honest about what it doesn't do:

- **No MVCC / snapshots.** `get()`/`scan()` see whatever the store's
  state is at the instant they take the lock — there's no consistent
  point-in-time view held across multiple calls.
- **No transactions.** Each `put`/`remove` is atomic on its own; there
  is no multi-key atomic batch.
- **Single writer.** `put`/`remove` serialize against each other (and
  against reads) with a single process-wide lock — there's no
  multi-process or multi-writer support, and no replication.
- **SSTables are read fully into memory** at open time rather than
  seeked into on disk per lookup. The sparse index still bounds how
  much of that in-memory buffer any one lookup inspects, but this
  means an SSTable has to comfortably fit in memory.
- **No compression, no bloom filters.** A `get()` for a missing key
  still has to consult the sparse index of every SSTable before
  concluding the key isn't there.

## License

No license file yet — treat as "all rights reserved" until one is
added.

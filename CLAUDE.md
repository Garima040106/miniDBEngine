# tiny-lsm-kv

## What this project is

A persistent key-value store in C++17, modeled on an LSM-tree (the
design behind LevelDB/RocksDB) — a small one, for learning/reference
rather than production use. Public API:

```cpp
void put(const std::string& key, const std::string& value);
std::optional<std::string> get(const std::string& key) const;
void remove(const std::string& key);
std::vector<std::pair<std::string, std::string>> scan(
    const std::string& start, const std::string& end) const;
```

`scan` returns every live (non-deleted) entry with `start <= key < end`,
in ascending key order.

## Why an LSM-tree

Writes only ever touch an in-memory structure and an append-only log —
never a random-access seek into a big on-disk file — so writes are fast
and durable without needing in-place updates. The cost is paid on reads
(possibly checking several places for a key) and in the background
(compaction), rather than on the write path. That tradeoff is the whole
point of the design, and it's why every read has to check newest-to-
oldest and respect tombstones: the same key can legitimately exist in
several places at once, and only the most recent write (or delete) for
that key is the truth.

## Components

- **MemTable** — an in-memory sorted map of every recent write. Sorted
  by key so it can be scanned in order and eventually flushed straight
  into a sorted SSTable with no extra sort step.
- **WAL (write-ahead log)** — an append-only on-disk log of every write,
  so the MemTable's contents can be replayed and rebuilt after a crash.
  Every `put`/`remove` appends a serialized record and `fsync`s it
  *before* the call is allowed to return to its caller — durability
  comes from the log being safely on disk, not from the in-memory copy.
  See "Write-ahead log" below for the on-disk record format, why the
  fsync has to happen where it does, and how replay decides where the
  log's valid contents actually end.
- **SSTables (sorted string tables)** *(planned)* — immutable on-disk
  files holding a sorted run of key-value entries, produced by flushing
  a full MemTable. Immutable means no in-place edits: a later write to
  the same key lives in a *newer* SSTable (or the MemTable) and shadows
  the old one at read time.
- **Compaction** *(planned)* — a background process that merges several
  SSTables into fewer, larger ones: dropping keys that are shadowed by a
  newer file, and dropping tombstones once no older file could possibly
  still need them (i.e. once nothing older than the tombstone survives
  the merge).

## Tombstones

A `remove()` doesn't erase the key from the MemTable — it can't, because
the key (and an older value for it) might already live in an on-disk
SSTable that this MemTable knows nothing about. Instead it writes a
*tombstone*: a marker entry meaning "this key was deleted as of this
point in time." The public `get()`/`scan()` API treats a tombstoned key
as absent, but the tombstone itself has to survive — in the MemTable,
through a flush to an SSTable, and through however many compactions —
until compaction can prove no older copy of that key remains anywhere
in the store, at which point (and only then) it's safe to drop for real.
Dropping a tombstone too early is the classic LSM-tree correctness bug:
it makes a deleted key reappear from an older, still-present SSTable.

## Read path

1. Check the MemTable. If the key is there (live value or tombstone),
   that's the answer — it's the newest data in the system.
2. *(planned)* If not, check on-disk SSTables from newest to oldest,
   returning the first hit (live value or tombstone).
3. If nothing is found anywhere, or the first hit found is a tombstone,
   the key is absent as far as the public API is concerned.

`scan(start, end)` is the same idea across a range instead of one key:
merge the live key ranges from the MemTable and every SSTable
(newest-wins per key), drop tombstoned keys, and return what's left in
sorted order.

## Write path

1. `put`/`remove`: append a record to the WAL and `fsync` it. The call
   does not return to its caller until the `fsync` completes — that's
   the entire durability guarantee (see "Write-ahead log" below for why
   the ordering has to be exactly this).
2. Apply the same write to the MemTable (a real value for `put`, a
   tombstone for `remove`). Whether this happens before or after the
   fsync doesn't actually change the durability guarantee (a crash at
   any point loses the whole process's memory regardless), but doing it
   after is easier to reason about, so that's the order used here.
3. *(planned)* Once the MemTable exceeds a size threshold, flush it to a
   new immutable SSTable (already sorted, so this is a straight
   sequential write) and truncate the WAL, since the MemTable's
   contents — now durable in the SSTable — no longer need log replay.

## Write-ahead log

**On-disk record format.** Keys and values are arbitrary binary-safe
`std::string`s, so delimiter-based framing (e.g. newline-separated text)
isn't safe — a key or value could contain any byte, including whatever
delimiter was chosen. Every record is instead length-prefixed:

```
offset  size       field
0       4          checksum   (uint32 LE - CRC32 over bytes [4, end) of this record)
4       1          type       (1 = PUT, 2 = DELETE)
5       4          key_len    (uint32 LE)
9       4          value_len  (uint32 LE - always 0 for DELETE)
13      key_len    key bytes
13+key_len  value_len  value bytes (absent for DELETE)
```

Total on-disk size = `13 + key_len + value_len`. The checksum comes
*first* and covers everything after it, including the length fields
themselves - `key_len`/`value_len` can't be trusted until the checksum
that covers them has already been verified, since a torn or corrupted
write could leave garbage in the length fields too.

**Detecting a torn/partial write at the tail.** A crash can interrupt a
`write()` at any byte offset, leaving the file ending mid-record. There
are exactly three places this shows up, plus a fourth failure mode that
looks similar but isn't strictly "torn":

1. Fewer than 13 bytes remain - cut off inside the fixed header.
2. The header read fine (so `key_len` is known), but fewer than
   `key_len` bytes remain after it - cut off inside the key.
3. Header and key read fine, but fewer than `value_len` bytes remain
   after that - cut off inside the value.
4. A complete-length record is physically present, but its checksum
   doesn't match (or its type byte isn't 1 or 2) - not a short read, but
   still not a trustworthy record.

All four are treated identically by the reader: **the log's valid
contents end at the offset right before this record started.** Under a
single-writer, strictly-append-only log, a partial/corrupt record can
only ever occur at the physical tail (the one record being written when
a crash happened) - every earlier record was already fully written and
`fsync`'d before the next append began. So on hitting any of the four
cases, replay stops immediately rather than scanning forward for the
next record that happens to parse; skipping past an anomaly here would
risk silently dropping an acknowledged write; if anomaly is found NOT at
the tail, that means something worse than a torn write occurred (real
corruption, a bug) and is exactly the case stopping-instead-of-skipping
is meant to catch.

**Why fsync ordering matters.** `write()` only copies bytes into the
kernel's page cache; the write can vanish on crash even after `write()`
returns. `fsync(fd)` blocks until those bytes are actually durable. So:
append the record, `fsync`, and *only after `fsync` returns* is the call
allowed to acknowledge success to its caller. Acknowledging earlier would
mean telling a caller a write is safe when it might not survive a crash.
This is also why the guarantee is "no *acknowledged* write is lost," not
"no write is ever lost": a write that crashes before its own `fsync`
returns was never acknowledged, so it's fine either way whether it
happens to survive.

Raw POSIX `open()`/`write()`/`fsync()` is used for the log file rather
than `std::ofstream` - a `FILE*`/stream has its own userspace buffer on
top of the kernel's page cache, which would need an `fflush()` (userspace
→ kernel) *before* the `fsync()` (kernel → disk) on every write, and C++
streams don't give a portable way to get at the native fd to fsync it at
all. Going straight to the POSIX calls means only one buffer to reason
about.

**How replay finds where the log ends, and what happens next.** Replay
reads records from offset 0, stopping at the first of the four failure
cases above. The offset immediately after the last successfully-decoded
record is the log's real end. That offset has to be used to **truncate
the file** before any new appends happen - simply reopening in append
mode and writing would leave any torn tail bytes sitting in the file
forever, and the *next* replay would hit that same garbage first and
stop, even though everything appended after it (this run) is perfectly
valid.

**Serialization vs. I/O.** `EncodeRecord`/`DecodeRecord`/`Crc32` in
`include/tinylsm/wal_record.h` are pure - no file handles, no fsync, just
bytes in and bytes out - specifically so the framing/checksum logic is
testable independent of any file I/O, and so the file-handling code (the
`WriteAheadLog` class: opening the file, `write`+`fsync` on append,
looping+truncating on replay) is a separate, thin layer on top.

## On-disk formats: SSTable (planned — not implemented yet)

The WAL's on-disk format is documented above in "Write-ahead log", since
it's implemented. Nothing below is built yet; this is the intended
design for when SSTables and flushing are added.

**SSTable** — immutable, written once, sorted by key:

```
[ data block  ] repeated (key_len, key, tombstone_flag, value_len, value) entries, sorted
[ index block ] sparse list of (key, offset) pairs into the data block,
                so a lookup binary-searches the index, then does one
                seek + linear scan within a small range - not a scan of
                the whole file
[ footer      ] fixed-size: offset + length of the index block, plus a
                magic number to sanity-check the file on open
```

Read at file open time: mmap or open the file, read the footer to find
the index, then keep the index in memory for lookups (this is the same
shape LevelDB uses, simplified).

## Current status

Implemented: the `KVStore` interface, `MemTable` (in-memory, `std::map`-
backed, with tombstone support), a `Database` class that currently just
delegates every call straight to one `MemTable` (no SSTables yet - see
below), a REPL (`PUT`/`GET`/`DEL`/`SCAN`), and the full write-ahead log:
both the serialization layer (`EncodeRecord`/`DecodeRecord`/`Crc32` in
`wal_record.h`/`.cpp`, pure, no file I/O) and the file-handling layer
(`WriteAheadLog` in `wal.h`/`.cpp`: append+fsync, replay with torn/
corrupt-tail detection, reset). Covered by unit tests, including the
standard CRC32 test vectors and every torn/corrupt-record case described
above, plus a simulated-restart (close, reopen, replay) test.

Not implemented yet: `Database` doesn't actually call the WAL from
`put`/`remove` or replay it on startup yet - that wiring, along with
SSTables, flushing, and compaction, is next. So there's still no real
persistence across restarts through `Database` itself yet, even though
the WAL underneath it is already durable and crash-safe on its own.

## File layout

```
tiny-lsm-kv/
├── CLAUDE.md
├── CMakeLists.txt              # top-level: project, options, adds src/ and tests/
├── include/
│   └── tinylsm/
│       ├── kv_store.h          # KVStore abstract interface
│       ├── memtable.h          # MemTable
│       ├── database.h          # Database : public KVStore (wraps a MemTable for now)
│       ├── wal_record.h        # EncodeRecord/DecodeRecord/Crc32 - pure, no file I/O
│       └── wal.h                # WriteAheadLog - append+fsync, replay, reset
├── src/
│   ├── memtable.cpp
│   ├── database.cpp
│   ├── wal_record.cpp
│   ├── wal.cpp
│   └── repl.cpp                 # main() - the PUT/GET/DEL/SCAN REPL
└── tests/
    ├── CMakeLists.txt          # fetches Catch2, defines the test binary
    ├── test_util.h              # TempDir - self-cleaning temp directory for file-backed tests
    ├── memtable_test.cpp
    ├── database_test.cpp
    ├── wal_record_test.cpp
    └── wal_test.cpp
```

## Tooling

- **Build**: CMake (3.20+), C++17. Out-of-source build:
  ```bash
  cmake -S . -B build
  cmake --build build
  ```
- **Tests**: [Catch2](https://github.com/catchorg/Catch2) v3, pulled in
  via `FetchContent` (no manual install step). Run with:
  ```bash
  ctest --test-dir build
  ```
- **REPL**: `./build/src/tinylsm_repl` after building.

## Conventions

- Keys and values are both `std::string` — no separate binary-safe byte
  type. `std::string` is already binary-safe (it doesn't stop at `\0`),
  so this isn't a real limitation, just a simplification of the API
  surface.
- `MemTable` (and later, SSTable readers) only ever deal in tombstones
  vs. real values — "is this key absent" is a question the *caller*
  (`Database`, and ultimately the public API) answers by treating a
  tombstone as absent. Internally, absent and tombstoned are two
  different states, on purpose, per the tombstone section above.
- No exceptions used for expected outcomes (a missing key isn't an
  error - that's what `std::optional` is for). Exceptions are reserved
  for real invariant violations.

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
- **WAL (write-ahead log)** *(planned)* — an append-only on-disk log of
  every write, so the MemTable's contents can be replayed and rebuilt
  after a crash. Written before the MemTable is updated, never the other
  way around — durability comes from the log, not the in-memory copy.
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

1. `put`/`remove` *(planned)*: append a record to the WAL and `fsync`
   it, so the write survives a crash even before it's visible to reads.
2. Apply the same write to the MemTable (a real value for `put`, a
   tombstone for `remove`).
3. *(planned)* Once the MemTable exceeds a size threshold, flush it to a
   new immutable SSTable (already sorted, so this is a straight
   sequential write) and truncate the WAL, since the MemTable's
   contents — now durable in the SSTable — no longer need log replay.

## On-disk formats (planned — not implemented yet)

Nothing below is built yet; this is the intended design for when
persistence is added, written down now so the read/write paths above
have something concrete to point at.

**WAL** — append-only, one record per write:

```
[ 1 byte  ] op        (0 = PUT, 1 = DELETE)
[ 4 bytes ] key_len   (little-endian uint32)
[ key_len ] key
[ 4 bytes ] value_len (little-endian uint32, 0 for DELETE)
[ value_len ] value
```

Read back sequentially at startup and replayed into a fresh MemTable to
recover any writes that were logged but never flushed to an SSTable.

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
delegates every call straight to one `MemTable` (no WAL, no flushing —
it exists so the public API doesn't have to change shape later), and a
REPL (`PUT`/`GET`/`DEL`/`SCAN`) built on top of it. Covered by unit
tests.

Not implemented: WAL, SSTables, flushing a MemTable to disk, compaction,
and therefore no actual persistence across restarts yet — everything
lives in memory only, for now.

## File layout

```
tiny-lsm-kv/
├── CLAUDE.md
├── CMakeLists.txt              # top-level: project, options, adds src/ and tests/
├── include/
│   └── tinylsm/
│       ├── kv_store.h          # KVStore abstract interface
│       ├── memtable.h          # MemTable
│       └── database.h          # Database : public KVStore (wraps a MemTable for now)
├── src/
│   ├── memtable.cpp
│   ├── database.cpp
│   └── repl.cpp                # main() - the PUT/GET/DEL/SCAN REPL
└── tests/
    ├── CMakeLists.txt          # fetches Catch2, defines the test binary
    ├── memtable_test.cpp
    └── database_test.cpp
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

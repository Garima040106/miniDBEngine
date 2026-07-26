// Prints a small results table: write throughput, read throughput
// against a pure in-memory MemTable, and point-read latency compared
// between a database backed by one SSTable versus many - the case the
// sparse index/newest-to-oldest read path (see CLAUDE.md's "Read path"
// and "SSTable" sections) exists to keep cheap.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "tinylsm/database.h"

namespace {

using Clock = std::chrono::steady_clock;

std::string Key(int i) {
    std::ostringstream oss;
    oss << "key-" << std::setw(8) << std::setfill('0') << i;
    return oss.str();
}

std::string Value(int i) { return "value-" + std::to_string(i) + "-payload-0123456789"; }

double Seconds(Clock::duration d) {
    return std::chrono::duration<double>(d).count();
}

struct Row {
    std::string label;
    double value;
    std::string unit;
};

void PrintTable(const std::string& title, const std::vector<Row>& rows) {
    std::cout << "\n" << title << "\n";
    std::cout << std::string(title.size(), '-') << "\n";
    size_t label_width = 0;
    for (const auto& r : rows) label_width = std::max(label_width, r.label.size());
    for (const auto& r : rows) {
        std::cout << "  " << std::left << std::setw(static_cast<int>(label_width + 2)) << r.label
                   << std::right << std::fixed << std::setprecision(2) << std::setw(14) << r.value
                   << " " << r.unit << "\n";
    }
}

// Write throughput and pure-MemTable read throughput: a large flush
// threshold means this run never touches disk beyond the WAL, so it
// isolates the MemTable+WAL path from SSTable/compaction overhead.
void BenchWriteAndMemtableRead(const std::filesystem::path& dir, int n) {
    std::filesystem::remove_all(dir);
    tinylsm::Database db(dir, /*flush_threshold_bytes=*/1ull << 30, /*compaction_trigger_count=*/1000);

    auto start = Clock::now();
    for (int i = 0; i < n; ++i) {
        db.put(Key(i), Value(i));
    }
    double write_secs = Seconds(Clock::now() - start);

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> pick(0, n - 1);
    std::vector<int> lookups(n);
    for (int i = 0; i < n; ++i) lookups[i] = pick(rng);

    start = Clock::now();
    for (int i : lookups) {
        auto v = db.get(Key(i));
        if (!v) { std::cerr << "unexpected miss for " << Key(i) << "\n"; std::exit(1); }
    }
    double read_secs = Seconds(Clock::now() - start);

    PrintTable("write / memtable-read throughput (" + std::to_string(n) + " records)",
               {
                   {"writes/sec", n / write_secs, "ops/s"},
                   {"reads/sec (memtable)", n / read_secs, "ops/s"},
               });

    std::filesystem::remove_all(dir);
}

// Builds a database with `n` records split into exactly `sstable_count`
// SSTables (a small flush threshold forces repeated flushes; a huge
// compaction trigger keeps them from being merged back down), then
// measures average point-read latency across `lookups` random keys.
double BenchReadLatencyMicros(const std::filesystem::path& dir, int n, int sstable_count,
                               int lookups) {
    std::filesystem::remove_all(dir);
    size_t approx_record_bytes = Key(0).size() + Value(0).size();
    size_t flush_threshold =
        std::max<size_t>(64, (approx_record_bytes * static_cast<size_t>(n)) /
                                  static_cast<size_t>(sstable_count));
    tinylsm::Database db(dir, flush_threshold, /*compaction_trigger_count=*/1000000);

    for (int i = 0; i < n; ++i) {
        db.put(Key(i), Value(i));
    }

    std::mt19937 rng(7);
    std::uniform_int_distribution<int> pick(0, n - 1);
    std::vector<int> targets(lookups);
    for (int i = 0; i < lookups; ++i) targets[i] = pick(rng);

    auto start = Clock::now();
    for (int i : targets) {
        auto v = db.get(Key(i));
        if (!v) { std::cerr << "unexpected miss for " << Key(i) << "\n"; std::exit(1); }
    }
    double total_secs = Seconds(Clock::now() - start);

    std::cout << "  (built with " << db.sstable_count() << " SSTable(s) on disk)\n";
    std::filesystem::remove_all(dir);
    return (total_secs / lookups) * 1e6;
}

}  // namespace

int main(int argc, char** argv) {
    int n = (argc > 1) ? std::atoi(argv[1]) : 20000;
    int lookups = (argc > 2) ? std::atoi(argv[2]) : 5000;

    std::filesystem::path dir = std::filesystem::temp_directory_path() / "tinylsm_bench";

    std::cout << "tinylsm benchmark: n=" << n << " records, " << lookups
              << " lookups per read test\n";

    BenchWriteAndMemtableRead(dir, n);

    std::cout << "\npoint-read latency: 1 SSTable vs many\n"
              << "---------------------------------------\n";
    double one_sst_us = BenchReadLatencyMicros(dir, n, /*sstable_count=*/1, lookups);
    double many_sst_us = BenchReadLatencyMicros(dir, n, /*sstable_count=*/20, lookups);

    PrintTable("point-read latency (average, over " + std::to_string(lookups) + " lookups)",
               {
                   {"1 SSTable", one_sst_us, "us/op"},
                   {"20 SSTables", many_sst_us, "us/op"},
               });
    std::cout << "\n  many-SSTable lookups are slower because a miss in each newer table\n"
                 "  still has to be ruled out (sparse-index binary search + bounded\n"
                 "  block scan) before falling through to the next one - see CLAUDE.md's\n"
                 "  \"Read path\" section.\n";

    return 0;
}

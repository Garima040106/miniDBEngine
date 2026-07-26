// Forks a child that opens a fresh Database and streams puts into it,
// acknowledging each one over a pipe the instant put() returns (i.e.
// the instant it's fsync'd). The parent kills the child with SIGKILL
// at a random point, reopens the same directory, and checks that
// every acknowledged write survived and that recovery itself never
// throws - no torn record left by the kill is allowed to corrupt
// replay.
//
// This complements wal_test.cpp's synthetic torn-record tests: those
// prove the *parsing* logic handles a torn tail correctly in isolation.
// This proves the whole system, killed for real by the OS at an
// unpredictable instant (possibly mid-write(), mid-flush, or
// mid-compaction), actually produces the failure shapes those tests
// assume - and recovers cleanly from them.

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>

#include "tinylsm/database.h"

namespace {

std::string Key(int i) {
    std::ostringstream oss;
    oss << "key-" << std::setw(6) << std::setfill('0') << i;
    return oss.str();
}

std::string Value(int i) { return "value-for-" + std::to_string(i); }

// Runs only in the child process. Small thresholds (versus the
// REPL/production defaults) so a few hundred records are enough to
// force several flushes and at least one compaction - exercising the
// SSTable and compaction crash-safety paths, not just the WAL.
[[noreturn]] void RunChild(const std::filesystem::path& dir, int count, int ack_fd) {
    tinylsm::Database db(dir, /*flush_threshold_bytes=*/2048, /*compaction_trigger_count=*/3);
    for (int i = 0; i < count; ++i) {
        db.put(Key(i), Value(i));
        // A short ASCII write is well under PIPE_BUF, so this lands
        // atomically in the parent's read - no torn acks to worry
        // about on this side of the harness.
        std::string msg = std::to_string(i) + "\n";
        ssize_t written = ::write(ack_fd, msg.data(), msg.size());
        (void)written;  // best-effort ack; a failed write here just means a smaller last_acked
    }
    _exit(0);  // finished before being killed - a valid, if less interesting, trial
}

// Reads acks from `ack_fd` until the child dies (EOF) or `deadline`
// passes. Returns the highest index acknowledged, or -1 if none yet.
int CollectAcks(int ack_fd, std::chrono::steady_clock::time_point deadline) {
    int last_acked = -1;
    std::string buf;
    char chunk[256];
    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = ::read(ack_fd, chunk, sizeof(chunk));
        if (n <= 0) break;  // child closed the pipe - exited or about to be killed
        buf.append(chunk, static_cast<size_t>(n));
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            last_acked = std::stoi(buf.substr(0, pos));
            buf.erase(0, pos + 1);
        }
    }
    return last_acked;
}

// One trial: fork a writer child, kill it at a random point, restart
// against the same directory, verify. Returns true if every
// acknowledged write survived and reopening didn't throw.
bool RunTrial(const std::filesystem::path& dir, int count, unsigned seed) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        std::perror("pipe");
        std::exit(1);
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("fork");
        std::exit(1);
    }
    if (pid == 0) {
        ::close(pipefd[0]);
        RunChild(dir, count, pipefd[1]);
    }
    ::close(pipefd[1]);

    // Kill after a random delay. There's no way to know the child's
    // pace in advance, so the upper bound scales with `count` (each
    // put fsyncs, so pace is roughly constant per record) - wide
    // enough that across seeds, kills land at every phase of the run:
    // before the first put, mid-stream, right around a flush or
    // compaction, or after the child has already finished.
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> delay_micros(0, count * 3000);
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::microseconds(delay_micros(rng));

    int last_acked = CollectAcks(pipefd[0], deadline);
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ::close(pipefd[0]);

    // Restart against the same directory. Reopening must neither throw
    // nor hang - both are required for "no torn record corrupts
    // recovery".
    std::unique_ptr<tinylsm::Database> db;
    try {
        db = std::make_unique<tinylsm::Database>(dir);
    } catch (const std::exception& e) {
        std::cerr << "  FAIL (seed=" << seed << "): reopening after kill threw: " << e.what()
                  << "\n";
        return false;
    }

    bool ok = true;
    for (int i = 0; i <= last_acked; ++i) {
        auto value = db->get(Key(i));
        if (!value || *value != Value(i)) {
            std::cerr << "  FAIL (seed=" << seed << "): acknowledged write " << Key(i)
                      << " missing after restart\n";
            ok = false;
        }
    }
    std::cout << "  trial seed=" << std::setw(3) << seed << ": killed after "
              << (last_acked + 1) << "/" << count << " acked writes, restart "
              << (ok ? "OK" : "FAILED") << "\n";
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    int trials = (argc > 1) ? std::atoi(argv[1]) : 30;
    int records_per_trial = (argc > 2) ? std::atoi(argv[2]) : 500;
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "tinylsm_crash_harness";

    std::cout << "crash-recovery harness: " << trials << " trials, " << records_per_trial
              << " records/trial, killed at a random point in each\n";

    int failures = 0;
    for (int t = 0; t < trials; ++t) {
        if (!RunTrial(dir, records_per_trial, /*seed=*/static_cast<unsigned>(t))) {
            ++failures;
        }
    }
    std::filesystem::remove_all(dir);

    if (failures == 0) {
        std::cout << "PASS: " << trials
                  << "/" << trials
                  << " trials survived a hard kill with zero acknowledged writes lost.\n";
        return 0;
    }
    std::cout << "FAIL: " << failures << "/" << trials << " trials lost an acknowledged write.\n";
    return 1;
}

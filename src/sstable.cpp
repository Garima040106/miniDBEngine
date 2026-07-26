#include "tinylsm/sstable.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <stdexcept>
#include <system_error>

#include "byte_util.h"
#include "posix_io.h"
#include "tinylsm/wal_record.h"

namespace tinylsm {

namespace {

// Every kSparseIndexInterval-th entry (by position, not by byte size -
// a real system would typically sample by approximate block size
// instead; sampling by count is simpler and just as effective at
// demonstrating the technique for a store this small) gets an index
// entry. Smaller means a bigger in-memory index and a shorter linear
// scan per lookup; larger is the opposite trade.
constexpr size_t kSparseIndexInterval = 16;

constexpr size_t kFooterSize = 8 + 8 + 4 + 4;  // index_offset + index_size + entry_count + magic
constexpr uint32_t kMagic = 0x53535442;        // arbitrary, but fixed - spells "SSTB" in ASCII bytes

}  // namespace

void WriteSSTable(const std::filesystem::path& path, const std::map<std::string, Entry>& entries) {
    std::string data;
    std::vector<std::pair<std::string, uint64_t>> sparse_index;

    size_t position = 0;
    for (const auto& [key, entry] : entries) {
        if (position % kSparseIndexInterval == 0) {
            sparse_index.emplace_back(key, static_cast<uint64_t>(data.size()));
        }
        const RecordType type = entry.is_tombstone ? RecordType::kDelete : RecordType::kPut;
        data += EncodeRecord(type, key, entry.value);
        ++position;
    }

    const uint64_t index_offset = data.size();
    std::string index_bytes;
    for (const auto& [key, offset] : sparse_index) {
        detail::PutUint32LE(&index_bytes, static_cast<uint32_t>(key.size()));
        index_bytes += key;
        detail::PutUint64LE(&index_bytes, offset);
    }
    const uint64_t index_size = index_bytes.size();

    std::string footer;
    detail::PutUint64LE(&footer, index_offset);
    detail::PutUint64LE(&footer, index_size);
    detail::PutUint32LE(&footer, static_cast<uint32_t>(entries.size()));
    detail::PutUint32LE(&footer, kMagic);

    // Write everything to a temp file and fsync it *before* it's visible
    // at `path` at all - a crash at any point up to here leaves nothing
    // for a reader to trip over; the rename is the single atomic step
    // that makes the new SSTable exist. This is what makes flushing
    // crash-safe, not per-entry checksums (see CLAUDE.md).
    std::filesystem::path tmp_path = path;
    tmp_path += ".tmp";

    const int fd = ::open(tmp_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        detail::ThrowErrno("failed to create SSTable temp file " + tmp_path.string());
    }
    detail::WriteAll(fd, data);
    detail::WriteAll(fd, index_bytes);
    detail::WriteAll(fd, footer);
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    if (!synced) {
        detail::ThrowErrno("failed to fsync SSTable temp file " + tmp_path.string());
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        throw std::runtime_error("failed to rename " + tmp_path.string() + " to " + path.string() +
                                  ": " + ec.message());
    }
}

SSTableReader::SSTableReader(std::filesystem::path path) : path_(std::move(path)) {
    const int fd = ::open(path_.c_str(), O_RDONLY);
    if (fd < 0) {
        detail::ThrowErrno("failed to open SSTable " + path_.string());
    }
    std::string contents;
    try {
        contents = detail::ReadAll(fd);
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);

    if (contents.size() < kFooterSize) {
        throw std::runtime_error("SSTable too small to contain a footer: " + path_.string());
    }

    const char* footer = contents.data() + contents.size() - kFooterSize;
    const uint64_t index_offset = detail::GetUint64LE(footer);
    const uint64_t index_size = detail::GetUint64LE(footer + 8);
    const uint32_t magic = detail::GetUint32LE(footer + 20);

    if (magic != kMagic) {
        throw std::runtime_error("bad SSTable magic number in " + path_.string());
    }
    if (index_offset + index_size + kFooterSize != contents.size()) {
        throw std::runtime_error("corrupt SSTable footer offsets in " + path_.string());
    }

    data_ = contents.substr(0, index_offset);

    std::string_view index_view(contents.data() + index_offset, index_size);
    while (!index_view.empty()) {
        if (index_view.size() < 4) {
            throw std::runtime_error("corrupt SSTable index in " + path_.string());
        }
        const uint32_t key_len = detail::GetUint32LE(index_view.data());
        index_view.remove_prefix(4);
        if (index_view.size() < static_cast<size_t>(key_len) + 8) {
            throw std::runtime_error("corrupt SSTable index in " + path_.string());
        }
        std::string key(index_view.substr(0, key_len));
        index_view.remove_prefix(key_len);
        const uint64_t offset = detail::GetUint64LE(index_view.data());
        index_view.remove_prefix(8);
        index_.push_back(IndexEntry{std::move(key), static_cast<size_t>(offset)});
    }
}

std::optional<Entry> SSTableReader::Find(const std::string& key) const {
    if (index_.empty()) {
        return std::nullopt;
    }

    // First sampled index entry with a key strictly greater than the
    // target - the block we want is the one *before* it, since that's
    // the last block whose starting key is <= target.
    auto it = std::upper_bound(index_.begin(), index_.end(), key,
                                [](const std::string& k, const IndexEntry& e) { return k < e.key; });
    if (it == index_.begin()) {
        // Even the first sampled key (which is always the file's very
        // first key) is greater than the target - it can't be present.
        return std::nullopt;
    }
    --it;

    const size_t start_offset = it->offset;
    const size_t bound_offset = (it + 1 != index_.end()) ? (it + 1)->offset : data_.size();
    std::string_view block(data_.data() + start_offset, bound_offset - start_offset);

    while (!block.empty()) {
        DecodedRecord record;
        if (DecodeRecord(block, &record) != DecodeStatus::kOk) {
            throw std::runtime_error("corrupt SSTable data block in " + path_.string());
        }
        if (record.key == key) {
            return Entry{record.value, record.type == RecordType::kDelete};
        }
        if (record.key > key) {
            return std::nullopt;  // sorted data - it would have been here already
        }
        block = block.substr(record.bytes_consumed);
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, Entry>> SSTableReader::ScanRaw(const std::string& start,
                                                                    const std::string& end) const {
    std::vector<std::pair<std::string, Entry>> results;
    if (index_.empty()) {
        return results;
    }

    auto it = std::upper_bound(index_.begin(), index_.end(), start,
                                [](const std::string& k, const IndexEntry& e) { return k < e.key; });
    const size_t start_offset = (it == index_.begin()) ? 0 : (it - 1)->offset;

    std::string_view block(data_.data() + start_offset, data_.size() - start_offset);
    while (!block.empty()) {
        DecodedRecord record;
        if (DecodeRecord(block, &record) != DecodeStatus::kOk) {
            throw std::runtime_error("corrupt SSTable data block in " + path_.string());
        }
        if (record.key >= end) {
            break;
        }
        if (record.key >= start) {
            results.emplace_back(record.key, Entry{record.value, record.type == RecordType::kDelete});
        }
        block = block.substr(record.bytes_consumed);
    }
    return results;
}

std::vector<std::pair<std::string, Entry>> SSTableReader::AllRaw() const {
    std::vector<std::pair<std::string, Entry>> results;
    std::string_view block(data_);
    while (!block.empty()) {
        DecodedRecord record;
        if (DecodeRecord(block, &record) != DecodeStatus::kOk) {
            throw std::runtime_error("corrupt SSTable data block in " + path_.string());
        }
        results.emplace_back(record.key, Entry{record.value, record.type == RecordType::kDelete});
        block = block.substr(record.bytes_consumed);
    }
    return results;
}

std::map<std::string, Entry> MergeSSTables(const std::vector<std::shared_ptr<SSTableReader>>& readers) {
    std::map<std::string, Entry> merged;
    for (const auto& reader : readers) {  // oldest first, so later ones correctly overwrite
        for (auto& [key, entry] : reader->AllRaw()) {
            merged[key] = std::move(entry);
        }
    }
    return merged;
}

}  // namespace tinylsm

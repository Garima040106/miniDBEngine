#include "tinylsm/database.h"

namespace tinylsm {

void Database::put(const std::string& key, const std::string& value) { memtable_.put(key, value); }

std::optional<std::string> Database::get(const std::string& key) const { return memtable_.get(key); }

void Database::remove(const std::string& key) { memtable_.remove(key); }

std::vector<std::pair<std::string, std::string>> Database::scan(const std::string& start,
                                                                  const std::string& end) const {
    return memtable_.scan(start, end);
}

}  // namespace tinylsm

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

#include "tinylsm/database.h"

namespace {

void printHelp() {
    std::cout << "Commands:\n"
                 "  PUT <key> <value...>   store value under key\n"
                 "  GET <key>              print the value for key, or (nil)\n"
                 "  DEL <key>              delete key\n"
                 "  SCAN <start> <end>     print every live key in [start, end)\n"
                 "  HELP                   show this message\n"
                 "  EXIT | QUIT            leave the REPL\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path db_dir = (argc > 1) ? argv[1] : "./tinylsm-data";
    tinylsm::Database db(db_dir);
    std::cout << "tiny-lsm-kv REPL. Data directory: " << db_dir << "\n";
    if (db.sstable_count() > 0) {
        std::cout << "Recovered " << db.sstable_count() << " SSTable(s) from a previous run.\n";
    }
    std::cout << "Type HELP for commands, EXIT to quit.\n";

    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        std::istringstream iss(line);
        std::string command;
        iss >> command;

        if (command.empty()) {
            continue;
        } else if (command == "HELP") {
            printHelp();
        } else if (command == "EXIT" || command == "QUIT") {
            break;
        } else if (command == "PUT") {
            std::string key;
            iss >> key;
            std::string value;
            std::getline(iss, value);
            if (!value.empty() && value.front() == ' ') {
                value.erase(0, 1);  // drop the separator getline left in front of the value
            }
            if (key.empty() || value.empty()) {
                std::cout << "usage: PUT <key> <value...>\n";
                continue;
            }
            db.put(key, value);
            std::cout << "OK\n";
        } else if (command == "GET") {
            std::string key;
            iss >> key;
            if (key.empty()) {
                std::cout << "usage: GET <key>\n";
                continue;
            }
            if (auto value = db.get(key)) {
                std::cout << *value << "\n";
            } else {
                std::cout << "(nil)\n";
            }
        } else if (command == "DEL") {
            std::string key;
            iss >> key;
            if (key.empty()) {
                std::cout << "usage: DEL <key>\n";
                continue;
            }
            db.remove(key);
            std::cout << "OK\n";
        } else if (command == "SCAN") {
            std::string start;
            std::string end;
            iss >> start >> end;
            if (start.empty() || end.empty()) {
                std::cout << "usage: SCAN <start> <end>\n";
                continue;
            }
            auto results = db.scan(start, end);
            if (results.empty()) {
                std::cout << "(empty)\n";
            }
            for (const auto& [resultKey, resultValue] : results) {
                std::cout << resultKey << " = " << resultValue << "\n";
            }
        } else {
            std::cout << "unknown command: " << command << " (type HELP)\n";
        }
    }

    std::cout << "bye\n";
    return 0;
}

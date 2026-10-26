#include "tinykv/database.hpp"
#include "tinykv/protocol.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #condition "\n"; \
        ++failures; \
    } \
} while (false)

std::filesystem::path temp_path(const std::string& name) {
    return std::filesystem::temp_directory_path() / ("tinykv-test-" + name + ".wal");
}

void database_basics() {
    tinykv::Database database;
    CHECK(!database.get("missing").has_value());
    database.set("alpha", "one");
    CHECK(database.get("alpha") == std::optional<std::string>("one"));
    database.set("alpha", "two");
    CHECK(database.get("alpha") == std::optional<std::string>("two"));
    CHECK(database.erase("alpha"));
    CHECK(!database.erase("alpha"));
    CHECK(!database.get("alpha").has_value());

    std::string key("k\0y", 3);
    std::string value("v\0l\n", 4);
    database.set(key, value);
    CHECK(database.get(key) == std::optional<std::string>(value));
    const auto stats = database.stats();
    CHECK(stats.keys == 1);
    CHECK(stats.writes == 3);
    CHECK(stats.deletes == 2);
}

void database_concurrency() {
    tinykv::Database database;
    constexpr int thread_count = 8;
    constexpr int values_per_thread = 1500;
    std::atomic<bool> okay{true};
    std::vector<std::thread> threads;
    for (int thread = 0; thread < thread_count; ++thread) {
        threads.emplace_back([thread, &database, &okay] {
            for (int index = 0; index < values_per_thread; ++index) {
                const auto key = "thread-" + std::to_string(thread) + "-" + std::to_string(index);
                const auto value = "value-" + std::to_string(index);
                database.set(key, value);
                const auto actual = database.get(key);
                if (!actual || *actual != value) {
                    okay = false;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(okay.load());
    CHECK(database.stats().keys == static_cast<std::size_t>(thread_count * values_per_thread));
}

void wal_recovery_and_torn_tail() {
    const auto path = temp_path("recovery");
    std::filesystem::remove(path);
    {
        tinykv::Database database(path, false);
        database.set("kept", "value");
        database.set("removed", "old");
        CHECK(database.erase("removed"));
    }
    {
        std::ofstream append(path, std::ios::binary | std::ios::app);
        append.write("TKV", 3); // Simulate a crash during the next record header.
    }
    {
        tinykv::Database recovered(path, false);
        CHECK(recovered.get("kept") == std::optional<std::string>("value"));
        CHECK(!recovered.get("removed").has_value());
        CHECK(recovered.stats().keys == 1);
        recovered.set("after-restart", "new-value");
    }
    {
        tinykv::Database recovered_again(path, false);
        CHECK(recovered_again.get("kept") == std::optional<std::string>("value"));
        CHECK(recovered_again.get("after-restart") == std::optional<std::string>("new-value"));
        CHECK(!recovered_again.get("removed").has_value());
    }
    std::filesystem::remove(path);
}

void wal_corruption_is_rejected() {
    const auto path = temp_path("corrupt");
    std::filesystem::remove(path);
    {
        tinykv::Database database(path, false);
        database.set("key", "value");
    }
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(28);
        file.put('X');
    }
    bool threw = false;
    try {
        tinykv::Database database(path, false);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    std::filesystem::remove(path);
}

void protocol_incremental_and_binary() {
    const std::vector<std::string> command{"SET", std::string("a\0b", 3), std::string("x\r\ny", 4)};
    const auto encoded = tinykv::protocol::encode_request(command);
    tinykv::protocol::RequestParser parser;
    std::string error;
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        CHECK(parser.feed(std::string_view(encoded.data() + index, 1), error));
        std::vector<std::string> parsed;
        const auto result = parser.next(parsed, error);
        if (index + 1 == encoded.size()) {
            CHECK(result == tinykv::protocol::ParseResult::complete);
            CHECK(parsed == command);
        } else {
            CHECK(result == tinykv::protocol::ParseResult::incomplete);
        }
    }
}

void protocol_pipeline_and_errors() {
    tinykv::protocol::RequestParser parser;
    std::string error;
    const auto first = tinykv::protocol::encode_request({"PING"});
    const auto second = tinykv::protocol::encode_request({"GET", "k"});
    CHECK(parser.feed(first + second, error));
    std::vector<std::string> parsed;
    CHECK(parser.next(parsed, error) == tinykv::protocol::ParseResult::complete);
    CHECK(parsed == std::vector<std::string>{"PING"});
    CHECK(parser.next(parsed, error) == tinykv::protocol::ParseResult::complete);
    CHECK(parsed == (std::vector<std::string>{"GET", "k"}));

    tinykv::protocol::RequestParser invalid;
    CHECK(invalid.feed("+PING\r\n", error));
    CHECK(invalid.next(parsed, error) == tinykv::protocol::ParseResult::error);
}

} // namespace

int main() {
    database_basics();
    database_concurrency();
    wal_recovery_and_torn_tail();
    wal_corruption_is_rejected();
    protocol_incremental_and_binary();
    protocol_pipeline_and_errors();
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All TinyKV tests passed\n";
    return 0;
}

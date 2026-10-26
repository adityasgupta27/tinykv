#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace tinykv {

class Wal;

struct DatabaseStats {
    std::size_t keys = 0;
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    std::uint64_t deletes = 0;
};

// A concurrent in-memory key/value store with an optional write-ahead log.
// Keys and values are byte strings: embedded NUL bytes are supported.
class Database {
public:
    explicit Database(std::optional<std::filesystem::path> wal_path = std::nullopt,
                      bool durable_writes = true);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    [[nodiscard]] std::optional<std::string> get(const std::string& key) const;
    void set(std::string key, std::string value);
    [[nodiscard]] bool erase(const std::string& key);
    [[nodiscard]] DatabaseStats stats() const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::string> records_;
    std::unique_ptr<Wal> wal_;
    mutable std::atomic<std::uint64_t> reads_{0};
    std::atomic<std::uint64_t> writes_{0};
    std::atomic<std::uint64_t> deletes_{0};
};

} // namespace tinykv

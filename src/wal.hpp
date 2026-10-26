#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace tinykv {

enum class WalOperation : std::uint8_t { set = 1, erase = 2 };

class Wal {
public:
    Wal(const std::filesystem::path& path, bool durable);
    ~Wal();

    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    void replay(const std::function<void(WalOperation, std::string, std::string)>& apply);
    void append(WalOperation operation, const std::string& key, const std::string& value);

private:
    std::filesystem::path path_;
    std::intptr_t handle_ = -1;
    bool durable_;
    bool poisoned_ = false;
    std::uint64_t sequence_ = 0;
};

} // namespace tinykv

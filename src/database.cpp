#include "tinykv/database.hpp"

#include "wal.hpp"

#include <mutex>
#include <stdexcept>

namespace tinykv {

Database::Database(std::optional<std::filesystem::path> wal_path, bool durable_writes) {
    if (wal_path.has_value()) {
        wal_ = std::make_unique<Wal>(*wal_path, durable_writes);
        wal_->replay([this](WalOperation operation, std::string key, std::string value) {
            if (operation == WalOperation::set) {
                records_.insert_or_assign(std::move(key), std::move(value));
            } else {
                records_.erase(key);
            }
        });
    }
}

Database::~Database() = default;

std::optional<std::string> Database::get(const std::string& key) const {
    std::shared_lock lock(mutex_);
    reads_.fetch_add(1, std::memory_order_relaxed);
    const auto found = records_.find(key);
    if (found == records_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void Database::set(std::string key, std::string value) {
    std::unique_lock lock(mutex_);
    if (wal_) {
        wal_->append(WalOperation::set, key, value);
    }
    records_.insert_or_assign(std::move(key), std::move(value));
    writes_.fetch_add(1, std::memory_order_relaxed);
}

bool Database::erase(const std::string& key) {
    std::unique_lock lock(mutex_);
    if (wal_) {
        wal_->append(WalOperation::erase, key, {});
    }
    const bool removed = records_.erase(key) != 0;
    deletes_.fetch_add(1, std::memory_order_relaxed);
    return removed;
}

DatabaseStats Database::stats() const {
    DatabaseStats result;
    {
        std::shared_lock lock(mutex_);
        result.keys = records_.size();
    }
    result.reads = reads_.load(std::memory_order_relaxed);
    result.writes = writes_.load(std::memory_order_relaxed);
    result.deletes = deletes_.load(std::memory_order_relaxed);
    return result;
}

} // namespace tinykv

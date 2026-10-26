#include "wal.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace tinykv {
namespace {

constexpr std::size_t kHeaderSize = 28;
constexpr std::uint32_t kMaxFieldBytes = 16U * 1024U * 1024U;
constexpr std::array<char, 4> kMagic{'T', 'K', 'V', '1'};

void put_u16(std::vector<char>& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xffU));
    out.push_back(static_cast<char>((value >> 8U) & 0xffU));
}

void put_u32(std::vector<char>& out, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
}

void put_u64(std::vector<char>& out, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
}

std::uint16_t get_u16(const char* bytes) {
    const auto a = static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[0]));
    const auto b = static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[1]));
    return static_cast<std::uint16_t>(a | static_cast<std::uint16_t>(b << 8U));
}

std::uint32_t get_u32(const char* bytes) {
    std::uint32_t result = 0;
    for (unsigned int i = 0; i < 4; ++i) {
        result |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << (i * 8U);
    }
    return result;
}

std::uint64_t get_u64(const char* bytes) {
    std::uint64_t result = 0;
    for (unsigned int i = 0; i < 8; ++i) {
        result |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (i * 8U);
    }
    return result;
}

std::uint32_t crc32(const char* data, std::size_t size, std::uint32_t crc = 0xffffffffU) {
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint32_t>(static_cast<unsigned char>(data[i]));
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc;
}

std::uint32_t record_crc(const char* header, const std::string& payload) {
    auto crc = crc32(header + 4, 20);
    crc = crc32(payload.data(), payload.size(), crc);
    return crc ^ 0xffffffffU;
}

std::runtime_error io_error(const std::string& action, const std::filesystem::path& path) {
#ifdef _WIN32
    const DWORD code = GetLastError();
    return std::runtime_error(action + " '" + path.string() + "' (Windows error " +
                              std::to_string(code) + ")");
#else
    const int code = errno;
    return std::runtime_error(action + " '" + path.string() + "': " +
                              std::error_code(code, std::generic_category()).message());
#endif
}

} // namespace

Wal::Wal(const std::filesystem::path& path, bool durable)
    : path_(path), durable_(durable) {
    if (path_.has_parent_path()) {
        std::filesystem::create_directories(path_.parent_path());
    }
}

Wal::~Wal() {
    if (handle_ == -1) {
        return;
    }
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    ::close(static_cast<int>(handle_));
#endif
}

void Wal::replay(const std::function<void(WalOperation, std::string, std::string)>& apply) {
    std::ifstream input(path_, std::ios::binary);
    if (!input && !std::filesystem::exists(path_)) {
        std::ofstream create(path_, std::ios::binary | std::ios::app);
        if (!create) {
            throw std::runtime_error("unable to create WAL '" + path_.string() + "'");
        }
        create.close();
        input.clear();
        input.open(path_, std::ios::binary);
    }
    if (!input) {
        throw std::runtime_error("unable to read WAL '" + path_.string() + "'");
    }

    std::uintmax_t valid_bytes = 0;
    bool truncate_tail = false;
    for (;;) {
        std::array<char, kHeaderSize> header{};
        input.read(header.data(), static_cast<std::streamsize>(header.size()));
        const auto header_bytes = input.gcount();
        if (header_bytes == 0) {
            if (input.bad()) {
                throw std::runtime_error("I/O error while reading WAL '" + path_.string() + "'");
            }
            break;
        }
        if (header_bytes != static_cast<std::streamsize>(header.size())) {
            truncate_tail = true;
            break;
        }

        if (!std::equal(kMagic.begin(), kMagic.end(), header.begin())) {
            throw std::runtime_error("invalid WAL magic at byte " + std::to_string(valid_bytes));
        }
        const auto operation_byte = static_cast<unsigned char>(header[4]);
        if ((operation_byte != static_cast<unsigned char>(WalOperation::set) &&
             operation_byte != static_cast<unsigned char>(WalOperation::erase)) ||
            header[5] != 0 || get_u16(header.data() + 6) != 0) {
            throw std::runtime_error("invalid WAL record header at byte " + std::to_string(valid_bytes));
        }

        const auto sequence = get_u64(header.data() + 8);
        const auto key_size = get_u32(header.data() + 16);
        const auto value_size = get_u32(header.data() + 20);
        const auto expected_crc = get_u32(header.data() + 24);
        if (key_size > kMaxFieldBytes || value_size > kMaxFieldBytes ||
            (operation_byte == static_cast<unsigned char>(WalOperation::erase) && value_size != 0)) {
            throw std::runtime_error("invalid WAL record lengths at byte " + std::to_string(valid_bytes));
        }
        if (sequence != sequence_ + 1) {
            throw std::runtime_error("non-contiguous WAL sequence at byte " + std::to_string(valid_bytes));
        }

        const auto payload_size = static_cast<std::size_t>(key_size) + static_cast<std::size_t>(value_size);
        std::string payload(payload_size, '\0');
        if (payload_size != 0) {
            input.read(payload.data(), static_cast<std::streamsize>(payload_size));
            if (input.gcount() != static_cast<std::streamsize>(payload_size)) {
                truncate_tail = true;
                break;
            }
        }
        if (record_crc(header.data(), payload) != expected_crc) {
            throw std::runtime_error("WAL checksum mismatch at byte " + std::to_string(valid_bytes));
        }

        std::string key(payload.data(), key_size);
        std::string value(payload.data() + key_size, value_size);
        apply(static_cast<WalOperation>(operation_byte), std::move(key), std::move(value));
        sequence_ = sequence;
        valid_bytes += static_cast<std::uintmax_t>(kHeaderSize + payload_size);
    }
    input.close();

    if (truncate_tail) {
        std::error_code error;
        std::filesystem::resize_file(path_, valid_bytes, error);
        if (error) {
            throw std::runtime_error("unable to truncate incomplete WAL tail: " + error.message());
        }
    }

#ifdef _WIN32
    const HANDLE handle = CreateFileW(path_.c_str(), GENERIC_WRITE | FILE_APPEND_DATA | SYNCHRONIZE,
                                      FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw io_error("unable to open WAL", path_);
    }
    handle_ = reinterpret_cast<std::intptr_t>(handle);
#else
    const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) {
        throw io_error("unable to open WAL", path_);
    }
    handle_ = static_cast<std::intptr_t>(fd);
#endif
}

void Wal::append(WalOperation operation, const std::string& key, const std::string& value) {
    if (poisoned_) {
        throw std::runtime_error("WAL is unusable after a previous write failure");
    }
    if (key.size() > kMaxFieldBytes || value.size() > kMaxFieldBytes ||
        (operation == WalOperation::erase && !value.empty())) {
        throw std::length_error("WAL key or value exceeds the 16 MiB record limit");
    }
    if (sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("WAL sequence number exhausted");
    }

    std::vector<char> record;
    record.reserve(kHeaderSize + key.size() + value.size());
    record.insert(record.end(), kMagic.begin(), kMagic.end());
    record.push_back(static_cast<char>(operation));
    record.push_back(0);
    put_u16(record, 0);
    put_u64(record, sequence_ + 1);
    put_u32(record, static_cast<std::uint32_t>(key.size()));
    put_u32(record, static_cast<std::uint32_t>(value.size()));
    put_u32(record, 0);
    record.insert(record.end(), key.begin(), key.end());
    record.insert(record.end(), value.begin(), value.end());
    const std::string payload = key + value;
    const auto checksum = record_crc(record.data(), payload);
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        record[24 + shift / 8] = static_cast<char>((checksum >> shift) & 0xffU);
    }

    try {
#ifdef _WIN32
        // CreateFileW starts a synchronous handle's file pointer at byte zero. Because
        // this handle also has GENERIC_WRITE (needed by FlushFileBuffers), FILE_APPEND_DATA
        // alone does not guarantee that WriteFile ignores that pointer. Reposition before
        // each record so writes after a restart cannot overwrite the beginning of the WAL.
        LARGE_INTEGER append_offset{};
        if (!SetFilePointerEx(reinterpret_cast<HANDLE>(handle_), append_offset, nullptr, FILE_END)) {
            throw io_error("unable to seek to WAL end", path_);
        }
        std::size_t written = 0;
        while (written < record.size()) {
            const auto remaining = record.size() - written;
            const DWORD chunk = static_cast<DWORD>(remaining > 0xffffffffU ? 0xffffffffU : remaining);
            DWORD count = 0;
            if (!WriteFile(reinterpret_cast<HANDLE>(handle_), record.data() + written, chunk, &count, nullptr) ||
                count == 0) {
                throw io_error("unable to append WAL", path_);
            }
            written += count;
        }
        if (durable_ && !FlushFileBuffers(reinterpret_cast<HANDLE>(handle_))) {
            throw io_error("unable to flush WAL", path_);
        }
#else
        std::size_t written = 0;
        while (written < record.size()) {
            const auto count = ::write(static_cast<int>(handle_), record.data() + written,
                                       record.size() - written);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw io_error("unable to append WAL", path_);
            }
            if (count == 0) {
                throw std::runtime_error("short write while appending WAL '" + path_.string() + "'");
            }
            written += static_cast<std::size_t>(count);
        }
        if (durable_ && ::fsync(static_cast<int>(handle_)) != 0) {
            throw io_error("unable to flush WAL", path_);
        }
#endif
        ++sequence_;
    } catch (...) {
        poisoned_ = true;
        throw;
    }
}

} // namespace tinykv

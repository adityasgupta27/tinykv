#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tinykv::protocol {

inline constexpr std::size_t kMaxArguments = 16;
inline constexpr std::size_t kMaxBulkBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaxRequestBytes = 32U * 1024U * 1024U;

enum class ParseResult { complete, incomplete, error };

// Incremental parser for RESP2 command arrays containing bulk strings.
class RequestParser {
public:
    [[nodiscard]] bool feed(std::string_view bytes, std::string& error);
    [[nodiscard]] ParseResult next(std::vector<std::string>& arguments, std::string& error);

private:
    std::string buffer_;
    std::size_t offset_ = 0;
    bool failed_ = false;
};

[[nodiscard]] std::string encode_request(const std::vector<std::string>& arguments);
[[nodiscard]] std::string simple(std::string_view message);
[[nodiscard]] std::string error(std::string_view message);
[[nodiscard]] std::string bulk(std::string_view value);
[[nodiscard]] std::string null_bulk();
[[nodiscard]] std::string integer(long long value);

} // namespace tinykv::protocol

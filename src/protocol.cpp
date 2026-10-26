#include "tinykv/protocol.hpp"

#include <charconv>
#include <limits>
#include <stdexcept>

namespace tinykv::protocol {
namespace {

bool parse_unsigned(std::string_view text, std::size_t& value) {
    if (text.empty()) {
        return false;
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

} // namespace

bool RequestParser::feed(std::string_view bytes, std::string& error_message) {
    if (failed_) {
        error_message = "parser is in a failed state";
        return false;
    }
    const auto buffered = buffer_.size() - offset_;
    if (bytes.size() > kMaxRequestBytes - buffered) {
        failed_ = true;
        error_message = "request exceeds the 32 MiB limit";
        return false;
    }
    if (offset_ != 0 && (offset_ >= 64U * 1024U || offset_ == buffer_.size())) {
        buffer_.erase(0, offset_);
        offset_ = 0;
    }
    if (!bytes.empty()) {
        buffer_.append(bytes.data(), bytes.size());
    }
    return true;
}

ParseResult RequestParser::next(std::vector<std::string>& arguments, std::string& error_message) {
    if (failed_) {
        error_message = "parser is in a failed state";
        return ParseResult::error;
    }
    const std::string_view input(buffer_);
    std::size_t cursor = offset_;
    auto fail = [&](const char* message) {
        failed_ = true;
        error_message = message;
        return ParseResult::error;
    };
    auto line = [&](std::size_t& position, std::string_view& value) -> ParseResult {
        const auto end = input.find("\r\n", position);
        if (end == std::string_view::npos) {
            if (input.size() - position > 32) {
                return fail("RESP length line is too long");
            }
            return ParseResult::incomplete;
        }
        if (end - position > 32) {
            return fail("RESP length line is too long");
        }
        value = input.substr(position, end - position);
        position = end + 2;
        return ParseResult::complete;
    };

    if (cursor == input.size()) {
        return ParseResult::incomplete;
    }
    if (input[cursor] != '*') {
        return fail("request must start with a RESP array");
    }
    ++cursor;
    std::string_view array_line;
    auto result = line(cursor, array_line);
    if (result != ParseResult::complete) {
        return result;
    }
    std::size_t count = 0;
    if (!parse_unsigned(array_line, count) || count > kMaxArguments) {
        return fail("invalid RESP array length");
    }

    std::vector<std::string> parsed;
    parsed.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (cursor == input.size()) {
            return ParseResult::incomplete;
        }
        if (input[cursor] != '$') {
            return fail("request arguments must be RESP bulk strings");
        }
        ++cursor;
        std::string_view length_line;
        result = line(cursor, length_line);
        if (result != ParseResult::complete) {
            return result;
        }
        std::size_t length = 0;
        if (!parse_unsigned(length_line, length) || length > kMaxBulkBytes) {
            return fail("invalid RESP bulk length (maximum 16 MiB)");
        }
        const auto remaining = input.size() - cursor;
        if (length > remaining || remaining - length < 2) {
            return ParseResult::incomplete;
        }
        if (input[cursor + length] != '\r' || input[cursor + length + 1] != '\n') {
            return fail("RESP bulk string is missing its terminator");
        }
        parsed.emplace_back(input.data() + cursor, length);
        cursor += length + 2;
    }

    arguments = std::move(parsed);
    offset_ = cursor;
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    } else if (offset_ >= 64U * 1024U) {
        buffer_.erase(0, offset_);
        offset_ = 0;
    }
    return ParseResult::complete;
}

std::string encode_request(const std::vector<std::string>& arguments) {
    if (arguments.size() > kMaxArguments) {
        throw std::length_error("too many request arguments");
    }
    std::string output = "*" + std::to_string(arguments.size()) + "\r\n";
    for (const auto& argument : arguments) {
        if (argument.size() > kMaxBulkBytes) {
            throw std::length_error("request argument exceeds the 16 MiB limit");
        }
        output += "$" + std::to_string(argument.size()) + "\r\n";
        output.append(argument);
        output += "\r\n";
    }
    if (output.size() > kMaxRequestBytes) {
        throw std::length_error("request exceeds the 32 MiB limit");
    }
    return output;
}

std::string simple(std::string_view message) {
    return "+" + std::string(message) + "\r\n";
}

std::string error(std::string_view message) {
    return "-ERR " + std::string(message) + "\r\n";
}

std::string bulk(std::string_view value) {
    return "$" + std::to_string(value.size()) + "\r\n" + std::string(value) + "\r\n";
}

std::string null_bulk() { return "$-1\r\n"; }

std::string integer(long long value) { return ":" + std::to_string(value) + "\r\n"; }

} // namespace tinykv::protocol

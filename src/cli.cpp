#include "tinykv/protocol.hpp"
#include "socket.hpp"

#include <charconv>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif

namespace {

tinykv::net::Socket connect_to(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const int result = getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses);
    if (result != 0) {
#ifdef _WIN32
        throw std::runtime_error("address lookup failed with code " + std::to_string(result));
#else
        throw std::runtime_error(std::string("address lookup failed: ") + gai_strerror(result));
#endif
    }

    tinykv::net::Socket connection = tinykv::net::kInvalidSocket;
    for (auto* current = addresses; current != nullptr; current = current->ai_next) {
        connection = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (connection == tinykv::net::kInvalidSocket) {
            continue;
        }
        if (::connect(connection, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0) {
            break;
        }
        tinykv::net::close(connection);
        connection = tinykv::net::kInvalidSocket;
    }
    freeaddrinfo(addresses);
    if (connection == tinykv::net::kInvalidSocket) {
        throw std::runtime_error("unable to connect: " + tinykv::net::last_error());
    }
    return connection;
}

class ResponseReader {
public:
    explicit ResponseReader(tinykv::net::Socket socket) : socket_(socket) {}

    std::string line() {
        for (;;) {
            const auto end = buffer_.find("\r\n", offset_);
            if (end != std::string::npos) {
                std::string result(buffer_.data() + offset_, end - offset_);
                offset_ = end + 2;
                compact();
                return result;
            }
            receive_more();
        }
    }

    std::string exact(std::size_t size) {
        while (buffer_.size() - offset_ < size) {
            receive_more();
        }
        std::string result(buffer_.data() + offset_, size);
        offset_ += size;
        compact();
        return result;
    }

private:
    void receive_more() {
        compact();
        char block[4096];
        const int count = tinykv::net::receive(socket_, block, sizeof(block));
        if (count <= 0) {
            throw std::runtime_error(count == 0 ? "server closed the connection" :
                                     "socket read failed: " + tinykv::net::last_error());
        }
        buffer_.append(block, static_cast<std::size_t>(count));
    }

    void compact() {
        if (offset_ != 0 && (offset_ >= 4096 || offset_ == buffer_.size())) {
            buffer_.erase(0, offset_);
            offset_ = 0;
        }
    }

    tinykv::net::Socket socket_;
    std::string buffer_;
    std::size_t offset_ = 0;
};

void usage() {
    std::cout << "Usage: tinykv-cli [--host ADDRESS] [--port PORT] COMMAND [ARG ...]\n"
                 "Commands: PING, SET key value, GET key, DEL key, STATS, QUIT\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::string host = "127.0.0.1";
        std::string port = "6380";
        int index = 1;
        while (index < argc) {
            const std::string option = argv[index];
            if (option == "--help" || option == "-h") {
                usage();
                return 0;
            }
            if (option == "--host" || option == "--port") {
                if (index + 1 >= argc) {
                    throw std::invalid_argument("missing value for " + option);
                }
                if (option == "--host") {
                    host = argv[index + 1];
                } else {
                    port = argv[index + 1];
                    unsigned int parsed = 0;
                    const auto result = std::from_chars(port.data(), port.data() + port.size(), parsed);
                    if (result.ec != std::errc{} || result.ptr != port.data() + port.size() ||
                        parsed == 0 || parsed > 65535) {
                        throw std::invalid_argument("invalid port: " + port);
                    }
                }
                index += 2;
                continue;
            }
            break;
        }
        if (index >= argc) {
            usage();
            return 2;
        }

        std::vector<std::string> arguments;
        for (; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }
        const auto request = tinykv::protocol::encode_request(arguments);
        tinykv::net::Runtime runtime;
        const auto connection = connect_to(host, port);
        const bool sent = tinykv::net::send_all(connection, request);
        if (!sent) {
            tinykv::net::close(connection);
            throw std::runtime_error("socket write failed: " + tinykv::net::last_error());
        }

        ResponseReader reader(connection);
        const auto response = reader.line();
        if (response.empty()) {
            throw std::runtime_error("empty server response");
        }
        if (response[0] == '+') {
            std::cout << response.substr(1) << '\n';
        } else if (response[0] == '-') {
            std::cerr << response.substr(1) << '\n';
            tinykv::net::close(connection);
            return 1;
        } else if (response[0] == ':') {
            std::cout << response.substr(1) << '\n';
        } else if (response[0] == '$') {
            long long length = -2;
            const auto text = std::string_view(response).substr(1);
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), length);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || length < -1 ||
                length > static_cast<long long>(tinykv::protocol::kMaxBulkBytes)) {
                throw std::runtime_error("invalid bulk response length");
            }
            if (length == -1) {
                std::cout << "(nil)\n";
            } else {
                const auto value = reader.exact(static_cast<std::size_t>(length));
                if (reader.exact(2) != "\r\n") {
                    throw std::runtime_error("invalid bulk response terminator");
                }
                std::cout.write(value.data(), static_cast<std::streamsize>(value.size()));
                std::cout << '\n';
            }
        } else {
            throw std::runtime_error("unsupported server response");
        }
        tinykv::net::close(connection);
    } catch (const std::exception& exception) {
        std::cerr << "tinykv-cli: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}

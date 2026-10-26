#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace tinykv::net {

#ifdef _WIN32
using Socket = SOCKET;
inline constexpr Socket kInvalidSocket = INVALID_SOCKET;

class Runtime {
public:
    Runtime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("Winsock initialization failed");
        }
    }
    ~Runtime() { WSACleanup(); }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
};

inline void close(Socket socket) noexcept {
    if (socket != kInvalidSocket) {
        closesocket(socket);
    }
}

inline bool interrupted() noexcept { return WSAGetLastError() == WSAEINTR; }
inline bool timed_out() noexcept {
    const int code = WSAGetLastError();
    return code == WSAETIMEDOUT || code == WSAEWOULDBLOCK;
}
inline std::string last_error() { return "Winsock error " + std::to_string(WSAGetLastError()); }

inline void set_timeout(Socket socket, int milliseconds) {
    const DWORD timeout = static_cast<DWORD>(milliseconds);
    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR ||
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR) {
        throw std::runtime_error("unable to configure socket timeout: " + last_error());
    }
}

inline int receive(Socket socket, char* buffer, std::size_t capacity) {
    const auto count = static_cast<int>(std::min<std::size_t>(capacity, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    return recv(socket, buffer, count, 0);
}

inline int send(Socket socket, const char* buffer, std::size_t length) {
    const auto count = static_cast<int>(std::min<std::size_t>(length, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    return ::send(socket, buffer, count, 0);
}

#else
using Socket = int;
inline constexpr Socket kInvalidSocket = -1;

class Runtime {
public:
    Runtime() = default;
};

inline void close(Socket socket) noexcept {
    if (socket != kInvalidSocket) {
        ::close(socket);
    }
}

inline bool interrupted() noexcept { return errno == EINTR; }
inline bool timed_out() noexcept { return errno == EAGAIN || errno == EWOULDBLOCK; }
inline std::string last_error() { return std::string(std::strerror(errno)); }

inline void set_timeout(Socket socket, int milliseconds) {
    const timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
#ifdef SO_NOSIGPIPE
    const int no_sigpipe = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0) {
        throw std::runtime_error("unable to configure SIGPIPE handling: " + last_error());
    }
#endif
    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        throw std::runtime_error("unable to configure socket timeout: " + last_error());
    }
}

inline int receive(Socket socket, char* buffer, std::size_t capacity) {
    const auto count = ::recv(socket, buffer, capacity, 0);
    return count > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(count);
}

inline int send(Socket socket, const char* buffer, std::size_t length) {
    const auto count = std::min<std::size_t>(length, static_cast<std::size_t>(std::numeric_limits<int>::max()));
#ifdef MSG_NOSIGNAL
    const auto result = ::send(socket, buffer, count, MSG_NOSIGNAL);
#else
    const auto result = ::send(socket, buffer, count, 0);
#endif
    return result > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(result);
}
#endif

inline bool send_all(Socket socket, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = send(socket, data.data() + offset, data.size() - offset);
        if (count <= 0) {
            if (count < 0 && interrupted()) {
                continue;
            }
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

} // namespace tinykv::net

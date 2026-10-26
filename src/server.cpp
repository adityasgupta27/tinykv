#include "tinykv/server.hpp"

#include "tinykv/protocol.hpp"
#include "socket.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <csignal>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/select.h>
#endif

namespace tinykv {
namespace {

std::atomic_bool stop_requested{false};
constexpr std::size_t kMaxQueuedConnections = 256;
constexpr int kClientTimeoutMs = 1000;

bool stopping() noexcept { return stop_requested.load(std::memory_order_relaxed); }

class ConnectionQueue {
public:
    bool push(net::Socket socket) {
        std::lock_guard lock(mutex_);
        if (closed_ || sockets_.size() >= kMaxQueuedConnections) {
            return false;
        }
        sockets_.push_back(socket);
        available_.notify_one();
        return true;
    }

    bool pop(net::Socket& socket) {
        std::unique_lock lock(mutex_);
        available_.wait(lock, [this] { return closed_ || !sockets_.empty(); });
        if (sockets_.empty()) {
            return false;
        }
        socket = sockets_.front();
        sockets_.pop_front();
        return true;
    }

    void close() noexcept {
        std::lock_guard lock(mutex_);
        closed_ = true;
        while (!sockets_.empty()) {
            net::close(sockets_.front());
            sockets_.pop_front();
        }
        available_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<net::Socket> sockets_;
    bool closed_ = false;
};

std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::string execute(Database& database, const std::vector<std::string>& arguments, bool& close_after) {
    using namespace protocol;
    if (arguments.empty()) {
        return error("empty command");
    }
    const auto command = uppercase(arguments[0]);
    if (command == "PING") {
        if (arguments.size() == 1) {
            return simple("PONG");
        }
        if (arguments.size() == 2) {
            return bulk(arguments[1]);
        }
        return error("wrong number of arguments for PING");
    }
    if (command == "SET") {
        if (arguments.size() != 3) {
            return error("wrong number of arguments for SET");
        }
        database.set(arguments[1], arguments[2]);
        return simple("OK");
    }
    if (command == "GET") {
        if (arguments.size() != 2) {
            return error("wrong number of arguments for GET");
        }
        const auto value = database.get(arguments[1]);
        return value ? bulk(*value) : null_bulk();
    }
    if (command == "DEL") {
        if (arguments.size() != 2) {
            return error("wrong number of arguments for DEL");
        }
        return integer(database.erase(arguments[1]) ? 1 : 0);
    }
    if (command == "STATS") {
        if (arguments.size() != 1) {
            return error("wrong number of arguments for STATS");
        }
        const auto stats = database.stats();
        const auto text = "keys=" + std::to_string(stats.keys) +
                          " reads=" + std::to_string(stats.reads) +
                          " writes=" + std::to_string(stats.writes) +
                          " deletes=" + std::to_string(stats.deletes);
        return bulk(text);
    }
    if (command == "QUIT") {
        if (arguments.size() != 1) {
            return error("wrong number of arguments for QUIT");
        }
        close_after = true;
        return simple("BYE");
    }
    return error("unknown command");
}

void handle_client(Database& database, net::Socket client) {
    try {
        net::set_timeout(client, kClientTimeoutMs);
    } catch (const std::exception& exception) {
        std::cerr << "client socket setup failed: " << exception.what() << '\n';
        return;
    }

    protocol::RequestParser parser;
    std::array<char, 8192> buffer{};
    while (!stopping()) {
        const int received = net::receive(client, buffer.data(), buffer.size());
        if (received == 0) {
            break;
        }
        if (received < 0) {
            if (net::interrupted()) {
                continue;
            }
            break;
        }

        std::string parser_error;
        if (!parser.feed(std::string_view(buffer.data(), static_cast<std::size_t>(received)), parser_error)) {
            (void)net::send_all(client, protocol::error(parser_error));
            break;
        }

        for (;;) {
            std::vector<std::string> arguments;
            const auto result = parser.next(arguments, parser_error);
            if (result == protocol::ParseResult::incomplete) {
                break;
            }
            if (result == protocol::ParseResult::error) {
                (void)net::send_all(client, protocol::error(parser_error));
                return;
            }

            bool close_after = false;
            std::string response;
            try {
                response = execute(database, arguments, close_after);
            } catch (const std::exception& exception) {
                std::cerr << "request failed: " << exception.what() << '\n';
                response = protocol::error("storage operation failed");
            }
            if (!net::send_all(client, response) || close_after) {
                return;
            }
        }
    }
}

net::Socket create_listener(const std::string& address, unsigned short port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    const std::string service = std::to_string(port);
    const char* node = (address == "*" || address.empty()) ? nullptr : address.c_str();

    addrinfo* addresses = nullptr;
    const int result = getaddrinfo(node, service.c_str(), &hints, &addresses);
    if (result != 0) {
#ifdef _WIN32
        throw std::runtime_error("address lookup failed with code " + std::to_string(result));
#else
        throw std::runtime_error(std::string("address lookup failed: ") + gai_strerror(result));
#endif
    }

    net::Socket listener = net::kInvalidSocket;
    for (auto* current = addresses; current != nullptr; current = current->ai_next) {
        listener = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (listener == net::kInvalidSocket) {
            continue;
        }
        const int enabled = 1;
#ifdef _WIN32
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
#else
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#endif
        if (::bind(listener, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0 &&
            ::listen(listener, 128) == 0) {
            break;
        }
        net::close(listener);
        listener = net::kInvalidSocket;
    }
    freeaddrinfo(addresses);
    if (listener == net::kInvalidSocket) {
        throw std::runtime_error("unable to bind TCP listener: " + net::last_error());
    }
    return listener;
}

} // namespace

TcpServer::TcpServer(Database& database, std::string address, unsigned short port,
                     std::size_t worker_count)
    : database_(database), address_(std::move(address)), port_(port), worker_count_(worker_count) {
    if (worker_count_ == 0) {
        throw std::invalid_argument("worker count must be at least one");
    }
}

void TcpServer::request_stop() noexcept { stop_requested.store(true, std::memory_order_relaxed); }

void TcpServer::run() {
    stop_requested.store(false, std::memory_order_relaxed);
    net::Runtime runtime;
    const auto listener = create_listener(address_, port_);
    ConnectionQueue queue;
    std::vector<std::thread> workers;
    try {
        workers.reserve(worker_count_);
        for (std::size_t index = 0; index < worker_count_; ++index) {
            workers.emplace_back([this, &queue] {
                net::Socket client = net::kInvalidSocket;
                while (queue.pop(client)) {
                    handle_client(database_, client);
                    net::close(client);
                }
            });
        }

        std::cout << "TinyKV listening on " << address_ << ':' << port_
                  << " with " << worker_count_ << " workers" << std::endl;
        while (!stopping()) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(listener, &read_set);
            timeval timeout{0, 250000};
#ifdef _WIN32
            const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
#else
            const int ready = select(listener + 1, &read_set, nullptr, nullptr, &timeout);
#endif
            if (ready == 0) {
                continue;
            }
            if (ready < 0) {
                if (net::interrupted()) {
                    continue;
                }
                throw std::runtime_error("listener wait failed: " + net::last_error());
            }

            const auto client = ::accept(listener, nullptr, nullptr);
            if (client == net::kInvalidSocket) {
                if (net::interrupted() || net::timed_out()) {
                    continue;
                }
                throw std::runtime_error("accept failed: " + net::last_error());
            }
            if (!queue.push(client)) {
                (void)net::send_all(client, protocol::error("server is busy"));
                net::close(client);
            }
        }
    } catch (...) {
        net::close(listener);
        queue.close();
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }

    net::close(listener);
    queue.close();
    for (auto& worker : workers) {
        worker.join();
    }
}

} // namespace tinykv

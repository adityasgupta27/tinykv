#include "tinykv/database.hpp"
#include "tinykv/server.hpp"

#include <algorithm>
#include <csignal>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <thread>

namespace {

void handle_signal(int) { tinykv::TcpServer::request_stop(); }

unsigned long parse_number(const std::string& text, const std::string& option,
                           unsigned long maximum) {
    std::size_t consumed = 0;
    const auto value = std::stoul(text, &consumed);
    if (consumed != text.size() || value > maximum) {
        throw std::invalid_argument("invalid value for " + option + ": " + text);
    }
    return value;
}

void usage() {
    std::cout << "Usage: tinykv-server [--host ADDRESS] [--port PORT] [--workers N] [--wal PATH] [--no-sync]\n"
                 "Defaults: host=127.0.0.1 port=6380 workers=min(hardware threads, 32)\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::string address = "127.0.0.1";
        unsigned short port = 6380;
        const auto hardware_threads = std::max(1U, std::thread::hardware_concurrency());
        std::size_t workers = std::min<std::size_t>(hardware_threads, 32);
        std::optional<std::filesystem::path> wal_path;
        bool durable = true;

        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--help" || option == "-h") {
                usage();
                return 0;
            }
            if (option == "--no-sync") {
                durable = false;
                continue;
            }
            if (index + 1 >= argc) {
                throw std::invalid_argument("missing value for " + option);
            }
            const std::string value = argv[++index];
            if (option == "--host") {
                address = value;
            } else if (option == "--port") {
                const auto parsed_port = parse_number(value, option, 65535);
                if (parsed_port == 0) {
                    throw std::invalid_argument("--port must be between 1 and 65535");
                }
                port = static_cast<unsigned short>(parsed_port);
            } else if (option == "--workers") {
                workers = parse_number(value, option, 256);
                if (workers == 0) {
                    throw std::invalid_argument("--workers must be at least one");
                }
            } else if (option == "--wal") {
                wal_path = std::filesystem::path(value);
            } else {
                throw std::invalid_argument("unknown option: " + option);
            }
        }

        std::signal(SIGINT, handle_signal);
#ifdef SIGTERM
        std::signal(SIGTERM, handle_signal);
#endif
        tinykv::Database database(wal_path, durable);
        tinykv::TcpServer server(database, address, port, workers);
        std::cout << (wal_path ? "WAL: " + wal_path->string() + (durable ? " (sync per write)" : " (sync disabled)")
                               : "WAL: disabled (in-memory only)")
                  << std::endl;
        server.run();
    } catch (const std::exception& exception) {
        std::cerr << "tinykv-server: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}

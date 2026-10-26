#pragma once

#include "tinykv/database.hpp"

#include <cstddef>
#include <string>

namespace tinykv {

class TcpServer {
public:
    TcpServer(Database& database, std::string address, unsigned short port,
              std::size_t worker_count);
    void run();

    // Safe to call from the process signal handler.
    static void request_stop() noexcept;

private:
    Database& database_;
    std::string address_;
    unsigned short port_;
    std::size_t worker_count_;
};

} // namespace tinykv

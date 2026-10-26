#include "tinykv/database.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::size_t threads = std::min<std::size_t>(std::max(1U, std::thread::hardware_concurrency()), 32);
    std::size_t operations_per_thread = 200000;
    std::size_t key_space = 10000;
    std::optional<std::filesystem::path> wal;
    bool durable = true;
};

std::size_t number(const std::string& text, const std::string& name) {
    std::size_t used = 0;
    const auto value = std::stoull(text, &used);
    if (used != text.size() || value == 0) {
        throw std::invalid_argument("invalid " + name + ": " + text);
    }
    return static_cast<std::size_t>(value);
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string name = argv[index];
        if (name == "--no-sync") {
            options.durable = false;
            continue;
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + name);
        }
        const std::string value = argv[++index];
        if (name == "--threads") {
            options.threads = number(value, name);
        } else if (name == "--operations") {
            options.operations_per_thread = number(value, name);
        } else if (name == "--keys") {
            options.key_space = number(value, name);
        } else if (name == "--wal") {
            options.wal = std::filesystem::path(value);
        } else {
            throw std::invalid_argument("unknown option: " + name);
        }
    }
    if (options.threads > 256) {
        throw std::invalid_argument("--threads cannot exceed 256");
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        tinykv::Database database(options.wal, options.durable);
        for (std::size_t index = 0; index < options.key_space; ++index) {
            database.set("key-" + std::to_string(index), "value-" + std::to_string(index));
        }

        std::atomic<std::size_t> ready{0};
        std::atomic<bool> start{false};
        std::vector<std::thread> workers;
        workers.reserve(options.threads);
        for (std::size_t thread_id = 0; thread_id < options.threads; ++thread_id) {
            workers.emplace_back([&, thread_id] {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (std::size_t index = 0; index < options.operations_per_thread; ++index) {
                    const auto key_index = (index * 1315423911ULL + thread_id * 97ULL) % options.key_space;
                    const auto key = "key-" + std::to_string(key_index);
                    if (index % 10 == 0) {
                        database.set(key, "value-" + std::to_string(index + thread_id));
                    } else {
                        (void)database.get(key);
                    }
                }
            });
        }
        while (ready.load(std::memory_order_acquire) != options.threads) {
            std::this_thread::yield();
        }
        const auto began = std::chrono::steady_clock::now();
        start.store(true, std::memory_order_release);
        for (auto& worker : workers) {
            worker.join();
        }
        const auto ended = std::chrono::steady_clock::now();
        const auto seconds = std::chrono::duration<double>(ended - began).count();
        const auto total = options.threads * options.operations_per_thread;

        std::cout << "TinyKV benchmark (90% reads, 10% writes)\n"
                  << "threads=" << options.threads << " operations_per_thread=" << options.operations_per_thread
                  << " key_space=" << options.key_space << '\n'
                  << "persistence=" << (options.wal ? (options.durable ? "WAL+sync" : "WAL, no sync") : "memory only") << '\n'
                  << "operations=" << total << " elapsed_seconds=" << seconds
                  << " operations_per_second=" << static_cast<double>(total) / seconds << '\n';
    } catch (const std::exception& exception) {
        std::cerr << "tinykv-bench: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}

#include "memory_pool/fixed_block_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::size_t option(int argc, char** argv, const std::string& name, std::size_t fallback) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] == name) {
            return static_cast<std::size_t>(std::strtoull(argv[index + 1], nullptr, 10));
        }
    }
    return fallback;
}

}  // namespace

int main(int argc, char** argv) {
    const auto threads = option(argc, argv, "--threads", std::max(1u, std::thread::hardware_concurrency()));
    const auto iterations = option(argc, argv, "--iterations", 100000);
    memory_pool::FixedBlockPool pool(64, threads * 256);
    std::atomic<std::size_t> allocations{0};
    std::atomic<std::size_t> failures{0};
    std::vector<std::thread> workers;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t thread = 0; thread < threads; ++thread) {
        workers.emplace_back([&] {
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                void* block = pool.allocate();
                if (block == nullptr) {
                    ++failures;
                    continue;
                }
                ++allocations;
                static_cast<unsigned char*>(block)[0] = 0xA5;
                pool.deallocate(block);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::cout << "threads=" << threads << " iterations=" << iterations << " allocations=" << allocations.load()
              << " failures=" << failures.load() << " elapsed_ms=" << elapsed << "\n";
    return failures.load() == 0 ? 0 : 2;
}

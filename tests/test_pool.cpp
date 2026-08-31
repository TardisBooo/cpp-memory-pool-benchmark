#include "memory_pool/memory_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_local_and_invalid_release() {
    memory_pool::PoolConfig config;
    config.preallocate_pages_per_class = 0;
    memory_pool::MemoryPool pool(config);

    void* pointer = pool.allocate(24);
    check(pointer != nullptr, "small allocation should succeed");
    check(reinterpret_cast<std::uintptr_t>(pointer) % alignof(std::max_align_t) == 0,
          "small allocation should satisfy the default alignment");
    check(pool.owns(pointer), "pool should recognize an owned small pointer");
    static_cast<std::byte*>(pointer)[0] = std::byte{ 0x5a };
    check(pool.deallocate(pointer), "first release should succeed");
    check(!pool.deallocate(pointer), "repeated release should be rejected");
    check(pool.owns(pointer), "pool should retain address ownership after caching");

    void* zero_byte_pointer = pool.allocate(0);
    check(zero_byte_pointer != nullptr, "zero-byte allocation should return a usable block");
    check(pool.deallocate(zero_byte_pointer), "zero-byte block should be releasable");

    int stack_value = 0;
    check(!pool.deallocate(&stack_value), "foreign pointer should be rejected");
    check(!pool.owns(&stack_value), "foreign pointer should not be owned");

    const auto stats = pool.snapshot();
    check(stats.successful_allocations >= 2, "successful allocation counter should advance");
    check(stats.successful_deallocations >= 2, "successful deallocation counter should advance");
    check(stats.double_deallocations >= 1, "repeated small release should be counted");
    check(stats.invalid_deallocations >= 1, "foreign release should be counted");
    check(stats.live_allocations == 0, "all test allocations should be released");
}

void test_alignment_and_large_cache() {
    memory_pool::PoolConfig config;
    config.preallocate_pages_per_class = 0;
    config.large_cache_high_watermark = 4 * 64u * 1024u;
    config.large_cache_low_watermark = 64u * 1024u;
    memory_pool::MemoryPool pool(config);

    void* aligned_small = pool.allocate(64, 64);
    check(aligned_small != nullptr, "over-aligned small request should succeed");
    check(reinterpret_cast<std::uintptr_t>(aligned_small) % 64 == 0,
          "over-aligned request should be aligned");
    check(pool.deallocate(aligned_small), "over-aligned block should be releasable");

    void* large = pool.allocate(64u * 1024u + 1u, 4096);
    check(large != nullptr, "large allocation should succeed");
    check(reinterpret_cast<std::uintptr_t>(large) % 4096 == 0,
          "large allocation should satisfy its alignment");
    check(pool.owns(large), "pool should recognize an owned large pointer");
    static_cast<std::byte*>(large)[0] = std::byte{ 0xa5 };
    check(pool.deallocate(large), "large block should be releasable");

    void* reused = pool.allocate(64u * 1024u + 1u, 4096);
    check(reused != nullptr, "large cache reuse should succeed");
    check(pool.deallocate(reused), "reused large block should be releasable");

    const auto stats = pool.snapshot();
    check(stats.large_cache_hits >= 1, "large cache hit should be recorded");
    check(stats.large_cache_misses >= 1, "large cache miss should be recorded");
    check(stats.live_allocations == 0, "large cache test should not leak live allocations");
}

void test_remote_release_and_trim() {
    memory_pool::PoolConfig config;
    config.preallocate_pages_per_class = 0;
    config.resident_page_floor = 1;
    memory_pool::MemoryPool pool(config);

    void* remote = pool.allocate(128);
    check(remote != nullptr, "remote-release allocation should succeed");
    std::atomic<bool> released{ false };
    std::thread releaser([&] {
        released.store(pool.deallocate(remote), std::memory_order_release);
    });
    releaser.join();
    check(released.load(std::memory_order_acquire), "remote release should succeed");

    std::vector<void*> blocks;
    blocks.reserve(5000);
    for (std::size_t index = 0; index < 5000; ++index) {
        void* pointer = pool.allocate(16);
        check(pointer != nullptr, "page growth allocation should succeed");
        blocks.push_back(pointer);
    }
    for (void* pointer : blocks) {
        check(pool.deallocate(pointer), "page growth block should be releasable");
    }

    const auto before_trim = pool.snapshot();
    check(before_trim.resident_pages >= 2, "test should create more than one page");
    pool.trim();
    const auto after_trim = pool.snapshot();
    check(after_trim.pages_reclaimed >= 1, "trim should reclaim an empty page");
    check(after_trim.resident_pages >= 1, "trim should retain the resident page floor");
    check(after_trim.remote_free_releases >= 1, "remote release should be counted");
    check(after_trim.live_allocations == 0, "trim test should not leak live allocations");
}

void test_concurrent_small_allocations() {
    memory_pool::PoolConfig config;
    config.preallocate_pages_per_class = 0;
    memory_pool::MemoryPool pool(config);

    constexpr std::size_t thread_count = 4;
    constexpr std::size_t iterations = 2000;
    std::atomic<std::size_t> failures{ 0 };
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (std::size_t thread = 0; thread < thread_count; ++thread) {
        workers.emplace_back([&] {
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                void* pointer = pool.allocate(64 + (iteration % 4) * 16);
                if (pointer == nullptr) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                static_cast<std::byte*>(pointer)[0] = std::byte{ 0x11 };
                if (!pool.deallocate(pointer)) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    check(failures.load(std::memory_order_relaxed) == 0,
          "concurrent small allocation workload should not fail");
    const auto stats = pool.snapshot();
    check(stats.successful_allocations == thread_count * iterations,
          "concurrent allocation count should match");
    check(stats.successful_deallocations == thread_count * iterations,
          "concurrent deallocation count should match");
    check(stats.live_allocations == 0, "concurrent workload should not leak");
}

}  // namespace

int main() {
    try {
        test_local_and_invalid_release();
        test_alignment_and_large_cache();
        test_remote_release_and_trim();
        test_concurrent_small_allocations();
        std::cout << "memory pool tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "memory pool test failure: " << error.what() << '\n';
        return 1;
    }
}

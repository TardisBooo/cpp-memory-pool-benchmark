#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <iterator>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace memory_pool {

struct PoolConfig {
    std::size_t page_bytes = 64u * 1024u;
    std::size_t segment_pages = 16;
    std::size_t small_max_bytes = 1024;
    std::size_t large_threshold_bytes = 32u * 1024u;
    std::size_t preallocate_pages_per_class = 1;
    double page_reclaim_ratio = 0.75;
    std::size_t resident_page_floor = 1;
    std::size_t large_cache_high_watermark = 4u * 1024u * 1024u;
    std::size_t large_cache_low_watermark = 1u * 1024u * 1024u;
    bool debug_checks = false;
};

struct PoolStats {
    std::uint64_t allocation_attempts = 0;
    std::uint64_t successful_allocations = 0;
    std::uint64_t failed_allocations = 0;
    std::uint64_t successful_deallocations = 0;
    std::uint64_t invalid_deallocations = 0;
    std::uint64_t double_deallocations = 0;

    std::uint64_t requested_bytes = 0;
    std::uint64_t live_requested_bytes = 0;
    std::uint64_t peak_live_requested_bytes = 0;
    std::uint64_t live_allocations = 0;
    std::uint64_t live_size_class_waste_bytes = 0;
    std::uint64_t reserved_bytes = 0;
    std::uint64_t peak_reserved_bytes = 0;
    std::uint64_t committed_bytes = 0;

    std::uint64_t pages_created = 0;
    std::uint64_t pages_reclaimed = 0;
    std::uint64_t resident_pages = 0;
    std::uint64_t segments_created = 0;
    std::uint64_t segments_reclaimed = 0;
    std::uint64_t resident_segments = 0;
    std::uint64_t os_allocations = 0;
    std::uint64_t os_releases = 0;

    std::uint64_t local_cache_hits = 0;
    std::uint64_t local_cache_releases = 0;
    std::uint64_t remote_free_releases = 0;
    std::uint64_t large_cache_hits = 0;
    std::uint64_t large_cache_misses = 0;
    std::uint64_t large_cache_bytes = 0;

    double size_class_waste_ratio = 0.0;
};

class MemoryPool {
public:
    explicit MemoryPool(PoolConfig config = {})
        : config_(config), lifetime_(std::make_shared<Lifetime>()) {
        validate_config(config_);
        size_classes_ = make_size_classes(config_);
        directories_.reserve(size_classes_.size());
        for (const auto size : size_classes_) {
            auto directory = std::make_unique<SizeClassDirectory>();
            directory->block_bytes = size;
            directories_.push_back(std::move(directory));
        }

        for (std::size_t index = 0; index < directories_.size(); ++index) {
            for (std::size_t page = 0; page < config_.preallocate_pages_per_class; ++page) {
                std::lock_guard<std::mutex> lock(directories_[index]->mutex);
                (void)create_page_locked(index);
            }
        }
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    ~MemoryPool() {
        // A pool must not be destroyed while another thread is using it. The
        // local flush keeps the current thread's cached blocks out of pages
        // before those pages are released below.
        flush_thread_cache();
        lifetime_.reset();
        release_large_runs();
        // Page and Segment destructors release their OS mappings. They are
        // deliberately metadata-owning objects, so no pointer is dereferenced
        // during destruction.
    }

    void* allocate(std::size_t bytes,
                   std::size_t alignment = alignof(std::max_align_t)) noexcept {
        counters_.allocation_attempts.fetch_add(1, std::memory_order_relaxed);

        std::size_t normalized_alignment = 0;
        std::size_t normalized_bytes = 0;
        if (!normalize_request(bytes, alignment, normalized_bytes, normalized_alignment)) {
            counters_.failed_allocations.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        const auto logical_bytes = bytes == 0 ? std::size_t{ 1 } : bytes;

        try {
            if (normalized_bytes > config_.large_threshold_bytes ||
                choose_size_class(normalized_bytes, normalized_alignment) == npos) {
                if (void* pointer = allocate_large(normalized_bytes,
                                                    logical_bytes,
                                                    normalized_alignment)) {
                    record_successful_allocation(logical_bytes, logical_bytes, false);
                    return pointer;
                }
            } else {
                const auto class_index = choose_size_class(normalized_bytes, normalized_alignment);
                if (void* pointer = allocate_small(class_index, logical_bytes)) {
                    record_successful_allocation(logical_bytes,
                                                  size_classes_[class_index],
                                                  true);
                    return pointer;
                }
            }
        } catch (...) {
            // The public allocation contract is noexcept. Allocation failure
            // is reported as nullptr and counted rather than escaping from a
            // worker thread in a benchmark.
        }

        counters_.failed_allocations.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    bool deallocate(void* pointer) noexcept {
        if (pointer == nullptr) {
            counters_.invalid_deallocations.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        try {
            if (SmallLookup lookup = locate_small(pointer); lookup.page != nullptr) {
                return deallocate_small(lookup.page, lookup.index);
            }

            std::lock_guard<std::mutex> lock(large_mutex_);
            for (auto& holder : large_runs_) {
                LargeRun* run = holder.get();
                if (run->active && run->user == pointer) {
                    return deallocate_large_locked(run);
                }
                if (!run->active && !run->released && run->user == pointer) {
                    counters_.double_deallocations.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
            }
        } catch (...) {
            // Invalid memory ownership must never turn into an exception from
            // a cleanup path.
        }

        counters_.invalid_deallocations.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    [[nodiscard]] bool owns(const void* pointer) const noexcept {
        if (pointer == nullptr) {
            return false;
        }

        try {
            {
                std::shared_lock<std::shared_mutex> lock(registry_mutex_);
                for (const Page* page : pages_) {
                    if (page->contains(pointer)) {
                        return true;
                    }
                }
            }

            std::lock_guard<std::mutex> lock(large_mutex_);
            for (const auto& holder : large_runs_) {
                const LargeRun* run = holder.get();
                if (run->active && run->user == pointer) {
                    return true;
                }
            }
        } catch (...) {
            return false;
        }
        return false;
    }

    // Reclamation is intended to be called at a quiescent point: callers
    // must not concurrently use the pool or retain cached blocks on other
    // live threads while trim is running.
    void trim() noexcept {
        try {
            flush_thread_cache();
            for (std::size_t index = 0; index < directories_.size(); ++index) {
                trim_directory(index);
            }
            std::lock_guard<std::mutex> lock(large_mutex_);
            trim_large_cache_locked(config_.large_cache_low_watermark);
        } catch (...) {
            // trim is best effort and is deliberately safe to call from
            // diagnostics or shutdown paths.
        }
    }

    [[nodiscard]] PoolStats snapshot() const noexcept {
        PoolStats result;
        result.allocation_attempts = counters_.allocation_attempts.load(std::memory_order_relaxed);
        result.successful_allocations = counters_.successful_allocations.load(std::memory_order_relaxed);
        result.failed_allocations = counters_.failed_allocations.load(std::memory_order_relaxed);
        result.successful_deallocations = counters_.successful_deallocations.load(std::memory_order_relaxed);
        result.invalid_deallocations = counters_.invalid_deallocations.load(std::memory_order_relaxed);
        result.double_deallocations = counters_.double_deallocations.load(std::memory_order_relaxed);
        result.requested_bytes = counters_.requested_bytes.load(std::memory_order_relaxed);
        result.live_requested_bytes = counters_.live_requested_bytes.load(std::memory_order_relaxed);
        result.peak_live_requested_bytes = counters_.peak_live_requested_bytes.load(std::memory_order_relaxed);
        result.live_allocations = counters_.live_allocations.load(std::memory_order_relaxed);
        result.live_size_class_waste_bytes = counters_.live_size_class_waste_bytes.load(std::memory_order_relaxed);
        result.reserved_bytes = counters_.reserved_bytes.load(std::memory_order_relaxed);
        result.peak_reserved_bytes = counters_.peak_reserved_bytes.load(std::memory_order_relaxed);
        result.committed_bytes = counters_.committed_bytes.load(std::memory_order_relaxed);
        result.pages_created = counters_.pages_created.load(std::memory_order_relaxed);
        result.pages_reclaimed = counters_.pages_reclaimed.load(std::memory_order_relaxed);
        result.resident_pages = counters_.resident_pages.load(std::memory_order_relaxed);
        result.segments_created = counters_.segments_created.load(std::memory_order_relaxed);
        result.segments_reclaimed = counters_.segments_reclaimed.load(std::memory_order_relaxed);
        result.resident_segments = counters_.resident_segments.load(std::memory_order_relaxed);
        result.os_allocations = counters_.os_allocations.load(std::memory_order_relaxed);
        result.os_releases = counters_.os_releases.load(std::memory_order_relaxed);
        result.local_cache_hits = counters_.local_cache_hits.load(std::memory_order_relaxed);
        result.local_cache_releases = counters_.local_cache_releases.load(std::memory_order_relaxed);
        result.remote_free_releases = counters_.remote_free_releases.load(std::memory_order_relaxed);
        result.large_cache_hits = counters_.large_cache_hits.load(std::memory_order_relaxed);
        result.large_cache_misses = counters_.large_cache_misses.load(std::memory_order_relaxed);
        result.large_cache_bytes = counters_.large_cache_bytes.load(std::memory_order_relaxed);

        const auto live_capacity = result.live_requested_bytes + result.live_size_class_waste_bytes;
        if (live_capacity != 0) {
            result.size_class_waste_ratio =
                static_cast<double>(result.live_size_class_waste_bytes) /
                static_cast<double>(live_capacity);
        }
        return result;
    }

    [[nodiscard]] const PoolConfig& config() const noexcept { return config_; }
    [[nodiscard]] const std::vector<std::size_t>& size_classes() const noexcept {
        return size_classes_;
    }

private:
    static constexpr std::uint8_t kFree = 0;
    static constexpr std::uint8_t kInUse = 1;
    static constexpr std::uint8_t kCached = 2;
    static constexpr std::uint32_t kNullIndex = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();
    static constexpr std::uint64_t kIndexMask = 0xffffffffULL;

    struct Lifetime {};

    struct Block {
        std::atomic<std::uint8_t> state{ kFree };
        std::atomic<std::uint32_t> next{ kNullIndex };
        std::atomic<std::uint64_t> owner_thread{ 0 };
        std::atomic<std::size_t> requested{ 0 };
    };

    struct Segment;

    struct Page {
        Page(MemoryPool* pool,
             Segment* segment,
             std::size_t page_bytes,
             std::size_t block_bytes,
             std::size_t alignment)
            : pool(pool),
              segment(segment),
              page_bytes(page_bytes),
              block_bytes(block_bytes),
              alignment(alignment),
              memory(os_allocate(page_bytes)) {
            if (memory == nullptr) {
                throw std::bad_alloc();
            }

            try {
                const auto begin_address = reinterpret_cast<std::uintptr_t>(memory);
                const auto end_address = begin_address + page_bytes;
                const auto aligned_address = align_up_address(begin_address, alignment);
                if (aligned_address < begin_address || aligned_address > end_address ||
                    block_bytes > end_address - aligned_address) {
                    throw std::bad_alloc();
                }

                data_begin = reinterpret_cast<std::byte*>(aligned_address);
                capacity = (end_address - aligned_address) / block_bytes;
                if (capacity == 0 || capacity > std::numeric_limits<std::uint32_t>::max()) {
                    throw std::bad_alloc();
                }

                blocks = std::unique_ptr<Block[]>(new Block[capacity]);
                for (std::size_t index = 0; index < capacity; ++index) {
                    blocks[index].next.store(
                        index + 1 < capacity ? static_cast<std::uint32_t>(index + 1) : kNullIndex,
                        std::memory_order_relaxed);
                }
                head.store(pack(1, 0), std::memory_order_release);
            } catch (...) {
                release_memory();
                throw;
            }
        }

        ~Page() { release_memory(); }

        Page(const Page&) = delete;
        Page& operator=(const Page&) = delete;

        [[nodiscard]] void* pointer(std::uint32_t index) const noexcept {
            return data_begin + static_cast<std::size_t>(index) * block_bytes;
        }

        [[nodiscard]] bool contains(const void* pointer_value) const noexcept {
            if (pointer_value == nullptr || data_begin == nullptr || capacity == 0) {
                return false;
            }
            const auto address = reinterpret_cast<std::uintptr_t>(pointer_value);
            const auto begin = reinterpret_cast<std::uintptr_t>(data_begin);
            if (block_bytes > std::numeric_limits<std::uintptr_t>::max() / capacity) {
                return false;
            }
            const auto span = block_bytes * capacity;
            if (address < begin || address - begin >= span) {
                return false;
            }
            return (address - begin) % block_bytes == 0;
        }

        [[nodiscard]] std::uint32_t index_of(const void* pointer_value) const noexcept {
            if (!contains(pointer_value)) {
                return kNullIndex;
            }
            const auto address = reinterpret_cast<std::uintptr_t>(pointer_value);
            const auto begin = reinterpret_cast<std::uintptr_t>(data_begin);
            return static_cast<std::uint32_t>((address - begin) / block_bytes);
        }

        [[nodiscard]] bool try_allocate_direct(std::uint32_t& index) noexcept {
            for (;;) {
                const auto candidate = pop_raw();
                if (candidate == kNullIndex) {
                    return false;
                }
                std::uint8_t expected = kFree;
                if (blocks[candidate].state.compare_exchange_strong(
                        expected, kInUse, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    live.fetch_add(1, std::memory_order_relaxed);
                    index = candidate;
                    return true;
                }
            }
        }

        [[nodiscard]] bool try_take_for_cache(std::uint32_t& index) noexcept {
            for (;;) {
                const auto candidate = pop_raw();
                if (candidate == kNullIndex) {
                    return false;
                }
                std::uint8_t expected = kFree;
                if (blocks[candidate].state.compare_exchange_strong(
                        expected, kCached, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    index = candidate;
                    return true;
                }
            }
        }

        [[nodiscard]] bool claim_cached(std::uint32_t index) noexcept {
            if (index >= capacity) {
                return false;
            }
            std::uint8_t expected = kCached;
            if (!blocks[index].state.compare_exchange_strong(
                    expected, kInUse, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return false;
            }
            live.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        [[nodiscard]] bool move_to_cache(std::uint32_t index) noexcept {
            if (index >= capacity) {
                return false;
            }
            std::uint8_t expected = kInUse;
            if (!blocks[index].state.compare_exchange_strong(
                    expected, kCached, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return false;
            }
            live.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }

        [[nodiscard]] bool release_to_page(std::uint32_t index) noexcept {
            if (index >= capacity) {
                return false;
            }
            std::uint8_t expected = kInUse;
            if (!blocks[index].state.compare_exchange_strong(
                    expected, kFree, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return false;
            }
            live.fetch_sub(1, std::memory_order_relaxed);
            push(index);
            return true;
        }

        void release_cached(std::uint32_t index) noexcept {
            if (index >= capacity) {
                return;
            }
            std::uint8_t expected = kCached;
            if (blocks[index].state.compare_exchange_strong(
                    expected, kFree, std::memory_order_acq_rel, std::memory_order_acquire)) {
                push(index);
            }
        }

        void flush_cached(std::uint32_t index) noexcept { release_cached(index); }

        [[nodiscard]] std::size_t live_count() const noexcept {
            return live.load(std::memory_order_acquire);
        }

        [[nodiscard]] std::size_t free_count() const noexcept {
            const auto used = live_count();
            return used <= capacity ? capacity - used : 0;
        }

        void release_memory() noexcept {
            if (memory == nullptr || released) {
                return;
            }
            os_release(memory, page_bytes);
            memory = nullptr;
            data_begin = nullptr;
            released = true;
        }

        static constexpr std::uint64_t pack(std::uint32_t tag, std::uint32_t index) noexcept {
            const auto low = index == kNullIndex ? 0ULL : static_cast<std::uint64_t>(index) + 1ULL;
            return (static_cast<std::uint64_t>(tag) << 32U) | low;
        }

        static constexpr std::uint32_t tag_of(std::uint64_t word) noexcept {
            return static_cast<std::uint32_t>(word >> 32U);
        }

        static constexpr std::uint32_t index_of_head(std::uint64_t word) noexcept {
            const auto low = static_cast<std::uint32_t>(word & kIndexMask);
            return low == 0 ? kNullIndex : low - 1U;
        }

        [[nodiscard]] std::uint32_t pop_raw() noexcept {
            std::uint64_t observed = head.load(std::memory_order_acquire);
            for (;;) {
                const auto index = index_of_head(observed);
                if (index == kNullIndex) {
                    return kNullIndex;
                }
                const auto next = blocks[index].next.load(std::memory_order_acquire);
                const auto desired = pack(tag_of(observed) + 1U, next);
                if (head.compare_exchange_weak(observed,
                                                desired,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                    return index;
                }
            }
        }

        void push(std::uint32_t index) noexcept {
            std::uint64_t observed = head.load(std::memory_order_acquire);
            for (;;) {
                blocks[index].next.store(index_of_head(observed), std::memory_order_release);
                const auto desired = pack(tag_of(observed) + 1U, index);
                if (head.compare_exchange_weak(observed,
                                               desired,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                    return;
                }
            }
        }

        MemoryPool* pool;
        Segment* segment;
        std::size_t page_bytes;
        std::size_t block_bytes;
        std::size_t alignment;
        std::byte* memory = nullptr;
        std::byte* data_begin = nullptr;
        std::size_t capacity = 0;
        std::unique_ptr<Block[]> blocks;
        std::atomic<std::uint64_t> head{ pack(0, kNullIndex) };
        std::atomic<std::size_t> live{ 0 };
        bool released = false;
    };

    struct Segment {
        std::vector<std::unique_ptr<Page>> pages;
    };

    struct SizeClassDirectory {
        std::size_t block_bytes = 0;
        std::mutex mutex;
        std::vector<std::unique_ptr<Segment>> segments;
    };

    struct CachedBlock {
        Page* page = nullptr;
        std::uint32_t index = kNullIndex;
    };

    struct CacheEntry {
        MemoryPool* pool = nullptr;
        std::weak_ptr<Lifetime> lifetime;
        std::vector<std::vector<CachedBlock>> buckets;
    };

    struct LargeRun {
        std::byte* base = nullptr;
        std::byte* user = nullptr;
        std::size_t mapping_bytes = 0;
        std::size_t requested_bytes = 0;
        std::size_t alignment = 0;
        bool active = false;
        bool released = false;
    };

    struct SmallLookup {
        Page* page = nullptr;
        std::uint32_t index = kNullIndex;
    };

    struct Counters {
        std::atomic<std::uint64_t> allocation_attempts{ 0 };
        std::atomic<std::uint64_t> successful_allocations{ 0 };
        std::atomic<std::uint64_t> failed_allocations{ 0 };
        std::atomic<std::uint64_t> successful_deallocations{ 0 };
        std::atomic<std::uint64_t> invalid_deallocations{ 0 };
        std::atomic<std::uint64_t> double_deallocations{ 0 };
        std::atomic<std::uint64_t> requested_bytes{ 0 };
        std::atomic<std::uint64_t> live_requested_bytes{ 0 };
        std::atomic<std::uint64_t> peak_live_requested_bytes{ 0 };
        std::atomic<std::uint64_t> live_allocations{ 0 };
        std::atomic<std::uint64_t> live_size_class_waste_bytes{ 0 };
        std::atomic<std::uint64_t> reserved_bytes{ 0 };
        std::atomic<std::uint64_t> peak_reserved_bytes{ 0 };
        std::atomic<std::uint64_t> committed_bytes{ 0 };
        std::atomic<std::uint64_t> pages_created{ 0 };
        std::atomic<std::uint64_t> pages_reclaimed{ 0 };
        std::atomic<std::uint64_t> resident_pages{ 0 };
        std::atomic<std::uint64_t> segments_created{ 0 };
        std::atomic<std::uint64_t> segments_reclaimed{ 0 };
        std::atomic<std::uint64_t> resident_segments{ 0 };
        std::atomic<std::uint64_t> os_allocations{ 0 };
        std::atomic<std::uint64_t> os_releases{ 0 };
        std::atomic<std::uint64_t> local_cache_hits{ 0 };
        std::atomic<std::uint64_t> local_cache_releases{ 0 };
        std::atomic<std::uint64_t> remote_free_releases{ 0 };
        std::atomic<std::uint64_t> large_cache_hits{ 0 };
        std::atomic<std::uint64_t> large_cache_misses{ 0 };
        std::atomic<std::uint64_t> large_cache_bytes{ 0 };
    };

    static std::vector<CacheEntry>& thread_cache_entries() noexcept {
        thread_local std::vector<CacheEntry> entries;
        return entries;
    }

    static std::uint64_t current_thread_id() noexcept {
        static std::atomic<std::uint64_t> next_id{ 1 };
        thread_local const std::uint64_t id = next_id.fetch_add(1, std::memory_order_relaxed);
        return id;
    }

    static constexpr bool is_power_of_two(std::size_t value) noexcept {
        return value != 0 && (value & (value - 1)) == 0;
    }

    static std::uintptr_t align_up_address(std::uintptr_t value,
                                           std::size_t alignment) noexcept {
        const auto remainder = value % alignment;
        if (remainder == 0) {
            return value;
        }
        const auto delta = static_cast<std::uintptr_t>(alignment - remainder);
        if (value > std::numeric_limits<std::uintptr_t>::max() - delta) {
            return std::numeric_limits<std::uintptr_t>::max();
        }
        return value + delta;
    }

    static void validate_config(const PoolConfig& config) {
        if (config.page_bytes < 4096 || !is_power_of_two(config.page_bytes)) {
            throw std::invalid_argument("page_bytes must be a power of two >= 4096");
        }
        if (config.segment_pages == 0 || config.small_max_bytes < 16 ||
            config.large_threshold_bytes <= config.small_max_bytes ||
            config.large_threshold_bytes > config.page_bytes) {
            throw std::invalid_argument("invalid memory pool size thresholds");
        }
        if (!(config.page_reclaim_ratio >= 0.0 && config.page_reclaim_ratio <= 1.0)) {
            throw std::invalid_argument("page_reclaim_ratio must be in [0, 1]");
        }
        if (config.resident_page_floor == 0 ||
            config.large_cache_low_watermark > config.large_cache_high_watermark) {
            throw std::invalid_argument("invalid reclaim floor or large cache watermark");
        }
    }

    static std::vector<std::size_t> make_size_classes(const PoolConfig& config) {
        std::vector<std::size_t> result;
        std::size_t value = 16;
        while (value < config.large_threshold_bytes) {
            result.push_back(value);
            std::size_t step = 16;
            if (value >= 128 && value < 512) {
                step = 64;
            } else if (value >= 512 && value < 1024) {
                step = 128;
            } else if (value >= 1024 && value < 4096) {
                step = 512;
            } else if (value >= 4096 && value < 16384) {
                step = 2048;
            } else if (value >= 16384) {
                step = 4096;
            }
            if (value > std::numeric_limits<std::size_t>::max() - step) {
                break;
            }
            value += step;
        }
        if (result.empty() || result.back() != config.large_threshold_bytes) {
            result.push_back(config.large_threshold_bytes);
        }
        return result;
    }

    [[nodiscard]] std::size_t choose_size_class(std::size_t bytes,
                                                std::size_t alignment) const noexcept {
        // Pages are aligned to max_align_t. Over-aligned requests use the
        // large-run path, where the returned address can be adjusted safely.
        if (alignment > alignof(std::max_align_t)) {
            return npos;
        }
        for (std::size_t index = 0; index < size_classes_.size(); ++index) {
            const auto size = size_classes_[index];
            if (size >= bytes && size % alignment == 0) {
                return index;
            }
        }
        return npos;
    }

    static bool normalize_request(std::size_t bytes,
                                  std::size_t alignment,
                                  std::size_t& normalized_bytes,
                                  std::size_t& normalized_alignment) noexcept {
        normalized_alignment = alignment == 0 ? alignof(std::max_align_t) : alignment;
        if (normalized_alignment < alignof(void*) || !is_power_of_two(normalized_alignment)) {
            return false;
        }
        const auto effective_bytes = bytes == 0 ? std::size_t{ 1 } : bytes;
        if (effective_bytes > std::numeric_limits<std::size_t>::max() - normalized_alignment + 1) {
            return false;
        }
        normalized_bytes = (effective_bytes + normalized_alignment - 1) &
                           ~(normalized_alignment - 1);
        return normalized_bytes != 0;
    }

    static std::byte* os_allocate(std::size_t bytes) noexcept {
#ifdef _WIN32
        return static_cast<std::byte*>(VirtualAlloc(nullptr,
                                                     bytes,
                                                     MEM_RESERVE | MEM_COMMIT,
                                                     PAGE_READWRITE));
#else
        void* result = mmap(nullptr,
                            bytes,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS,
                            -1,
                            0);
        return result == MAP_FAILED ? nullptr : static_cast<std::byte*>(result);
#endif
    }

    static void os_release(std::byte* memory, std::size_t bytes) noexcept {
        if (memory == nullptr) {
            return;
        }
#ifdef _WIN32
        (void)bytes;
        (void)VirtualFree(memory, 0, MEM_RELEASE);
#else
        (void)munmap(memory, bytes);
#endif
    }

    [[nodiscard]] CacheEntry* find_cache_entry(bool create) noexcept {
        auto& entries = thread_cache_entries();
        for (auto iterator = entries.begin(); iterator != entries.end(); ++iterator) {
            if (iterator->pool != this) {
                continue;
            }
            if (iterator->lifetime.expired()) {
                entries.erase(iterator);
                return nullptr;
            }
            return &*iterator;
        }
        if (!create) {
            return nullptr;
        }

        try {
            CacheEntry entry;
            entry.pool = this;
            entry.lifetime = lifetime_;
            entry.buckets.resize(size_classes_.size());
            entries.push_back(std::move(entry));
            return &entries.back();
        } catch (...) {
            return nullptr;
        }
    }

    void flush_thread_cache() noexcept {
        auto& entries = thread_cache_entries();
        for (auto iterator = entries.begin(); iterator != entries.end(); ++iterator) {
            if (iterator->pool != this) {
                continue;
            }
            for (auto& bucket : iterator->buckets) {
                for (const CachedBlock block : bucket) {
                    if (block.page != nullptr) {
                        block.page->flush_cached(block.index);
                    }
                }
                bucket.clear();
            }
            return;
        }
    }

    [[nodiscard]] bool pop_from_cache(std::size_t class_index, CachedBlock& result) noexcept {
        CacheEntry* entry = find_cache_entry(false);
        if (entry == nullptr || class_index >= entry->buckets.size()) {
            return false;
        }

        auto& bucket = entry->buckets[class_index];
        while (!bucket.empty()) {
            const CachedBlock candidate = bucket.back();
            bucket.pop_back();
            if (candidate.page != nullptr && candidate.page->claim_cached(candidate.index)) {
                result = candidate;
                counters_.local_cache_hits.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool push_to_cache(std::size_t class_index,
                                     Page* page,
                                     std::uint32_t index) noexcept {
        CacheEntry* entry = find_cache_entry(true);
        if (entry == nullptr || class_index >= entry->buckets.size()) {
            return false;
        }
        if (!page->move_to_cache(index)) {
            return false;
        }
        try {
            entry->buckets[class_index].push_back(CachedBlock{ page, index });
            counters_.local_cache_releases.fetch_add(1, std::memory_order_relaxed);
            return true;
        } catch (...) {
            page->release_cached(index);
            return false;
        }
    }

    [[nodiscard]] bool refill_cache(std::size_t class_index) noexcept {
        CacheEntry* entry = find_cache_entry(true);
        if (entry == nullptr || class_index >= entry->buckets.size()) {
            return false;
        }

        constexpr std::size_t batch = 32;
        auto& bucket = entry->buckets[class_index];
        auto& directory = *directories_[class_index];
        std::lock_guard<std::mutex> lock(directory.mutex);

        for (std::size_t count = 0; count < batch; ++count) {
            CachedBlock cached;
            Page* source = nullptr;
            for (auto& segment : directory.segments) {
                for (auto& page : segment->pages) {
                    std::uint32_t index = kNullIndex;
                    if (page->try_take_for_cache(index)) {
                        source = page.get();
                        cached = CachedBlock{ source, index };
                        break;
                    }
                }
                if (source != nullptr) {
                    break;
                }
            }

            if (source == nullptr) {
                auto page = create_page_locked(class_index);
                std::uint32_t index = kNullIndex;
                if (!page->try_take_for_cache(index)) {
                    break;
                }
                cached = CachedBlock{ page, index };
            }

            try {
                bucket.push_back(cached);
            } catch (...) {
                cached.page->release_cached(cached.index);
                break;
            }
        }
        return !bucket.empty();
    }

    [[nodiscard]] void* allocate_small(std::size_t class_index,
                                        std::size_t requested_bytes) noexcept {
        CachedBlock cached;
        if (pop_from_cache(class_index, cached)) {
            prepare_block(cached.page, cached.index, requested_bytes);
            return cached.page->pointer(cached.index);
        }

        if (refill_cache(class_index) && pop_from_cache(class_index, cached)) {
            prepare_block(cached.page, cached.index, requested_bytes);
            return cached.page->pointer(cached.index);
        }

        auto& directory = *directories_[class_index];
        std::lock_guard<std::mutex> lock(directory.mutex);
        for (auto& segment : directory.segments) {
            for (auto& page : segment->pages) {
                std::uint32_t index = kNullIndex;
                if (page->try_allocate_direct(index)) {
                    prepare_block(page.get(), index, requested_bytes);
                    return page->pointer(index);
                }
            }
        }

        Page* page = create_page_locked(class_index);
        std::uint32_t index = kNullIndex;
        if (!page->try_allocate_direct(index)) {
            return nullptr;
        }
        prepare_block(page, index, requested_bytes);
        return page->pointer(index);
    }

    void prepare_block(Page* page,
                       std::uint32_t index,
                       std::size_t requested_bytes) noexcept {
        Block& block = page->blocks[index];
        block.requested.store(requested_bytes, std::memory_order_relaxed);
        block.owner_thread.store(current_thread_id(), std::memory_order_release);
    }

    void record_successful_allocation(std::size_t requested_bytes,
                                      std::size_t capacity_bytes,
                                      bool account_size_class_waste) noexcept {
        counters_.successful_allocations.fetch_add(1, std::memory_order_relaxed);
        counters_.requested_bytes.fetch_add(requested_bytes, std::memory_order_relaxed);
        const auto live = counters_.live_requested_bytes.fetch_add(
                              requested_bytes, std::memory_order_relaxed) + requested_bytes;
        update_max(counters_.peak_live_requested_bytes, live);
        counters_.live_allocations.fetch_add(1, std::memory_order_relaxed);
        if (account_size_class_waste && capacity_bytes > requested_bytes) {
            counters_.live_size_class_waste_bytes.fetch_add(
                capacity_bytes - requested_bytes, std::memory_order_relaxed);
        }
    }

    void record_successful_deallocation(Page* page,
                                        std::uint32_t index,
                                        std::size_t capacity_bytes) noexcept {
        Block& block = page->blocks[index];
        const auto requested = block.requested.exchange(0, std::memory_order_acq_rel);
        block.owner_thread.store(0, std::memory_order_release);
        counters_.successful_deallocations.fetch_add(1, std::memory_order_relaxed);
        counters_.live_requested_bytes.fetch_sub(requested, std::memory_order_relaxed);
        counters_.live_allocations.fetch_sub(1, std::memory_order_relaxed);
        if (capacity_bytes > requested) {
            counters_.live_size_class_waste_bytes.fetch_sub(
                capacity_bytes - requested, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] SmallLookup locate_small(const void* pointer) const noexcept {
        SmallLookup result;
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        std::shared_lock<std::shared_mutex> lock(registry_mutex_);
        for (Page* page : pages_) {
            if (page->contains(pointer)) {
                result.page = page;
                result.index = page->index_of(reinterpret_cast<const void*>(address));
                return result;
            }
        }
        return result;
    }

    [[nodiscard]] bool deallocate_small(Page* page, std::uint32_t index) noexcept {
        if (page == nullptr || index == kNullIndex || index >= page->capacity) {
            counters_.invalid_deallocations.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const auto owner = page->blocks[index].owner_thread.load(std::memory_order_acquire);
        const auto current = current_thread_id();
        const auto class_index = class_index_for(page->block_bytes);
        if (owner == current && class_index != npos &&
            push_to_cache(class_index, page, index)) {
            record_successful_deallocation(page, index, page->block_bytes);
            return true;
        }

        if (page->release_to_page(index)) {
            record_successful_deallocation(page, index, page->block_bytes);
            counters_.remote_free_releases.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        counters_.double_deallocations.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    [[nodiscard]] std::size_t class_index_for(std::size_t block_bytes) const noexcept {
        const auto iterator = std::find(size_classes_.begin(), size_classes_.end(), block_bytes);
        return iterator == size_classes_.end()
                   ? npos
                   : static_cast<std::size_t>(std::distance(size_classes_.begin(), iterator));
    }

    [[nodiscard]] Page* create_page_locked(std::size_t class_index) {
        auto& directory = *directories_[class_index];
        Segment* segment = nullptr;
        if (!directory.segments.empty() &&
            directory.segments.back()->pages.size() < config_.segment_pages) {
            segment = directory.segments.back().get();
        } else {
            auto new_segment = std::make_unique<Segment>();
            segment = new_segment.get();
            directory.segments.push_back(std::move(new_segment));
            counters_.segments_created.fetch_add(1, std::memory_order_relaxed);
            counters_.resident_segments.fetch_add(1, std::memory_order_relaxed);
        }

        auto page = std::make_unique<Page>(this,
                                           segment,
                                           config_.page_bytes,
                                           size_classes_[class_index],
                                           alignof(std::max_align_t));
        Page* result = page.get();
        segment->pages.push_back(std::move(page));
        {
            std::unique_lock<std::shared_mutex> lock(registry_mutex_);
            pages_.push_back(result);
        }
        counters_.pages_created.fetch_add(1, std::memory_order_relaxed);
        counters_.resident_pages.fetch_add(1, std::memory_order_relaxed);
        add_reserved_bytes(config_.page_bytes);
        return result;
    }

    [[nodiscard]] void* allocate_large(std::size_t allocation_bytes,
                                        std::size_t logical_bytes,
                                        std::size_t alignment) noexcept {
        std::lock_guard<std::mutex> lock(large_mutex_);
        for (auto& holder : large_runs_) {
            LargeRun* run = holder.get();
            if (run->active || run->released || run->mapping_bytes < allocation_bytes) {
                continue;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(run->base);
            const auto aligned = align_up_address(base, alignment);
            const auto end = base + run->mapping_bytes;
            if (aligned < base || aligned > end || allocation_bytes > end - aligned) {
                continue;
            }
            run->user = reinterpret_cast<std::byte*>(aligned);
            run->requested_bytes = logical_bytes;
            run->alignment = alignment;
            run->active = true;
            remove_from_large_cache_locked(run);
            counters_.large_cache_hits.fetch_add(1, std::memory_order_relaxed);
            return run->user;
        }

        counters_.large_cache_misses.fetch_add(1, std::memory_order_relaxed);
        if (allocation_bytes > std::numeric_limits<std::size_t>::max() - alignment + 1) {
            return nullptr;
        }
        const auto required = allocation_bytes + alignment - 1;
        if (required > std::numeric_limits<std::size_t>::max() - config_.page_bytes + 1) {
            return nullptr;
        }
        const auto mapping_bytes =
            ((required + config_.page_bytes - 1) / config_.page_bytes) * config_.page_bytes;
        std::byte* base = os_allocate(mapping_bytes);
        if (base == nullptr) {
            return nullptr;
        }
        const auto base_address = reinterpret_cast<std::uintptr_t>(base);
            const auto aligned = align_up_address(base_address, alignment);
        if (aligned < base_address ||
            allocation_bytes > mapping_bytes - (aligned - base_address)) {
            os_release(base, mapping_bytes);
            return nullptr;
        }

        auto run = std::make_unique<LargeRun>();
        run->base = base;
        run->user = reinterpret_cast<std::byte*>(aligned);
        run->mapping_bytes = mapping_bytes;
        run->requested_bytes = logical_bytes;
        run->alignment = alignment;
        run->active = true;
        large_runs_.push_back(std::move(run));
        counters_.os_allocations.fetch_add(1, std::memory_order_relaxed);
        add_reserved_bytes(mapping_bytes);
        return reinterpret_cast<void*>(aligned);
    }

    [[nodiscard]] bool deallocate_large_locked(LargeRun* run) noexcept {
        if (run == nullptr || !run->active) {
            counters_.double_deallocations.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const auto requested = run->requested_bytes;
        run->active = false;
        run->requested_bytes = 0;
        counters_.successful_deallocations.fetch_add(1, std::memory_order_relaxed);
        counters_.live_requested_bytes.fetch_sub(requested, std::memory_order_relaxed);
        counters_.live_allocations.fetch_sub(1, std::memory_order_relaxed);
        add_to_large_cache_locked(run);
        return true;
    }

    void add_to_large_cache_locked(LargeRun* run) noexcept {
        if (run == nullptr || run->released) {
            return;
        }
        try {
            large_cache_.push_back(run);
            const auto bytes = counters_.large_cache_bytes.fetch_add(
                                   run->mapping_bytes, std::memory_order_relaxed) + run->mapping_bytes;
            while (bytes > config_.large_cache_high_watermark &&
                   counters_.large_cache_bytes.load(std::memory_order_relaxed) >
                       config_.large_cache_low_watermark) {
                if (large_cache_.empty()) {
                    break;
                }
                LargeRun* oldest = large_cache_.front();
                release_large_run_locked(oldest);
            }
        } catch (...) {
            release_large_run_locked(run);
        }
    }

    void remove_from_large_cache_locked(LargeRun* run) noexcept {
        const auto iterator = std::find(large_cache_.begin(), large_cache_.end(), run);
        if (iterator != large_cache_.end()) {
            large_cache_.erase(iterator);
            counters_.large_cache_bytes.fetch_sub(run->mapping_bytes, std::memory_order_relaxed);
        }
    }

    void release_large_run_locked(LargeRun* run) noexcept {
        if (run == nullptr || run->released || run->base == nullptr) {
            return;
        }
        const auto bytes = run->mapping_bytes;
        const auto iterator = std::find(large_cache_.begin(), large_cache_.end(), run);
        if (iterator != large_cache_.end()) {
            large_cache_.erase(iterator);
            counters_.large_cache_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        }
        os_release(run->base, bytes);
        run->base = nullptr;
        run->user = nullptr;
        run->mapping_bytes = 0;
        run->requested_bytes = 0;
        run->released = true;
        counters_.os_releases.fetch_add(1, std::memory_order_relaxed);
        subtract_reserved_bytes(bytes);
    }

    void trim_large_cache_locked(std::size_t target_bytes) noexcept {
        while (!large_cache_.empty() &&
            counters_.large_cache_bytes.load(std::memory_order_relaxed) > target_bytes) {
            LargeRun* run = large_cache_.front();
            release_large_run_locked(run);
        }
    }

    void release_large_runs() noexcept {
        std::lock_guard<std::mutex> lock(large_mutex_);
        for (auto& holder : large_runs_) {
            LargeRun* run = holder.get();
            if (run != nullptr && !run->released && run->base != nullptr) {
                os_release(run->base, run->mapping_bytes);
                run->base = nullptr;
                run->user = nullptr;
                run->mapping_bytes = 0;
                run->released = true;
            }
        }
        large_cache_.clear();
        counters_.large_cache_bytes.store(0, std::memory_order_relaxed);
    }

    void trim_directory(std::size_t class_index) noexcept {
        auto& directory = *directories_[class_index];
        std::lock_guard<std::mutex> lock(directory.mutex);
        std::size_t resident_pages = 0;
        for (const auto& segment : directory.segments) {
            resident_pages += segment->pages.size();
        }

        for (auto segment_iterator = directory.segments.begin();
             segment_iterator != directory.segments.end();) {
            Segment* segment = segment_iterator->get();
            for (auto page_iterator = segment->pages.begin();
                 page_iterator != segment->pages.end();) {
                Page* page = page_iterator->get();
                const auto free_ratio = page->capacity == 0
                                             ? 1.0
                                             : static_cast<double>(page->free_count()) /
                                                   static_cast<double>(page->capacity);
                const bool can_reclaim = page->live_count() == 0 &&
                                         free_ratio >= config_.page_reclaim_ratio &&
                                         resident_pages > config_.resident_page_floor;
                if (!can_reclaim) {
                    ++page_iterator;
                    continue;
                }
                {
                    std::unique_lock<std::shared_mutex> registry_lock(registry_mutex_);
                    const auto registry_iterator = std::find(pages_.begin(), pages_.end(), page);
                    if (registry_iterator != pages_.end()) {
                        pages_.erase(registry_iterator);
                    }
                }
                page->release_memory();
                page_iterator = segment->pages.erase(page_iterator);
                --resident_pages;
                counters_.pages_reclaimed.fetch_add(1, std::memory_order_relaxed);
                counters_.resident_pages.fetch_sub(1, std::memory_order_relaxed);
                counters_.os_releases.fetch_add(1, std::memory_order_relaxed);
                subtract_reserved_bytes(config_.page_bytes);
            }

            if (segment->pages.empty() && directory.segments.size() > 1) {
                segment_iterator = directory.segments.erase(segment_iterator);
                counters_.segments_reclaimed.fetch_add(1, std::memory_order_relaxed);
                counters_.resident_segments.fetch_sub(1, std::memory_order_relaxed);
            } else {
                ++segment_iterator;
            }
        }
    }

    static void update_max(std::atomic<std::uint64_t>& target,
                           std::uint64_t candidate) noexcept {
        auto current = target.load(std::memory_order_relaxed);
        while (current < candidate &&
               !target.compare_exchange_weak(current,
                                             candidate,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
        }
    }

    void add_reserved_bytes(std::size_t bytes) noexcept {
        const auto current = counters_.reserved_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        counters_.committed_bytes.fetch_add(bytes, std::memory_order_relaxed);
        update_max(counters_.peak_reserved_bytes, current);
    }

    void subtract_reserved_bytes(std::size_t bytes) noexcept {
        counters_.reserved_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        counters_.committed_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    }

    PoolConfig config_;
    std::shared_ptr<Lifetime> lifetime_;
    std::vector<std::size_t> size_classes_;
    std::vector<std::unique_ptr<SizeClassDirectory>> directories_;

    mutable std::shared_mutex registry_mutex_;
    std::vector<Page*> pages_;

    mutable std::mutex large_mutex_;
    std::vector<std::unique_ptr<LargeRun>> large_runs_;
    std::vector<LargeRun*> large_cache_;

    Counters counters_;
};

}  // namespace memory_pool

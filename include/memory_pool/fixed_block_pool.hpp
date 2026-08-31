#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace memory_pool {

class FixedBlockPool {
public:
    FixedBlockPool(std::size_t block_size, std::size_t block_count)
        : block_size_(block_size < sizeof(std::size_t) ? sizeof(std::size_t) : block_size),
          block_count_(block_count),
          storage_(std::make_unique<std::byte[]>(storage_size(block_size_, block_count_))),
          in_use_(std::make_unique<std::atomic<bool>[]>(block_count_)),
          head_(0) {
        for (std::size_t index = 0; index < block_count_; ++index) {
            in_use_[index].store(false, std::memory_order_relaxed);
            write_next(index, index + 1 < block_count_ ? index + 1 : npos);
        }
    }

    void* allocate() noexcept {
        auto current = head_.load(std::memory_order_acquire);
        while (current != npos) {
            const auto next = read_next(current);
            if (head_.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
                in_use_[current].store(true, std::memory_order_release);
                return storage_.get() + current * block_size_;
            }
        }
        return nullptr;
    }

    bool deallocate(void* pointer) noexcept {
        if (!contains(pointer)) {
            return false;
        }
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        const auto begin = reinterpret_cast<std::uintptr_t>(storage_.get());
        const auto index = static_cast<std::size_t>((address - begin) / block_size_);
        bool expected = true;
        if (!in_use_[index].compare_exchange_strong(
                expected, false, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return false;
        }
        auto current = head_.load(std::memory_order_acquire);
        do {
            write_next(index, current);
        } while (!head_.compare_exchange_weak(current, index, std::memory_order_acq_rel, std::memory_order_acquire));
        return true;
    }

    [[nodiscard]] bool contains(void* pointer) const noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        const auto begin = reinterpret_cast<std::uintptr_t>(storage_.get());
        const auto end = begin + block_size_ * block_count_;
        if (address < begin || address >= end) {
            return false;
        }
        return (address - begin) % block_size_ == 0;
    }

    [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }
    [[nodiscard]] std::size_t block_count() const noexcept { return block_count_; }

private:
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    static std::size_t storage_size(std::size_t block_size, std::size_t block_count) {
        if (block_count == 0) {
            throw std::invalid_argument("block_count must be positive");
        }
        if (block_size > std::numeric_limits<std::size_t>::max() / block_count) {
            throw std::invalid_argument("pool size overflows size_t");
        }
        return block_size * block_count;
    }

    void write_next(std::size_t index, std::size_t next) noexcept {
        std::memcpy(storage_.get() + index * block_size_, &next, sizeof(next));
    }

    [[nodiscard]] std::size_t read_next(std::size_t index) const noexcept {
        std::size_t next = npos;
        std::memcpy(&next, storage_.get() + index * block_size_, sizeof(next));
        return next;
    }

    const std::size_t block_size_;
    const std::size_t block_count_;
    std::unique_ptr<std::byte[]> storage_;
    std::unique_ptr<std::atomic<bool>[]> in_use_;
    std::atomic<std::size_t> head_;
};

}  // namespace memory_pool

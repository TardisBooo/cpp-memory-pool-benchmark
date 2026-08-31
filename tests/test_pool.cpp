#include "memory_pool/fixed_block_pool.hpp"

#include <cassert>
#include <vector>

int main() {
    memory_pool::FixedBlockPool pool(32, 2);
    void* first = pool.allocate();
    void* second = pool.allocate();
    assert(first != nullptr);
    assert(second != nullptr);
    assert(pool.allocate() == nullptr);
    assert(pool.contains(first));
    assert(!pool.contains(reinterpret_cast<void*>(0x1)));
    assert(pool.deallocate(first));
    assert(pool.allocate() == first);
    assert(pool.deallocate(second));
    return 0;
}

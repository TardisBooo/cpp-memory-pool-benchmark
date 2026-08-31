# C++ Memory Pool Benchmark

A clean-room C++17 systems demo for studying allocator behavior under
contention, mixed object lifetimes, cross-thread release, and large-object
reuse. It is intentionally self-contained: no company code, symbols, data,
internal filenames, or historical production measurements are included.

## What is implemented

The pool follows a layered ownership model:

```text
thread-local cache (TLD layer)
        ↓
size-class directory → segment → 64 KiB page → block
        ↓
page-rounded large run + bounded reuse cache
```

- Small and medium requests use size classes from 16 bytes through 32 KiB.
- Pages use a tagged atomic Treiber free-list and explicit `free`, `in-use`,
  and `cached` block states.
- A thread-local cache keeps hot blocks close to the allocating thread.
- A release from another thread goes back to the page free-list and is counted
  separately from local-cache release.
- Large requests use page-rounded OS mappings, alignment adjustment, and a
  high/low watermark cache.
- `trim()` can reclaim empty pages and large cached runs at a quiescent point.
- Counters expose allocation outcomes, live bytes, page lifecycle, cache hits,
  remote releases, reserved bytes, and size-class waste.

The public `owns()` check answers whether an address belongs to a pool-owned
page or an active large run. A small block may remain in a pool-owned page
after release because it is eligible for cache reuse; it is not a liveness
check.

## Build and test

Windows with Visual Studio 2022 is the primary development path:

```powershell
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Linux and other POSIX systems use the same commands:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

## Run the benchmark

The executable compares the pool with `new/delete` using deterministic
workloads:

```powershell
./build/Release/memory_pool_benchmark.exe `
  --threads 4 --iterations 100000 --scenario all `
  --output-dir build/report
```

Available scenarios are `small`, `mixed`, `large`, `cross`, and `all`.
`--no-system` runs only the pool. Every run writes three reviewable artifacts:

- `benchmark.json` — structured result data and pool counters;
- `benchmark.csv` — spreadsheet-friendly rows;
- `index.html` — a static, dependency-free report.

The result is machine-dependent. Compare changes only with the same compiler,
CPU, build mode, thread count, and workload; this project does not claim that
one allocator wins on every application.

## Public API

The implementation is header-only for easy inspection:

```cpp
#include "memory_pool/memory_pool.hpp"

memory_pool::MemoryPool pool;
void* pointer = pool.allocate(256);
if (pointer != nullptr) {
    // use the storage
    pool.deallocate(pointer);
}
```

`MemoryPool::allocate()` is `noexcept` and reports exhaustion or invalid
requests as `nullptr`. `deallocate()` returns `false` for foreign, repeated,
or otherwise invalid releases.

## Scope and limitations

This is an auditable engineering demo, not a drop-in replacement for a
production allocator. It deliberately keeps the implementation readable and
does not promise a complete production memory-reclamation scheme, ABI
compatibility, or universal benchmark superiority. `trim()` requires a
quiescent point: callers must not retain cached blocks on other live threads
while trimming.

## License

MIT.

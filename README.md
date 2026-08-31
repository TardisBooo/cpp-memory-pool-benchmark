# C++ Memory Pool Benchmark

A clean-room C++17 benchmark for studying fixed-size allocation under
contention. It is inspired by allocator design questions from performance
critical software, but contains no company code, symbols, data, or internal
thresholds.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
./build/memory_pool_benchmark --threads 4 --iterations 100000
```

The pool uses an atomic free-list for fixed-size blocks and a separate atomic
ownership bit per block so a repeated release is rejected instead of corrupting
the free-list. The benchmark reports successful allocations and failed
attempts; it is a teaching baseline, not a drop-in production allocator. It
does not replace a production allocator's full ABA, reclamation, alignment, or
instrumentation strategy. Compare it with the system allocator on the same
machine and record compiler, CPU, build mode, block size, and thread count.

## Questions this project makes measurable

- How does contention change with thread count?
- What is the effect of block size and pool capacity?
- How should exhaustion and ownership errors be surfaced?
- Which measurements are stable enough to publish?

## License

MIT.

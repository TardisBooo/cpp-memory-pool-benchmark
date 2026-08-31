#include "memory_pool/memory_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct Options {
    Options()
        : threads(std::max<std::size_t>(
              1, std::min<std::size_t>(8, std::thread::hardware_concurrency()))) {}

    std::size_t threads;
    std::size_t iterations = 100000;
    std::size_t rounds = 1;
    std::string scenario = "all";
    fs::path output_dir = fs::path("build") / "report";
    bool include_system_allocator = true;
};

struct Scenario {
    std::string id;
    std::string title;
    std::string description;
};

struct Counters {
    std::atomic<std::uint64_t> allocation_attempts{ 0 };
    std::atomic<std::uint64_t> successful_allocations{ 0 };
    std::atomic<std::uint64_t> failed_allocations{ 0 };
    std::atomic<std::uint64_t> successful_deallocations{ 0 };
};

struct BenchmarkResult {
    std::string scenario;
    std::string scenario_title;
    std::string allocator;
    std::size_t round = 0;
    std::size_t threads = 0;
    std::size_t iterations = 0;
    std::uint64_t operations = 0;
    std::uint64_t allocation_attempts = 0;
    std::uint64_t successful_allocations = 0;
    std::uint64_t failed_allocations = 0;
    std::uint64_t successful_deallocations = 0;
    double elapsed_ms = 0.0;
    double operations_per_second = 0.0;
    memory_pool::PoolStats pool_stats{};
    bool has_pool_stats = false;
};

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::size_t parse_size(const std::string& text, const char* option_name) {
    if (text.empty() || text.front() == '-') {
        throw std::invalid_argument(std::string("invalid value for ") + option_name);
    }
    std::size_t consumed = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(text, &consumed, 10);
    } catch (...) {
        throw std::invalid_argument(std::string("invalid value for ") + option_name);
    }
    if (consumed != text.size() || value == 0 ||
        value > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        throw std::invalid_argument(std::string("invalid value for ") + option_name);
    }
    return static_cast<std::size_t>(value);
}

void print_usage(const char* executable) {
    std::cout << "Usage: " << executable << " [options]\n\n"
              << "Options:\n"
              << "  --threads N       worker threads (default: capped at 8)\n"
              << "  --iterations N    iterations per worker (default: 100000)\n"
              << "  --rounds N        repeat each scenario (default: 1)\n"
              << "  --scenario NAME   all|small|mixed|large|cross (default: all)\n"
              << "  --output-dir PATH write JSON, CSV, and HTML reports here\n"
              << "  --no-system       skip the system allocator comparison\n"
              << "  --help            show this help\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto require_value = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                throw std::invalid_argument(std::string("missing value for ") + name);
            }
            return argv[++index];
        };

        if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        }
        if (argument == "--threads") {
            options.threads = parse_size(require_value("--threads"), "--threads");
        } else if (argument == "--iterations") {
            options.iterations = parse_size(require_value("--iterations"), "--iterations");
        } else if (argument == "--rounds") {
            options.rounds = parse_size(require_value("--rounds"), "--rounds");
        } else if (argument == "--scenario") {
            options.scenario = lower_copy(require_value("--scenario"));
        } else if (argument == "--output-dir") {
            options.output_dir = require_value("--output-dir");
        } else if (argument == "--no-system") {
            options.include_system_allocator = false;
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }

    if (options.threads > 1024) {
        throw std::invalid_argument("--threads must be <= 1024");
    }
    if (options.scenario != "all" && options.scenario != "small" &&
        options.scenario != "mixed" && options.scenario != "large" &&
        options.scenario != "cross") {
        throw std::invalid_argument("--scenario must be all, small, mixed, large, or cross");
    }
    return options;
}

std::vector<Scenario> selected_scenarios(const Options& options) {
    const std::vector<Scenario> scenarios = {
        { "small_contention", "Small-object contention",
          "Immediate allocate/release for 64-byte objects." },
        { "mixed_lifetime", "Mixed object lifetimes",
          "Small, medium, and large objects with a bounded live set." },
        { "large_cache", "Large-run cache",
          "Page-rounded allocations that exercise large-object reuse." },
        { "cross_thread_release", "Cross-thread release",
          "One worker allocates while a different worker releases the blocks." },
    };
    if (options.scenario == "all") {
        return scenarios;
    }
    const std::string selected_id =
        options.scenario == "small" ? "small_contention" :
        options.scenario == "mixed" ? "mixed_lifetime" :
        options.scenario == "large" ? "large_cache" : "cross_thread_release";
    for (const auto& scenario : scenarios) {
        if (scenario.id == selected_id) {
            return { scenario };
        }
    }
    return {};
}

template <typename Allocate, typename Deallocate>
BenchmarkResult run_parallel_scenario(const Options& options,
                                      const Scenario& scenario,
                                      std::size_t round,
                                      const std::string& allocator_name,
                                      Allocate allocate,
                                      Deallocate deallocate) {
    Counters counters;
    std::vector<std::thread> workers;
    workers.reserve(options.threads);
    std::atomic<std::size_t> ready{ 0 };
    std::atomic<std::size_t> phase_ready{ 0 };
    std::atomic<bool> start{ false };
    std::vector<std::vector<void*>> cross_thread_blocks;
    if (scenario.id == "cross_thread_release") {
        cross_thread_blocks.resize(options.threads);
        for (auto& blocks : cross_thread_blocks) {
            blocks.reserve(options.iterations);
        }
    }

    const auto wait_for_start = [&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };

    const auto worker_body = [&](std::size_t worker_index) {
        wait_for_start();
        if (scenario.id == "small_contention") {
            for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
                counters.allocation_attempts.fetch_add(1, std::memory_order_relaxed);
                void* pointer = allocate(64);
                if (pointer == nullptr) {
                    counters.failed_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                counters.successful_allocations.fetch_add(1, std::memory_order_relaxed);
                static_cast<std::byte*>(pointer)[0] = std::byte{ 0xa5 };
                if (deallocate(pointer)) {
                    counters.successful_deallocations.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return;
        }

        if (scenario.id == "large_cache") {
            constexpr std::size_t sizes[] = {
                64u * 1024u + 1u,
                96u * 1024u + 17u,
                192u * 1024u + 31u,
            };
            for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
                counters.allocation_attempts.fetch_add(1, std::memory_order_relaxed);
                void* pointer = allocate(sizes[iteration % 3]);
                if (pointer == nullptr) {
                    counters.failed_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                counters.successful_allocations.fetch_add(1, std::memory_order_relaxed);
                static_cast<std::byte*>(pointer)[0] = std::byte{ 0x3c };
                if (deallocate(pointer)) {
                    counters.successful_deallocations.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return;
        }

        if (scenario.id == "cross_thread_release") {
            for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
                counters.allocation_attempts.fetch_add(1, std::memory_order_relaxed);
                void* pointer = allocate(128);
                if (pointer == nullptr) {
                    counters.failed_allocations.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                counters.successful_allocations.fetch_add(1, std::memory_order_relaxed);
                static_cast<std::byte*>(pointer)[0] = std::byte{ 0x7f };
                cross_thread_blocks[worker_index].push_back(pointer);
            }
            phase_ready.fetch_add(1, std::memory_order_release);
            while (phase_ready.load(std::memory_order_acquire) < options.threads) {
                std::this_thread::yield();
            }
            const auto& to_release = cross_thread_blocks[(worker_index + 1) % options.threads];
            for (void* pointer : to_release) {
                if (deallocate(pointer)) {
                    counters.successful_deallocations.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return;
        }

        std::vector<void*> live;
        live.reserve(32);
        constexpr std::size_t sizes[] = { 24, 96, 512, 2048, 8192, 16000, 33000, 120000 };
        for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
            counters.allocation_attempts.fetch_add(1, std::memory_order_relaxed);
            void* pointer = allocate(sizes[(iteration + worker_index) % 8]);
            if (pointer == nullptr) {
                counters.failed_allocations.fetch_add(1, std::memory_order_relaxed);
            } else {
                counters.successful_allocations.fetch_add(1, std::memory_order_relaxed);
                static_cast<std::byte*>(pointer)[0] = std::byte{ 0x19 };
                live.push_back(pointer);
            }
            if (live.size() >= 16) {
                if (deallocate(live.front())) {
                    counters.successful_deallocations.fetch_add(1, std::memory_order_relaxed);
                }
                live.erase(live.begin());
            }
        }
        for (void* pointer : live) {
            if (deallocate(pointer)) {
                counters.successful_deallocations.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    const auto started = std::chrono::steady_clock::now();
    for (std::size_t worker = 0; worker < options.threads; ++worker) {
        workers.emplace_back(worker_body, worker);
    }
    while (ready.load(std::memory_order_acquire) < options.threads) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    BenchmarkResult result;
    result.scenario = scenario.id;
    result.scenario_title = scenario.title;
    result.allocator = allocator_name;
    result.round = round;
    result.threads = options.threads;
    result.iterations = options.iterations;
    result.allocation_attempts = counters.allocation_attempts.load(std::memory_order_relaxed);
    result.successful_allocations = counters.successful_allocations.load(std::memory_order_relaxed);
    result.failed_allocations = counters.failed_allocations.load(std::memory_order_relaxed);
    result.successful_deallocations = counters.successful_deallocations.load(std::memory_order_relaxed);
    result.operations = result.successful_allocations + result.successful_deallocations;
    result.elapsed_ms = elapsed;
    result.operations_per_second = elapsed > 0.0
                                       ? static_cast<double>(result.operations) * 1000.0 / elapsed
                                       : 0.0;
    return result;
}

BenchmarkResult run_pool(const Options& options, const Scenario& scenario, std::size_t round) {
    memory_pool::PoolConfig config;
    config.preallocate_pages_per_class = 1;
    memory_pool::MemoryPool pool(config);
    auto result = run_parallel_scenario(options,
                                        scenario,
                                        round,
                                        "memory_pool",
                                        [&](std::size_t bytes) { return pool.allocate(bytes); },
                                        [&](void* pointer) { return pool.deallocate(pointer); });
    result.pool_stats = pool.snapshot();
    result.has_pool_stats = true;
    return result;
}

BenchmarkResult run_system_allocator(const Options& options,
                                     const Scenario& scenario,
                                     std::size_t round) {
    return run_parallel_scenario(options,
                                 scenario,
                                 round,
                                 "system_new_delete",
                                 [](std::size_t bytes) {
                                     return ::operator new(bytes, std::nothrow);
                                 },
                                 [](void* pointer) {
                                     ::operator delete(pointer);
                                     return true;
                                 });
}

std::string json_escape(const std::string& value) {
    std::ostringstream escaped;
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': escaped << "\\\\"; break;
        case '"': escaped << "\\\""; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (character < 0x20) {
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned int>(character) << std::dec;
            } else {
                escaped << static_cast<char>(character);
            }
        }
    }
    return escaped.str();
}

std::string csv_escape(const std::string& value) {
    if (value.find_first_of(",\"\n\r") == std::string::npos) {
        return value;
    }
    std::string result = "\"";
    for (const char character : value) {
        if (character == '"') {
            result += "\"\"";
        } else {
            result += character;
        }
    }
    result += '"';
    return result;
}

void write_json_number(std::ostream& output, double value) {
    output << std::fixed << std::setprecision(3) << value;
}

void write_json(const fs::path& path,
                const Options& options,
                const std::vector<BenchmarkResult>& results) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot write " + path.string());
    }
    output << "{\n  \"schema_version\": 1,\n"
           << "  \"threads\": " << options.threads << ",\n"
           << "  \"iterations\": " << options.iterations << ",\n"
           << "  \"rounds\": " << options.rounds << ",\n"
           << "  \"results\": [\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        output << "    {\n"
               << "      \"scenario\": \"" << json_escape(result.scenario) << "\",\n"
               << "      \"title\": \"" << json_escape(result.scenario_title) << "\",\n"
               << "      \"allocator\": \"" << json_escape(result.allocator) << "\",\n"
               << "      \"round\": " << result.round << ",\n"
               << "      \"operations\": " << result.operations << ",\n"
               << "      \"allocation_attempts\": " << result.allocation_attempts << ",\n"
               << "      \"successful_allocations\": " << result.successful_allocations << ",\n"
               << "      \"failed_allocations\": " << result.failed_allocations << ",\n"
               << "      \"successful_deallocations\": " << result.successful_deallocations << ",\n"
               << "      \"elapsed_ms\": ";
        write_json_number(output, result.elapsed_ms);
        output << ",\n      \"operations_per_second\": ";
        write_json_number(output, result.operations_per_second);
        if (result.has_pool_stats) {
            const auto& stats = result.pool_stats;
            output << ",\n      \"pool_stats\": {\n"
                   << "        \"reserved_bytes\": " << stats.reserved_bytes << ",\n"
                   << "        \"resident_pages\": " << stats.resident_pages << ",\n"
                   << "        \"pages_created\": " << stats.pages_created << ",\n"
                   << "        \"pages_reclaimed\": " << stats.pages_reclaimed << ",\n"
                   << "        \"local_cache_hits\": " << stats.local_cache_hits << ",\n"
                   << "        \"remote_free_releases\": " << stats.remote_free_releases << ",\n"
                   << "        \"large_cache_hits\": " << stats.large_cache_hits << ",\n"
                   << "        \"large_cache_misses\": " << stats.large_cache_misses << ",\n"
                   << "        \"size_class_waste_ratio\": ";
            write_json_number(output, stats.size_class_waste_ratio);
            output << "\n      }\n";
        } else {
            output << ",\n      \"pool_stats\": null\n";
        }
        output << "    }" << (index + 1 == results.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
}

void write_csv(const fs::path& path, const std::vector<BenchmarkResult>& results) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot write " + path.string());
    }
    output << "scenario,allocator,round,threads,iterations,operations,allocation_attempts,"
              "successful_allocations,failed_allocations,successful_deallocations,elapsed_ms,"
              "operations_per_second,reserved_bytes,resident_pages,pages_created,pages_reclaimed,"
              "local_cache_hits,remote_free_releases,large_cache_hits,large_cache_misses,"
              "size_class_waste_ratio\n";
    for (const auto& result : results) {
        const auto& stats = result.pool_stats;
        output << csv_escape(result.scenario) << ',' << csv_escape(result.allocator) << ','
               << result.round << ',' << result.threads << ',' << result.iterations << ','
               << result.operations << ',' << result.allocation_attempts << ','
               << result.successful_allocations << ',' << result.failed_allocations << ','
               << result.successful_deallocations << ',' << std::fixed << std::setprecision(3)
               << result.elapsed_ms << ',' << result.operations_per_second << ','
               << stats.reserved_bytes << ',' << stats.resident_pages << ',' << stats.pages_created
               << ',' << stats.pages_reclaimed << ',' << stats.local_cache_hits << ','
               << stats.remote_free_releases << ',' << stats.large_cache_hits << ','
               << stats.large_cache_misses << ',' << stats.size_class_waste_ratio << '\n';
    }
}

std::string html_escape(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        default: result += character;
        }
    }
    return result;
}

void write_html(const fs::path& path,
                const Options& options,
                const std::vector<BenchmarkResult>& results) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot write " + path.string());
    }
    output << R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>C++ Memory Pool Benchmark</title>
  <style>
    :root { color-scheme: light; --ink:#17231f; --muted:#5b6c65; --line:#d7e4dc; --green:#0d8057; --mint:#eaf7ef; }
    * { box-sizing:border-box; }
    body { margin:0; font:15px/1.55 system-ui,-apple-system,"Segoe UI",sans-serif; color:var(--ink); background:linear-gradient(135deg,#f5fbf6,#eef7f2); }
    main { max-width:1180px; margin:0 auto; padding:52px 22px 72px; }
    .eyebrow { color:var(--green); font-size:12px; font-weight:800; letter-spacing:.14em; text-transform:uppercase; }
    h1 { margin:10px 0 8px; font-size:clamp(34px,5vw,62px); line-height:1.02; letter-spacing:-.045em; }
    .intro { max-width:720px; color:var(--muted); font-size:17px; }
    .cards { display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:12px; margin:30px 0; }
    .card, .panel { background:rgba(255,255,255,.78); border:1px solid var(--line); border-radius:18px; box-shadow:0 12px 30px rgba(42,83,61,.08); }
    .card { padding:18px; }
    .card strong { display:block; font-size:24px; }
    .card span { color:var(--muted); font-size:13px; }
    .panel { overflow:auto; }
    table { width:100%; border-collapse:collapse; min-width:950px; }
    th,td { padding:13px 14px; text-align:left; border-bottom:1px solid var(--line); white-space:nowrap; }
    th { color:var(--muted); font-size:12px; text-transform:uppercase; letter-spacing:.08em; background:var(--mint); }
    tr:last-child td { border-bottom:0; }
    .pool { color:var(--green); font-weight:700; }
    .note { margin-top:18px; color:var(--muted); font-size:13px; }
    code { padding:2px 5px; border-radius:5px; background:#edf3ef; }
    @media (max-width:700px) { .cards { grid-template-columns:1fr; } main { padding-top:30px; } }
  </style>
</head>
<body><main>
  <div class="eyebrow">Clean-room C++17 systems demo</div>
  <h1>Memory Pool Benchmark</h1>
  <p class="intro">A reproducible comparison of a layered pool (thread-local cache -&gt; segment -&gt; page -&gt; block, plus large-run reuse) and the system allocator. Read the raw data in <code>benchmark.json</code> or <code>benchmark.csv</code>.</p>
  <section class="cards">
    <div class="card"><strong>)HTML"
           << results.size() << R"HTML(</strong><span>benchmark rows</span></div>
    <div class="card"><strong>)HTML"
           << options.threads << R"HTML(</strong><span>worker threads</span></div>
    <div class="card"><strong>)HTML"
           << options.iterations << R"HTML(</strong><span>iterations / worker</span></div>
  </section>
  <div class="panel"><table><thead><tr>
    <th>Scenario</th><th>Allocator</th><th>Round</th><th>Operations</th><th>Elapsed (ms)</th><th>Ops / sec</th><th>Reserved</th><th>Pages</th><th>Cache hits</th><th>Remote frees</th>
  </tr></thead><tbody>
)HTML";
    for (const auto& result : results) {
        const auto& stats = result.pool_stats;
        output << "    <tr><td>" << html_escape(result.scenario_title) << "</td><td class=\""
               << (result.has_pool_stats ? "pool" : "") << "\">"
               << html_escape(result.allocator) << "</td><td>" << result.round << "</td><td>"
               << result.operations << "</td><td>" << std::fixed << std::setprecision(3)
               << result.elapsed_ms << "</td><td>" << result.operations_per_second
               << "</td><td>" << stats.reserved_bytes << "</td><td>" << stats.resident_pages
               << "</td><td>" << stats.local_cache_hits + stats.large_cache_hits << "</td><td>"
               << stats.remote_free_releases << "</td></tr>\n";
    }
    output << R"HTML(  </tbody></table></div>
  <p class="note">This report is an engineering instrument: rerun it on the same machine with the same compiler, build mode, thread count, and workload before comparing changes.</p>
</main></body></html>
)HTML";
}

void write_reports(const fs::path& output_dir,
                   const Options& options,
                   const std::vector<BenchmarkResult>& results) {
    fs::create_directories(output_dir);
    write_json(output_dir / "benchmark.json", options, results);
    write_csv(output_dir / "benchmark.csv", results);
    write_html(output_dir / "index.html", options, results);
}

void print_result(const BenchmarkResult& result) {
    std::cout << std::left << std::setw(24) << result.scenario << std::setw(20)
              << result.allocator << "ops=" << std::setw(10) << result.operations
              << "elapsed_ms=" << std::fixed << std::setprecision(3) << std::setw(10)
              << result.elapsed_ms << "ops/s=" << std::setprecision(1)
              << result.operations_per_second << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        const auto scenarios = selected_scenarios(options);
        if (scenarios.empty()) {
            throw std::invalid_argument("no scenario selected");
        }

        std::vector<BenchmarkResult> results;
        results.reserve(options.rounds * scenarios.size() *
                        (options.include_system_allocator ? 2 : 1));
        for (std::size_t round = 1; round <= options.rounds; ++round) {
            for (const auto& scenario : scenarios) {
                auto pool_result = run_pool(options, scenario, round);
                print_result(pool_result);
                results.push_back(std::move(pool_result));
                if (options.include_system_allocator) {
                    auto system_result = run_system_allocator(options, scenario, round);
                    print_result(system_result);
                    results.push_back(std::move(system_result));
                }
            }
        }

        write_reports(options.output_dir, options, results);
        std::cout << "reports: " << (options.output_dir / "index.html").string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\nUse --help for options.\n";
        return 2;
    }
}

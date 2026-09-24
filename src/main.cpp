#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>
#include <numeric>
#include <random>
#include <stdexcept>
#include <thread>

// Reading this value after a run makes the computed result observable.
volatile std::uint64_t result_sink = 0;

constexpr std::size_t KiB = 1024;
constexpr std::size_t MiB = 1024 * KiB;
constexpr std::size_t bytes_per_trial = 256 * MiB;
constexpr int trial_count = 3;

// NOINLINE keeps the scan separate from the code that fills the vector.
// The compiler barrier makes each repeat perform its reads again.
__attribute__((noinline))
std::uint64_t scan(const std::vector<std::uint64_t>& data,
                   std::size_t repeats) {
    std::uint64_t total = 0;
    for (std::size_t pass = 0; pass < repeats; ++pass) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        for (std::uint64_t value : data) {
            total += value;
        }
    }
    return total;
}

void benchmark(std::size_t size_bytes) {
    const std::size_t element_count = size_bytes / sizeof(std::uint64_t);
    std::vector<std::uint64_t> data(element_count);
    for (std::size_t i = 0; i < element_count; ++i) {
        data[i] = i % 256;
    }

    // Keep total traffic similar for every data-set size.
    const std::size_t repeats = bytes_per_trial / size_bytes;

    // Warm up outside the timed section.
    result_sink = result_sink ^ scan(data, 1);

    std::vector<double> seconds;
    seconds.reserve(trial_count);
    for (int trial = 0; trial < trial_count; ++trial) {
        const auto start = std::chrono::steady_clock::now();
        const std::uint64_t result = scan(data, repeats);
        const auto end = std::chrono::steady_clock::now();

        result_sink = result_sink ^ result;
        seconds.push_back(std::chrono::duration<double>(end - start).count());
    }

    std::sort(seconds.begin(), seconds.end());
    const double median_seconds = seconds[trial_count / 2];
    const double gigabytes_read =
        static_cast<double>(size_bytes) * repeats / 1'000'000'000.0;
    const double gb_per_second = gigabytes_read / median_seconds;

    std::cout << std::setw(10) << size_bytes / KiB << " KiB  "
              << std::setw(7) << repeats << " passes  "
              << std::fixed << std::setprecision(3)
              << std::setw(9) << median_seconds * 1000 << " ms  "
              << std::setprecision(2) << std::setw(7) << gb_per_second
              << " GB/s\n";
}

__attribute__((noinline))
std::size_t chase(const std::vector<std::size_t>& next,
                  std::size_t index,
                  std::size_t hops) {
    for (std::size_t hop = 0; hop < hops; ++hop) {
        index = next[index];
    }
    return index;
}

void benchmark_pointer_chase(std::size_t size_bytes) {
    const std::size_t element_count = size_bytes / sizeof(std::size_t);
    std::vector<std::size_t> next(element_count);
    std::size_t start_index = 0;

    // Shuffle the indices, then connect them into one cycle.
    {
        std::vector<std::size_t> order(element_count);
        std::iota(order.begin(), order.end(), 0);

        std::mt19937 engine(42);
        std::shuffle(order.begin(), order.end(), engine);

        start_index = order.front();

        for (std::size_t i = 0; i < element_count; ++i) {
            next[order[i]] = order[(i + 1) % element_count];
        }
    }

    const std::size_t hops = (64 * MiB) / sizeof(std::size_t);

    // Warm up before starting the timer.
    result_sink = result_sink ^ chase(next, start_index, element_count);

    std::vector<double> seconds;

    for (int trial = 0; trial < trial_count; ++trial) {
        const auto start = std::chrono::steady_clock::now();

        const std::size_t final_index = chase(next, start_index, hops);

        const auto end = std::chrono::steady_clock::now();

        result_sink = result_sink ^ final_index;
        seconds.push_back(
            std::chrono::duration<double>(end - start).count()
        );
    }

    std::sort(seconds.begin(), seconds.end());

    const double median_seconds = seconds[trial_count / 2];
    const double nanoseconds_per_hop =
        median_seconds * 1'000'000'000.0 / static_cast<double>(hops);

    std::cout << std::setw(10) << size_bytes / KiB << " KiB  "
              << std::fixed << std::setprecision(2)
              << nanoseconds_per_hop << " ns/hop\n";
}

struct CompactCounter {
    std::atomic<std::uint64_t> value{0};
};

struct alignas(64) PaddedCounter {
    std::atomic<std::uint64_t> value{0};
};

template <typename Counter>
double run_workers(std::vector<Counter>& counters,
                   std::size_t increments) {
    const std::size_t thread_count = counters.size();

    for (auto& counter : counters) {
        counter.value.store(0, std::memory_order_relaxed);
    }

    std::atomic<std::size_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (std::size_t i = 0; i < thread_count; ++i) {
        workers.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_relaxed);

            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::size_t n = 0; n < increments; ++n) {
                counters[i].value.fetch_add(
                    1, std::memory_order_relaxed
                );
            }
        });
    }

    while (ready.load(std::memory_order_relaxed) != thread_count) {
        std::this_thread::yield();
    }

    const auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);

    for (auto& worker : workers) {
        worker.join();
    }

    const auto end = std::chrono::steady_clock::now();

    std::uint64_t total = 0;
    for (const auto& counter : counters) {
        total += counter.value.load(std::memory_order_relaxed);
    }

    if (total != thread_count * increments) {
        throw std::runtime_error("Counter total is incorrect");
    }

    return std::chrono::duration<double>(end - start).count();
}

template <typename Counter>
double median_run(std::size_t thread_count,
                  std::size_t increments) {
    std::vector<Counter> counters(thread_count);
    std::vector<double> times;

    for (int trial = 0; trial < trial_count; ++trial) {
        times.push_back(run_workers(counters, increments));
    }

    std::sort(times.begin(), times.end());
    return times[trial_count / 2];
}

void benchmark_false_sharing(std::size_t thread_count) {
    constexpr std::size_t increments = 5'000'000;

    const double compact_seconds =
        median_run<CompactCounter>(thread_count, increments);

    const double padded_seconds =
        median_run<PaddedCounter>(thread_count, increments);

    const double operations =
        static_cast<double>(thread_count) * increments;

    std::cout << thread_count << " threads: "
              << "compact "
              << std::fixed << std::setprecision(1)
              << operations / compact_seconds / 1'000'000.0
              << " M increments/s, padded "
              << operations / padded_seconds / 1'000'000.0
              << " M increments/s\n";
}

int main() {
    std::cout << "Sequential read throughput (median of " << trial_count
              << " trials)\n";
    for (std::size_t size : {32 * KiB, 256 * KiB, 4 * MiB, 64 * MiB}) {
        benchmark(size);
    }

    std::cout << "\nRandom pointer-chase latency (median of "
          << trial_count << " trials)\n";

for (std::size_t size : {32 * KiB, 256 * KiB, 4 * MiB, 64 * MiB}) {
    benchmark_pointer_chase(size);
}

std::cout << "\nCounter throughput: compact vs padded\n";

for (std::size_t threads : {1, 2, 4}) {
    benchmark_false_sharing(threads);
}
    return 0;
}

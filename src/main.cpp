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
    return 0;
}

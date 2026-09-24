#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

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

int main() {
    std::cout << "Sequential read throughput (median of " << trial_count
              << " trials)\n";
    for (std::size_t size : {32 * KiB, 256 * KiB, 4 * MiB, 64 * MiB}) {
        benchmark(size);
    }
    return 0;
}

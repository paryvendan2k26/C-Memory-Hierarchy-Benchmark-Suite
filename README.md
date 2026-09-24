# Memory Hierarchy Benchmark Suite

The first experiment measures how quickly one C++ thread can repeatedly scan
arrays of different sizes. Later experiments can add pointer chasing and
multithreaded counters. The output is **effective sequential-read throughput**;
it is not a direct measurement of peak DRAM bandwidth.

## Build and run

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic src/main.cpp -o memory-bench
./memory-bench
```

Run these commands from this directory. No external C++ libraries are needed.

## What the first experiment does

1. Allocate a vector of 64-bit integers and fill it **before** timing.
2. Scan it once as a warm-up.
3. Scan it repeatedly until about 256 MiB has been read in each trial.
4. Run three trials, select their median time, and calculate
   `GB/s = bytes_read / seconds / 1,000,000,000`.
5. Repeat for 32 KiB, 256 KiB, 4 MiB, and 64 MiB arrays.

`std::chrono::steady_clock` measures elapsed time. `result_sink` makes the
computed sum observable so the compiler cannot discard the calculation. The
compiler barrier in `scan()` prevents it from moving repeated reads out of the
outer loop; it does not issue a CPU memory fence. The `noinline` attribute is a
GCC/Clang extension used for this small experiment.

## How to interpret it

The sizes may fit different cache levels on your CPU. Do not label a particular
row "L1", "L2", "L3", or "DRAM" without checking your machine's cache sizes.
The 64 MiB row may still fit in a large last-level cache on some systems.
Cache state, CPU frequency, other processes, and the summation loop also affect
the result. The warm-up means the first trial starts with some data already
cached. Repeat runs and compare trends rather than treating one number as a
hardware specification.

## Your first exercise

Run it twice and write down: which data size had the highest throughput, which
had the lowest, and a hypothesis for the difference. Then inspect your CPU's
cache sizes with `lscpu -C` (if supported) and compare them to the rows.

Next milestone: add a pointer-chasing workload to measure dependent-read
latency, where memory-level parallelism is deliberately limited.

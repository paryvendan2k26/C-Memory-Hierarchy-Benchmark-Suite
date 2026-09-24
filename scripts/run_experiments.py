import argparse
import csv
import re
import statistics
import subprocess
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt


SEQUENTIAL = re.compile(
    r"^\s*(\d+) KiB\s+\d+ passes\s+[\d.]+ ms\s+([\d.]+) GB/s$"
)
CHASE = re.compile(
    r"^\s*(\d+) KiB\s+([\d.]+) ns/hop$"
)
COUNTERS = re.compile(
    r"^(\d+) threads: compact ([\d.]+) M increments/s, "
    r"padded ([\d.]+) M increments/s$"
)


def parse_output(output):
    measurements = []

    for line in output.splitlines():
        if match := SEQUENTIAL.match(line):
            measurements.append(
                ("sequential", int(match[1]), "read",
                 float(match[2]), "GB/s")
            )
        elif match := CHASE.match(line):
            measurements.append(
                ("chase", int(match[1]), "random",
                 float(match[2]), "ns/hop")
            )
        elif match := COUNTERS.match(line):
            threads = int(match[1])
            measurements.append(
                ("counters", threads, "compact",
                 float(match[2]), "M increments/s")
            )
            measurements.append(
                ("counters", threads, "padded",
                 float(match[3]), "M increments/s")
            )

    if len(measurements) != 14:
        raise ValueError(
            f"Expected 14 measurements, found {len(measurements)}.\n"
            f"Program output:\n{output}"
        )

    return measurements


def save_csv(path, header, rows):
    with path.open("w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(header)
        writer.writerows(rows)


def make_plot(summary, experiment, variants, xlabel, ylabel, path):
    plt.figure(figsize=(8, 5))

    for variant in variants:
        points = sorted(
            (parameter, median)
            for name, parameter, label, median, _unit in summary
            if name == experiment and label == variant
        )

        x_values = [parameter for parameter, _ in points]
        y_values = [median for _, median in points]
        plt.plot(x_values, y_values, marker="o", label=variant)

    if experiment != "counters":
        plt.xscale("log", base=2)
        plt.xticks([32, 256, 4096, 65536],
                   ["32", "256", "4096", "65536"])
    else:
        plt.xticks([1, 2, 4])

    plt.xlabel(xlabel)
    plt.ylabel(ylabel)
    plt.title(experiment.capitalize())
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(path)
    plt.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="./memory-bench")
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()

    if args.runs < 1:
        parser.error("--runs must be at least 1")

    binary = Path(args.binary).resolve()
    output_dir = Path("results")
    output_dir.mkdir(exist_ok=True)

    raw_rows = []
    grouped = defaultdict(list)
    units = {}

    for run_number in range(1, args.runs + 1):
        print(f"Running experiment {run_number}/{args.runs}...")

        completed = subprocess.run(
            [str(binary)],
            capture_output=True,
            text=True,
            check=True,
        )

        for experiment, parameter, variant, value, unit in parse_output(
            completed.stdout
        ):
            raw_rows.append(
                (run_number, experiment, parameter, variant, value, unit)
            )
            key = (experiment, parameter, variant)
            grouped[key].append(value)
            units[key] = unit

    summary = [
        (*key, statistics.median(values), units[key])
        for key, values in sorted(grouped.items())
    ]

    save_csv(
        output_dir / "raw_results.csv",
        ["run", "experiment", "parameter", "variant", "value", "unit"],
        raw_rows,
    )
    save_csv(
        output_dir / "summary.csv",
        ["experiment", "parameter", "variant", "median", "unit"],
        summary,
    )

    make_plot(
        summary, "sequential", ["read"],
        "Array size (KiB)", "Throughput (GB/s)",
        output_dir / "sequential.png",
    )
    make_plot(
        summary, "chase", ["random"],
        "Array size (KiB)", "Time per hop (ns)",
        output_dir / "chase.png",
    )
    make_plot(
        summary, "counters", ["compact", "padded"],
        "Threads", "Throughput (million increments/s)",
        output_dir / "counters.png",
    )

    print(f"Saved CSV files and three graphs in {output_dir}/")


if __name__ == "__main__":
    main()
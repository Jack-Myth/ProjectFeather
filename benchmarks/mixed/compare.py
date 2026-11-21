"""Run from any directory with --feather and --lua pointing to the CLIs."""

import argparse
import math
from pathlib import Path
from statistics import median
import subprocess
import sys


HERE = Path(__file__).resolve().parent
EXPECTED = {"merge_sort": 365865366030, "edit_distance": 90, "particles": 737250846}


def sample(command):
    process = subprocess.run(command, capture_output=True, text=True, timeout=120)
    if process.returncode:
        raise RuntimeError(f"{command} failed\n{process.stdout}\n{process.stderr}")
    lines = process.stdout.splitlines()
    names = list(EXPECTED) * 4
    if len(lines) != len(names) * 3:
        raise ValueError(f"Unexpected output from {command}: {lines}")
    times = {name: [] for name in EXPECTED}
    for index, expected_name in enumerate(names):
        name, duration, checksum = lines[3 * index:3 * index + 3]
        ms = float(duration)
        if (name != expected_name or float(checksum) != EXPECTED[name]
                or not math.isfinite(ms) or ms <= 0):
            raise ValueError(f"{command}: invalid sample {name} {duration} {checksum}")
        if index >= len(EXPECTED):  # Discard the first batch of each workload.
            times[name].append(ms)
    return times


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--feather", default="feather")
    parser.add_argument("--lua", default="lua")
    args = parser.parse_args()
    commands = {
        "Feather": [args.feather, "run", HERE / "workloads.fe"],
        "Python": [sys.executable, HERE / "workloads.py"],
        "Lua": [args.lua, HERE / "workloads.lua"],
    }
    results = {name: {lang: [] for lang in commands} for name in EXPECTED}
    order = list(commands)
    for offset in range(len(order)):
        for lang in order[offset:] + order[:offset]:
            for name, times in sample(commands[lang]).items():
                results[name][lang].extend(times)
    print("workload,feather_ms,python_ms,lua_ms,feather/python,feather/lua")
    for name, times in results.items():
        f, p, l = (median(times[lang]) for lang in commands)
        print(f"{name},{f:.3f},{p:.3f},{l:.3f},{f/p:.2f},{f/l:.2f}")


if __name__ == "__main__":
    main()

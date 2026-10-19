#!/usr/bin/env python3
"""Measure fresh Clang -O3 invocations for the runtime benchmark references."""
import argparse
import csv
import hashlib
import json
import math
import platform
import re
from pathlib import Path
import shutil
import subprocess
import statistics
import time


def definition(source, function, templated=False):
    pattern = r"std::uint64_t\s+" + re.escape(function) + r"\s*\("
    if templated:
        pattern = r"template\s*<\s*std::size_t\s*\.\.\.\s*N\s*>\s*" + pattern
    match = re.search(pattern, source)
    if match is None:
        raise ValueError(f"benchmark definition not found: {function}")
    start = match.start()
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def expected_results():
    mask = (1 << 64) - 1
    checksum = 42
    for i in range(16384):
        checksum = ((checksum * 3) ^ i) & mask
    arithmetic = []
    for count in [64, 256]:
        r = 42
        for i in range(count):
            r = (((r * 3) ^ 42) + i + 1) & mask
        arithmetic.append(r)
    flags = [1] * 8190
    primes = 0
    for i in range(2, len(flags)):
        if flags[i]:
            primes += 1
            for k in range(i + i + 1, len(flags), i + 1):
                flags[k] = 0
    seed, total = 42, 0
    for _ in range(4096):
        seed = (seed ^ (seed << 13)) & mask
        seed ^= seed >> 7
        seed = (seed ^ (seed << 17)) & mask
        total = (total + seed) & mask
    return [43, checksum, primes, *arithmetic, 44, total]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=shutil.which("clang++"))
    parser.add_argument("--trials", type=int, default=21)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    if not args.compiler or args.trials < 1 or args.warmups < 0:
        parser.error("Clang, positive trials and nonnegative warmups are required")
    root = Path(__file__).resolve().parent
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    original = root / "JITExecutionReference.cpp"
    source = original.read_text()
    kinds = [
        ("tiny", "tiny", "Tiny", []),
        ("checksum_loop_16384", "checksum_loop", "ChecksumLoop", []),
        ("sieve_8190", "sieve", "Sieve", []),
        ("arithmetic_64", "arithmetic64", "Arithmetic64", ["arithmetic"]),
        ("arithmetic_256", "arithmetic256", "Arithmetic256", ["arithmetic"]),
        ("branches_32", "branches32", "Branches32", ["branches"]),
        ("memory_sum_4096", "memory_sum", "MemorySum", []),
    ]
    cases = []
    for index, (name, function, kind, helpers) in enumerate(kinds):
        bodies = [definition(source, helper, templated=True) for helper in helpers]
        bodies.append(definition(source, function))
        generated = ('#include "JITExecutionReference.hpp"\n#include <array>\n#include <utility>\n'
                     'namespace mirnext::benchmark { namespace {\n' + "\n".join(bodies)
                     + '\n}\nNativeFunction reference_function(RuntimeWorkload kind) {\n'
                     + f'  return kind == RuntimeWorkload::{kind} ? &{function} : nullptr;\n}}\n}}\n')
        path = output / (name + ".cpp")
        path.write_text(generated)
        cases.append((name, path, [str(index)]))
    cases.append(("all_7_reference_functions", original, []))
    expected = ",".join(str(n) + "ULL" for n in expected_results())
    harness = output / "driver.cpp"
    harness.write_text('''#include "JITExecutionReference.hpp"
#include <array>
using namespace mirnext::benchmark;
int main(int argc, char **argv) {
  const std::uint64_t expected[]{''' + expected + '''};
  unsigned first = argc > 1 ? unsigned(argv[1][0]-'0') : 0;
  unsigned end = argc > 1 ? first+1 : 7;
  if (first >= 7) return 1;
  std::array<std::uint64_t,4096> memory; std::uint64_t seed = 42;
  for (auto &value : memory) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; value = seed; }
  for (unsigned n = first; n < end; ++n) {
    auto f = reference_function(static_cast<RuntimeWorkload>(n)); if (!f) return 2;
    auto argument = n == 1 ? UINT64_C(16384) : n == 2 ? UINT64_C(2)
      : n == 6 ? std::uint64_t(reinterpret_cast<std::uintptr_t>(memory.data())) : UINT64_C(42);
    if (f(argument) != expected[n]) return 3;
  }
}
''')
    common = [args.compiler, "-std=c++20", "-O3", "-DNDEBUG", "-fno-exceptions", "-fno-rtti", "-fno-lto", "-I", str(root)]
    if platform.system() == "Darwin":
        sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
        common += ["-arch", platform.machine(), "-isysroot", sdk]
    driver = output / "driver.o"

    def run(command):
        start = time.perf_counter_ns()
        checked = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        elapsed = (time.perf_counter_ns() - start) / 1e6
        if checked.returncode:
            raise RuntimeError(f"command failed ({checked.returncode}): " + repr(command) + "\n" + checked.stderr)
        return elapsed

    run([*common, "-c", str(harness), "-o", str(driver)])
    commands = {}
    for name, path, validation in cases:
        obj = output / (name + ".o")
        executable = output / (name + ".exe")
        commands[name] = {
            "source_to_object": [*common, "-c", str(path), "-o", str(obj)],
            "object_to_executable": [*common, str(driver), str(obj), "-o", str(executable)],
            "source_to_executable": [*common, str(driver), str(path), "-o", str(executable)],
        }
        # Install the link-only prerequisite before rotating phase order.
        run(commands[name]["source_to_object"])
    version = subprocess.check_output([args.compiler, "--version"], text=True)
    manifest = {
        "compiler_version": version, "platform": platform.platform(), "machine": platform.machine(),
        "trials": args.trials, "warmups": args.warmups, "concurrent_compilers": 1,
        "boundary": "subprocess wall time; includes compiler/linker startup, frontend, file I/O; excludes driver.cpp compilation and execution",
        "reference_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
        "sources_sha256": {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path, _ in cases},
        "commands": commands,
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    with (output / "samples.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["workload", "level", "phase", "trial", "elapsed_ms"])
        for trial in range(-args.warmups, args.trials):
            for name, _, validation in cases:
                phases = list(commands[name])
                offset = (trial + args.warmups) % len(phases)
                for phase in phases[offset:] + phases[:offset]:
                    elapsed = run(commands[name][phase])
                    if phase != "source_to_object":
                        run([str(output / (name + ".exe")), *validation])
                    if trial >= 0:
                        writer.writerow([name, "O3", phase, trial, f"{elapsed:.6f}"])
                        stream.flush()
            print(f"round {trial + 1}/{args.trials} complete", flush=True)
    with (output / "samples.csv").open(newline="") as stream:
        groups = {}
        for row in csv.DictReader(stream):
            groups.setdefault((row["workload"], row["phase"]), []).append(float(row["elapsed_ms"]))
    with (output / "summary.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["workload", "level", "phase", "samples", "median_ms", "p95_ms"])
        for (name, phase), values in groups.items():
            values.sort()
            writer.writerow([name, "O3", phase, len(values), f"{statistics.median(values):.6f}",
                             f"{values[math.ceil(0.95 * len(values)) - 1]:.6f}"])


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Run the small benchmark smoke suite after building its executables."""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--configuration", default="Release")
args = parser.parse_args()
suffix = ".exe" if os.name == "nt" else ""
binary = args.build
if (binary / args.configuration / ("mirnext_classic_benchmark" + suffix)).exists():
    binary /= args.configuration
for workload in ("loop", "sieve"):
    subprocess.run([str(binary / ("mirnext_classic_benchmark" + suffix)), "--" + workload, "--quick"], check=True)
subprocess.run([str(binary / ("mirnext_format_io_benchmark" + suffix)), "--functions", "32"], check=True)

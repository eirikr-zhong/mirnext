#!/usr/bin/env python3
"""Generate the embedded runner's C++ input header."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
for name in ("m2b", "b2cpp", "input", "output"):
    parser.add_argument("--" + name, required=True)
args = parser.parse_args()
binary = subprocess.run([args.m2b], input=Path(args.input).read_bytes(), capture_output=True, check=True)
header = subprocess.run([args.b2cpp], input=binary.stdout, capture_output=True, check=True)
Path(args.output).write_bytes(header.stdout)

#!/usr/bin/env python3
"""Check fatal Result diagnostics in isolated child processes."""
import subprocess
import sys

probe = sys.argv[1]
result = subprocess.run([probe, "success"], capture_output=True, text=True)
if result.returncode != 0 or result.stderr:
    raise RuntimeError(f"Successful Result expression was not evaluated once: {result}")
for mode in ("value-failure", "void-failure"):
    result = subprocess.run([probe, mode], capture_output=True, text=True)
    if result.returncode != 1 or "first diagnostic" not in result.stderr or "repeated factory evaluation" in result.stderr:
        raise RuntimeError(f"Fatal Result expression lost its first diagnostic ({mode}): {result}")

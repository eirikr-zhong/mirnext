#!/usr/bin/env python3
"""Exercise a compiled embedded program in every execution mode."""
import os
import subprocess
import sys

for mode in ("interp", "gen", "lazy", "bbv"):
    environment = dict(os.environ, MIRNEXT_MODE=mode, MIRNEXT_LIBS="")
    result = subprocess.run([sys.argv[1], "embedded payload"], env=environment, capture_output=True, text=True)
    if result.returncode != 42 or result.stdout != "embedded payload\n":
        raise RuntimeError(f"{mode} embedded runner: {result}")

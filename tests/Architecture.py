#!/usr/bin/env python3
"""Check project source layout and project-header ownership."""
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
for directory in ("include/mirnext", "lib", "tools", "tests", "examples", "benchmarks"):
    for path in (root / directory).rglob("*"):
        if path.is_file() and path.suffix in (".c", ".h"):
            raise RuntimeError("Project sources must use .cpp and .hpp extensions: " + str(path))
files = []
for directory, suffixes in (("include/mirnext", (".hpp", ".def")),
                            ("lib", (".cpp", ".hpp")), ("tools", (".cpp", ".hpp"))):
    files += [p for p in (root / directory).rglob("*") if p.is_file() and p.suffix in suffixes]
for path in files:
    for name in re.findall(r'#\s*include\s*"([^"]+)"', path.read_text(encoding="utf-8")):
        if name.startswith("mirnext/") or (path.parent == root / "tools" and name in ("Run.hpp", "Streams.hpp")):
            continue
        raise RuntimeError(f"Project header outside its namespace: {path}: {name}")
print(f"Checked header ownership for {len(files)} C++ source files")

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
public_headers = sorted((root / "include/mirnext").rglob("*.hpp"))
for path in public_headers:
    source = path.read_text(encoding="utf-8")
    for name in re.findall(r'#\s*include\s*"([^"]+)"', source):
        if not (root / "include" / name).is_file():
            raise RuntimeError(f"Installed header depends on private implementation: {path}: {name}")
    if re.search(r"Result\s*<\s*std::unique_ptr\s*<", source):
        raise RuntimeError(f"Public owning results must return value objects: {path}")
    if re.search(r"Result\s*<\s*(?:const\s+)?[\w:]+\s*\*\s*>", source):
        raise RuntimeError(f"Public borrowed results must return references: {path}")
    if re.search(r"class\s+(?:Context|IRBuilder|ProgramBuilder|ModuleBuilder|FunctionBuilder|ProgramEditor|ModuleEditor|FunctionEditor|InstructionBuilder|InstructionRewriter|SSAUpdater|MachineRewriter)\s*(?:final\s*)?\{", source):
        raise RuntimeError(f"Old construction interface in installed headers: {path}")
result_source = (root / "include/mirnext/Support/Result.hpp").read_text(encoding="utf-8")
if re.search(r"operator\s*(?:\*|->|bool)", result_source):
    raise RuntimeError("Result and OptionalRef access must use named methods")
for path in (root / "examples").glob("*.cpp"):
    for name in re.findall(r'#\s*include\s*["<]([^">]+)[">]', path.read_text(encoding="utf-8")):
        if name.startswith("mirnext/") and not (root / "include" / name).is_file():
            raise RuntimeError(f"Example depends on private implementation: {path}: {name}")
    if re.search(r"\*\*\s*\w+", path.read_text(encoding="utf-8")):
        raise RuntimeError(f"Example uses double dereference for object access: {path}")
for directory in ("lib", "tools", "tests", "examples", "benchmarks"):
    for path in (root / directory).rglob("*.cpp"):
        if re.search(r"ExecutionSession::create\s*\(\s*\*\*", path.read_text(encoding="utf-8")):
            raise RuntimeError(f"Session input still uses an old owning-pointer result: {path}")
print(f"Checked header ownership for {len(files)} C++ source files and {len(public_headers)} public headers")

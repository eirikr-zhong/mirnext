#!/usr/bin/env python3
"""Identify source snapshots and generate acceptance-test build inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def source_files(root):
    files = {Path(name) for name in ("CMakeLists.txt", "LICENSE", "NOTICE")}
    extensions = {
        "include/mirnext": {".hpp", ".def"},
        "lib": {".cpp", ".hpp"},
        "tools": {".cpp", ".hpp", ".md"},
        "examples": {".cpp"},
        "tests": {".cpp", ".hpp", ".cmake", ".py", ".mir"},
        "cmake": {".cmake", ".py"},
        ".github/workflows": {".yml"},
    }
    for directory, suffixes in extensions.items():
        for path in (root / directory).rglob("*"):
            if path.is_file() and (path.suffix in suffixes or path.name == "CMakeLists.txt"):
                files.add(path.relative_to(root))
    files.update(path.relative_to(root) for path in (root / "benchmarks").rglob("CMakeLists.txt"))
    return sorted(files, key=lambda path: path.as_posix())


def source_fingerprint(root):
    root = Path(root)
    manifest = "MIRNext native source fingerprint v2\n"
    for path in source_files(root):
        content = (root / path).read_bytes().replace(b"\r\n", b"\n")
        manifest += hashlib.sha256(content).hexdigest() + " " + path.as_posix() + "\n"
    return hashlib.sha256(manifest.encode()).hexdigest()


def write_changed(path, content):
    path = Path(path)
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(content, encoding="utf-8")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--header", type=Path)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--configuration")
    parser.add_argument("--ctest", default="ctest")
    args = parser.parse_args()
    fingerprint = source_fingerprint(args.source)
    if args.header:
        write_changed(args.header, '#pragma once\n#define MIRNEXT_TEST_SOURCE_FINGERPRINT "' + fingerprint + '"\n')
    if args.build:
        if not args.configuration:
            parser.error("--build requires --configuration")
        inventory = subprocess.run([args.ctest, "--test-dir", str(args.build), "-C", args.configuration,
                                    "--show-only=json-v1"], check=True, capture_output=True, text=True)
        tests = [test["name"] for test in json.loads(inventory.stdout)["tests"]]
        manifest = dict(schema=2, source_fingerprint=fingerprint,
                        configuration=args.configuration, tests=sorted(tests))
        write_changed(args.build / ("native-test-manifest-" + args.configuration + ".json"),
                      json.dumps(manifest, indent=2) + "\n")
    if not args.header and not args.build:
        print(fingerprint)


if __name__ == "__main__":
    main()

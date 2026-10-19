#!/usr/bin/env python3
"""Check native acceptance artifacts; never generate or execute foreign code."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

PLATFORMS = {
    "linux-x86_64": ("linux", "x86_64", "x86_64-sysv"),
    "macos-x86_64": ("macos", "x86_64", "x86_64-sysv"),
    "windows-x86_64": ("windows", "x86_64", "x86_64-windows"),
    "linux-aarch64": ("linux", "aarch64", "aarch64-linux"),
    "macos-aarch64": ("macos", "aarch64", "aarch64-macos"),
}
SCENES = {("original-interpreter", 0)} | set(itertools.product(
    ("optimized-interpreter", "eager", "lazy", "bbv", "mixed"), range(4)))
LINKAGES = ("static", "shared")
IDENTITY_FIELDS = ("source_fingerprint", "configuration", "c_reference")


class InvalidEvidence(ValueError):
    pass


def require(condition, reason):
    if not condition:
        raise InvalidEvidence(reason)


def load_json(path):
    def unique(pairs):
        obj = {}
        for key, value in pairs:
            require(key not in obj, "duplicate JSON field: " + key)
            obj[key] = value
        return obj
    with path.open(encoding="utf-8") as stream:
        return json.load(stream, object_pairs_hook=unique)


def source_fingerprint(source):
    with tempfile.TemporaryDirectory(prefix="mirnext-evidence-") as work:
        output = Path(work) / "source.txt"
        result = subprocess.run([
            "cmake", "-DSOURCE_DIR=" + str(source), "-DOUTPUT_FILE=" + str(output),
            "-P", str(source / "tests/Platform/SourceFingerprint.cmake")],
            text=True, capture_output=True)
        require(result.returncode == 0, "source fingerprint failed: " + result.stderr)
        digest = output.read_text().strip()
        require(re.fullmatch(r"[0-9a-f]{64}", digest), "invalid source fingerprint")
        return digest


def count(obj, key, expected=None):
    value = obj.get(key)
    require(type(value) is int and value >= 0, "invalid count: " + key)
    require(expected is None or value == expected, "unexpected count: " + key)
    return value


def validate_receipt(receipt, fingerprint):
    require(type(receipt) is dict and type(receipt.get("schema")) is int and receipt["schema"] == 2, "unsupported receipt schema")
    require(receipt.get("status") == "passed", "receipt did not pass")
    require(receipt.get("source_fingerprint") == fingerprint, "stale or different source fingerprint")
    require(receipt.get("translated") is False, "translated or unknown execution")
    require(receipt.get("diagnostic") == "", "receipt contains a diagnostic")
    require(receipt.get("linkage") in LINKAGES, "invalid linkage")
    require(type(receipt.get("c_reference")) is bool, "missing reference build identity")
    require(receipt.get("configuration") in ("Debug", "Release", "RelWithDebInfo", "MinSizeRel"), "invalid configuration")
    host = (receipt.get("host_system"), receipt.get("host_architecture"), receipt.get("target"))
    names = [name for name, identity in PLATFORMS.items() if identity == host]
    require(len(names) == 1, "host and target ABI do not match")
    require(receipt.get("expected_target") in ("", host[2]), "expected target does not match")
    count(receipt, "completed_scenes", 21)
    scenes = receipt.get("scenes")
    require(type(scenes) is list and len(scenes) == 21, "incomplete scene list")
    seen, fingerprints = set(), set()
    for scene in scenes:
        require(type(scene) is dict, "invalid scene")
        mode, level = scene.get("mode"), count(scene, "level")
        require((mode, level) in SCENES and (mode, level) not in seen, "missing or duplicate scene")
        seen.add((mode, level))
        count(scene, "checked_calls", 5)
        count(scene, "raw_calls", 1)
        fingerprints.add(count(scene, "input_fingerprint"))
        preparations = count(scene, "preparations")
        require(count(scene, "legalizations") == preparations and count(scene, "frames") == preparations,
                "incomplete machine stages")
        blocks = count(scene, "encoded_blocks")
        if mode.endswith("interpreter"):
            require(preparations == 0 and blocks == 0, "interpreter compiled a function body")
        else:
            require(preparations > 0, "native machine stages were not observed")
            require(mode != "bbv" or blocks > 0, "BBV did not encode reached blocks")
        adapters = count(scene, "adapter_preparations")
        require(adapters > 0 and count(scene, "adapter_legalizations") == adapters
                and count(scene, "adapter_frames") == adapters, "incomplete native adapter stages")
    require(seen == SCENES and len(fingerprints) == 1, "scene input or coverage differs")
    return (names[0], receipt["configuration"], receipt["c_reference"], receipt["linkage"])


def marker(receipt):
    return "Native acceptance source {} configuration {} linkage {} c-reference {}".format(
        receipt["source_fingerprint"], receipt["configuration"], receipt["linkage"],
        str(receipt["c_reference"]).lower())


def validate_suite(manifest, junit, receipt):
    require(type(manifest) is dict and type(manifest.get("schema")) is int and manifest["schema"] == 1, "unsupported test manifest")
    require(type(manifest.get("c_reference")) is bool and all(manifest.get(key) == receipt[key] for key in IDENTITY_FIELDS), "test manifest build differs")
    expected = manifest.get("tests")
    require(type(expected) is list and all(type(name) is str for name in expected)
            and len(expected) == len(set(expected)), "invalid test manifest")
    critical = {"mirnext-native-host", "mirnext-unified-architecture"}
    critical.update("mirnext-native-acceptance-" + linkage for linkage in LINKAGES)
    require(critical <= set(expected), "test manifest omits native or architecture checks")
    cases = list(junit.iter("testcase"))
    require(junit.tag == "testsuite" and int(junit.get("tests", "-1")) == len(cases), "partial JUnit suite")
    for field in ("failures", "errors", "disabled", "skipped"):
        require(int(junit.get(field, "0")) == 0, "JUnit reports " + field)
    names = [case.get("name") for case in cases]
    require(len(names) == len(set(names)) and set(names) == set(expected), "JUnit does not cover the configured test manifest")
    for case in cases:
        require(case.get("status") == "run" and not any(case.find(tag) is not None for tag in ("failure", "error", "skipped")),
                "test failed, skipped or did not run: " + str(case.get("name")))
        if case.get("name") == "mirnext-native-acceptance-" + receipt["linkage"]:
            require(marker(receipt) in (case.findtext("system-out") or ""), "JUnit acceptance identity differs")


def audit(roots, fingerprint, required):
    found, suites = {}, {}
    paths = sorted({path.resolve() for root in roots for path in root.rglob("native-acceptance-*.json")
                    if not path.name.endswith(".mismatch.json")})
    for path in paths:
        receipt = load_json(path)
        identity = validate_receipt(receipt, fingerprint)
        # Repeated artifact copies are harmless only when byte-for-byte identical.
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        require(identity not in found or found[identity][1] == digest, "conflicting receipts for " + str(identity))
        manifest_path = path.parent / ("native-test-manifest-" + receipt["configuration"] + ".json")
        junit_path = path.parent / ("native-tests-" + receipt["configuration"] + ".xml")
        if not junit_path.exists():
            junit_path = path.parent / "native-tests.xml"
        suite_key = (manifest_path, junit_path)
        if suite_key not in suites:
            suites[suite_key] = (load_json(manifest_path), ET.parse(junit_path).getroot())
        validate_suite(*suites[suite_key], receipt)
        found[identity] = (str(path), digest)
    missing = required - set(found)
    require(not missing, "missing native evidence: " + ", ".join(map(str, sorted(missing))))
    # Both linkage variants must exercise exactly the same fixture in a build.
    inputs = {}
    for identity, (path, _) in found.items():
        fixture = load_json(Path(path))["scenes"][0]["input_fingerprint"]
        require(identity[:3] not in inputs or inputs[identity[:3]] == fixture, "linkage input fingerprints differ")
        inputs[identity[:3]] = fixture
    return {"schema": 1, "status": "passed", "source_fingerprint": fingerprint,
            "required": [list(row) for row in sorted(required)],
            "receipts": [{"identity": list(key), "path": found[key][0]} for key in sorted(required)]}


def native_platform():
    system = {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}.get(platform.system())
    architecture = {"AMD64": "x86_64", "x86_64": "x86_64", "arm64": "aarch64", "aarch64": "aarch64"}.get(platform.machine())
    name = str(system) + "-" + str(architecture)
    require(name in PLATFORMS, "unknown native audit host")
    return name


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("roots", nargs="+", type=Path, help="extracted artifact directories")
    parser.add_argument("--source", required=True, type=Path, help="source tree being accepted")
    parser.add_argument("--platform", action="append", choices=tuple(PLATFORMS) + ("native",))
    parser.add_argument("--configuration", action="append", choices=("Debug", "Release", "RelWithDebInfo", "MinSizeRel"))
    parser.add_argument("--reference", choices=("on", "off", "both"), default="both")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        platforms = {native_platform() if name == "native" else name for name in (args.platform or PLATFORMS)}
        references = (True, False) if args.reference == "both" else (args.reference == "on",)
        required = set(itertools.product(platforms, args.configuration or ("Debug", "Release"), references, LINKAGES))
        report = audit(args.roots, source_fingerprint(args.source.resolve()), required)
        output = json.dumps(report, indent=2) + "\n"
        if args.output:
            args.output.write_text(output, encoding="utf-8")
        sys.stdout.write(output)
        return 0
    except (InvalidEvidence, OSError, ValueError, TypeError, KeyError, ET.ParseError) as error:
        if args.output:
            try:
                args.output.write_text(json.dumps({"schema": 1, "status": "failed", "diagnostic": str(error)}) + "\n", encoding="utf-8")
            except OSError as write_error:
                print("Cannot record failed evidence audit: " + str(write_error), file=sys.stderr)
        print("Native evidence rejected: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

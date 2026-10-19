#!/usr/bin/env python3
"""Run platform acceptance and verify reports and target admission."""
import argparse
from pathlib import Path
import subprocess

from AuditAcceptance import load_json, validate_receipt, marker
from SourceFingerprint import source_fingerprint

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--run", required=True)
parser.add_argument("--report", type=Path, required=True)
parser.add_argument("--configuration", required=True)
parser.add_argument("--linkage", required=True)
parser.add_argument("--expected", default="")
parser.add_argument("--source", type=Path)
parser.add_argument("--manifest", type=Path)
args = parser.parse_args()
fingerprint = source_fingerprint(args.source) if args.source else ""
if args.source:
    if not args.manifest or load_json(args.manifest).get("source_fingerprint") != fingerprint:
        raise RuntimeError("Native acceptance sources changed; rebuild before testing")
command = [args.run, "--report", str(args.report)]
if args.expected:
    command += ["--expected", args.expected]
result = subprocess.run(command, capture_output=True, text=True)
if result.returncode:
    raise RuntimeError(f"Native acceptance failed ({result.returncode}): {result.stdout}\n{result.stderr}")
receipt = load_json(args.report)
validate_receipt(receipt, fingerprint, allow_unconfigured=args.source is None)
if receipt["configuration"] != args.configuration or receipt["linkage"] != args.linkage:
    raise RuntimeError("Native acceptance report has the wrong build identity")
wrong = "x86_64-sysv" if receipt["target"] == "aarch64-macos" else "aarch64-macos"
negative = args.report.with_name(args.report.name + ".mismatch.json")
negative.write_text(args.report.read_text(encoding="utf-8"), encoding="utf-8")
result = subprocess.run([args.run, "--report", str(negative), "--expected", wrong], capture_output=True, text=True)
if result.returncode != 2:
    raise RuntimeError(f"Wrong-target admission did not return a diagnostic: {result}")
rejected = load_json(negative)
if rejected["status"] != "failed" or rejected["completed_scenes"] != 0 or "expected target" not in rejected["diagnostic"]:
    raise RuntimeError(f"Wrong-target admission left a misleading report: {rejected}")
print("Native acceptance completed 21 scenes and rejected the mismatched target")
print(marker(receipt))

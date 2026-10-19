#!/usr/bin/env python3
"""Synthetic artifact checks, separate from actual native platform acceptance."""
import contextlib
import copy
import io
import itertools
import json
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET
from unittest.mock import patch

import AuditAcceptance as audit

DIGEST = "a" * 64


def receipt(linkage="static", configuration="Debug"):
    scenes = []
    for mode, level in sorted(audit.SCENES):
        native = not mode.endswith("interpreter")
        scenes.append(dict(mode=mode, level=level, checked_calls=5, raw_calls=1,
                           preparations=int(native), legalizations=int(native), frames=int(native),
                           encoded_blocks=int(mode == "bbv"), adapter_preparations=1,
                           adapter_legalizations=1, adapter_frames=1, input_fingerprint=42))
    return dict(schema=3, status="passed", source_fingerprint=DIGEST, configuration=configuration,
                linkage=linkage, host_system="macos", host_architecture="aarch64",
                target="aarch64-macos", expected_target="aarch64-macos", translated=False,
                diagnostic="", completed_scenes=21, scenes=scenes)


def manifest(r):
    return dict(schema=2, **{key: r[key] for key in audit.IDENTITY_FIELDS}, tests=[
        "mirnext-native-host", "mirnext-unified-architecture", "mirnext-unified-linker",
        "mirnext-native-acceptance-" + r["linkage"]])


def junit(r):
    root = ET.Element("testsuite", tests="4", failures="0", errors="0", disabled="0", skipped="0")
    for name in manifest(r)["tests"]:
        case = ET.SubElement(root, "testcase", name=name, status="run")
        if name.startswith("mirnext-native-acceptance-"):
            identity = dict(r, linkage=name.rsplit("-", 1)[1])
            ET.SubElement(case, "system-out").text = audit.marker(identity)
    return root


class ReceiptChecks(unittest.TestCase):
    def test_all_real_host_abi_combinations(self):
        for name, (system, architecture, target) in audit.PLATFORMS.items():
            r = receipt()
            r.update(host_system=system, host_architecture=architecture, target=target, expected_target=target)
            self.assertEqual(audit.validate_receipt(r, DIGEST), (name, "Debug", "static"))

    def test_identity_and_admission_failures(self):
        mutations = dict(schema=1, status="running", source_fingerprint="b" * 64,
                         configuration="Unknown", linkage="unknown",
                         translated=True, diagnostic="failure", host_system="linux",
                         host_architecture="x86_64", expected_target="x86_64-sysv", completed_scenes=20)
        for field, value in mutations.items():
            with self.subTest(field=field):
                r = receipt()
                r[field] = value
                with self.assertRaises(audit.InvalidEvidence):
                    audit.validate_receipt(r, DIGEST)

    def test_plain_tests_allow_an_empty_configuration_but_evidence_does_not(self):
        r = receipt(configuration="")
        with self.assertRaisesRegex(audit.InvalidEvidence, "configuration"):
            audit.validate_receipt(r, DIGEST)
        audit.validate_receipt(r, DIGEST, allow_unconfigured=True)
        r["configuration"] = "Unknown"
        with self.assertRaisesRegex(audit.InvalidEvidence, "configuration"):
            audit.validate_receipt(r, DIGEST, allow_unconfigured=True)

    def test_unsupported_receipt_and_manifest_schemas_are_rejected(self):
        r = receipt()
        r.update(schema=2)
        with self.assertRaisesRegex(audit.InvalidEvidence, "receipt schema"):
            audit.validate_receipt(r, DIGEST)
        r = receipt()
        old = manifest(r)
        old.update(schema=1)
        with self.assertRaisesRegex(audit.InvalidEvidence, "test manifest"):
            audit.validate_suite(old, junit(r), r)

    def test_scene_coverage_bits_and_machine_stages(self):
        for field, value in dict(level=True, checked_calls=4, raw_calls=0, preparations=-1,
                                 legalizations=0, frames=0, encoded_blocks=0,
                                 adapter_preparations=0, adapter_legalizations=0,
                                 adapter_frames=0, input_fingerprint=43).items():
            with self.subTest(field=field):
                r = receipt()
                next(s for s in r["scenes"] if s["mode"] == "bbv")[field] = value
                with self.assertRaises(audit.InvalidEvidence):
                    audit.validate_receipt(r, DIGEST)
        r = receipt()
        r["scenes"][-1] = copy.deepcopy(r["scenes"][0])
        with self.assertRaisesRegex(audit.InvalidEvidence, "duplicate scene"):
            audit.validate_receipt(r, DIGEST)
        r = receipt()
        next(s for s in r["scenes"] if s["mode"] == "original-interpreter")["encoded_blocks"] = 1
        with self.assertRaisesRegex(audit.InvalidEvidence, "interpreter"):
            audit.validate_receipt(r, DIGEST)

    def test_separate_build_suites_reject_the_wrong_linkage(self):
        for linkage in audit.LINKAGES:
            r = receipt(linkage)
            audit.validate_suite(manifest(r), junit(r), r)
            other = receipt("shared" if linkage == "static" else "static")
            with self.assertRaisesRegex(audit.InvalidEvidence, "omits native"):
                audit.validate_suite(manifest(other), junit(other), r)

    def test_suite_requires_exact_manifest_and_matching_receipt(self):
        r = receipt()
        audit.validate_suite(manifest(r), junit(r), r)
        for field in ("failures", "errors", "disabled", "skipped"):
            root = junit(r)
            root.set(field, "1")
            with self.assertRaises(audit.InvalidEvidence):
                audit.validate_suite(manifest(r), root, r)
        for mode in ("missing", "duplicate", "failed", "skipped", "not-run", "stale"):
            with self.subTest(mode=mode):
                root = junit(r)
                if mode == "missing":
                    root.remove(root[0])
                elif mode == "duplicate":
                    root[0].set("name", root[1].get("name"))
                elif mode in ("failed", "skipped"):
                    ET.SubElement(root[0], "failure" if mode == "failed" else "skipped")
                elif mode == "not-run":
                    root[0].set("status", "notrun")
                else:
                    root[-1].find("system-out").text = "old receipt"
                with self.assertRaises(audit.InvalidEvidence):
                    audit.validate_suite(manifest(r), root, r)
        old = manifest(r)
        old["source_fingerprint"] = "b" * 64
        with self.assertRaises(audit.InvalidEvidence):
            audit.validate_suite(old, junit(r), r)


class ArtifactChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mirnext-audit-tests-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.required = set(itertools.product(("macos-aarch64",), ("Debug",), audit.LINKAGES))
        for linkage in audit.LINKAGES:
            r = receipt(linkage)
            directory = self.root / linkage
            directory.mkdir()
            self.write(linkage + "/native-acceptance-" + linkage + "-Debug.json", r)
            self.write(linkage + "/native-test-manifest-Debug.json", manifest(r))
            ET.ElementTree(junit(r)).write(directory / "native-tests.xml")

    def write(self, name, data):
        (self.root / name).write_text(json.dumps(data), encoding="utf-8")

    def test_complete_artifacts_and_negative_admission_receipt(self):
        self.write("native-acceptance-static-Debug.json.mismatch.json", dict(status="failed"))
        report = audit.audit([self.root], DIGEST, self.required)
        self.assertEqual(report["status"], "passed")
        self.assertEqual(len(report["receipts"]), 2)
        all_platforms = set(itertools.product(audit.PLATFORMS, ("Debug", "Release"), audit.LINKAGES))
        with self.assertRaisesRegex(audit.InvalidEvidence, "missing native evidence"):
            audit.audit([self.root], DIGEST, all_platforms)

    def test_missing_junit_manifest_linkage_and_cross_linkage_inputs(self):
        for name in ("static/native-tests.xml", "static/native-test-manifest-Debug.json", "shared/native-acceptance-shared-Debug.json"):
            path = self.root / name
            saved = path.read_bytes()
            path.unlink()
            with self.assertRaises((audit.InvalidEvidence, OSError)):
                audit.audit([self.root], DIGEST, self.required)
            path.write_bytes(saved)
        r = receipt("shared")
        for scene in r["scenes"]:
            scene["input_fingerprint"] = 43
        self.write("shared/native-acceptance-shared-Debug.json", r)
        with self.assertRaisesRegex(audit.InvalidEvidence, "linkage input"):
            audit.audit([self.root], DIGEST, self.required)

    def test_duplicate_json_fields_and_failed_audit_overwrites_old_pass(self):
        path = self.root / "static/native-acceptance-static-Debug.json"
        path.write_text('{"schema":2,"schema":1}')
        with self.assertRaisesRegex(audit.InvalidEvidence, "duplicate JSON field"):
            audit.load_json(path)
        output = self.root / "evidence.json"
        output.write_text('{"status":"passed"}')
        with patch.object(audit, "source_fingerprint", return_value=DIGEST), contextlib.redirect_stderr(io.StringIO()):
            result = audit.main([str(self.root), "--source", str(self.root), "--platform", "macos-aarch64",
                                 "--configuration", "Debug", "--output", str(output)])
        self.assertEqual(result, 1)
        self.assertEqual(json.loads(output.read_text())["status"], "failed")


class SourceChecks(unittest.TestCase):
    def test_line_endings_untracked_files_and_core_content_changes(self):
        with tempfile.TemporaryDirectory(prefix="mirnext-source-tests-") as work:
            root = Path(work)
            (root / "CMakeLists.txt").write_bytes(b"project(fixture)\n")
            (root / "LICENSE").write_text("fixture license\n")
            (root / "NOTICE").write_text("fixture attribution\n")
            (root / "lib/IR").mkdir(parents=True)
            source = root / "lib/IR/Test.cpp"
            source.write_bytes(b"int value;\nint other;\n")
            rules = root / "lib/CMakeLists.txt"
            rules.write_text("add_library(fixture IR/Test.cpp)\n")
            def fingerprint():
                return audit.source_fingerprint(root)
            original = fingerprint()
            source.write_bytes(b"int value;\r\nint other;\r\n")
            self.assertEqual(fingerprint(), original)
            added = root / "lib/IR/Untracked.cpp"
            added.write_text("int untracked;\n")
            self.assertNotEqual(fingerprint(), original)
            added.unlink()
            self.assertEqual(fingerprint(), original)
            source.write_text("int changed;\n")
            self.assertNotEqual(fingerprint(), original)
            source.write_bytes(b"int value;\nint other;\n")
            self.assertEqual(fingerprint(), original)
            rules.write_text("add_library(fixture SHARED IR/Test.cpp)\n")
            self.assertNotEqual(fingerprint(), original)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
RUNNER_PATH = ROOT / "tools" / "test262" / "run.py"
SPEC = importlib.util.spec_from_file_location("velune_test262", RUNNER_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class MetadataTests(unittest.TestCase):
    def test_inline_metadata(self):
        source = """/*---\nflags: [onlyStrict]\nincludes: [compareArray.js, propertyHelper.js]\nfeatures: [Symbol.iterator, BigInt]\nnegative:\n  phase: runtime\n  type: TypeError\n---*/\n42;\n"""
        metadata = MODULE.parse_metadata(source)
        self.assertEqual(metadata.flags, ("onlyStrict",))
        self.assertEqual(metadata.includes, ("compareArray.js", "propertyHelper.js"))
        self.assertEqual(metadata.features, ("Symbol.iterator", "BigInt"))
        self.assertEqual(metadata.negative_phase, "runtime")
        self.assertEqual(metadata.negative_type, "TypeError")

    def test_block_lists(self):
        source = """/*---\nflags:\n  - noStrict\nincludes:\n  - assert.js\nfeatures:\n  - generators\n---*/\n"""
        metadata = MODULE.parse_metadata(source)
        self.assertEqual(metadata.flags, ("noStrict",))
        self.assertEqual(metadata.includes, ("assert.js",))
        self.assertEqual(metadata.features, ("generators",))

    def test_variants(self):
        self.assertEqual([v.name for v in MODULE.variants_for(MODULE.Metadata())], ["sloppy", "strict"])
        self.assertEqual([v.name for v in MODULE.variants_for(MODULE.Metadata(flags=("onlyStrict",)))], ["strict"])
        self.assertEqual([v.name for v in MODULE.variants_for(MODULE.Metadata(flags=("noStrict",)))], ["sloppy"])
        self.assertEqual([v.name for v in MODULE.variants_for(MODULE.Metadata(flags=("module",)))], ["module"])
        self.assertEqual([v.name for v in MODULE.variants_for(MODULE.Metadata(flags=("raw",)))], ["raw"])


class CorpusLayoutTests(unittest.TestCase):
    def test_discover_tests_uses_concrete_test_root(self):
        with tempfile.TemporaryDirectory() as temp:
            test_root = pathlib.Path(temp) / "test"
            target = test_root / "language" / "expressions" / "array" / "sample.js"
            target.parent.mkdir(parents=True)
            target.write_text("1;\n", encoding="utf-8")

            direct = MODULE.discover_tests(test_root, ["language/expressions/array"])
            prefixed = MODULE.discover_tests(test_root, ["test/language/expressions/array"])
            self.assertEqual(direct, [target.resolve()])
            self.assertEqual(prefixed, [target.resolve()])

    def test_harness_source_uses_separate_harness_root(self):
        with tempfile.TemporaryDirectory() as temp:
            harness_root = pathlib.Path(temp) / "harness"
            harness_root.mkdir()
            (harness_root / "assert.js").write_text("// assert\n", encoding="utf-8")
            (harness_root / "sta.js").write_text("// sta\n", encoding="utf-8")
            (harness_root / "helper.js").write_text("// helper\n", encoding="utf-8")

            source, error = MODULE.harness_source(
                harness_root, MODULE.Metadata(includes=("helper.js",))
            )
            self.assertIsNone(error)
            self.assertEqual(source, "// assert\n\n// sta\n\n// helper\n\n")


class ClassificationTests(unittest.TestCase):
    def classify(self, returncode: int, metadata: MODULE.Metadata, stderr: str = ""):
        return MODULE.classify_process(
            returncode=returncode,
            stdout="",
            stderr=stderr,
            metadata=metadata,
        )

    def result(self, status: str, category: str, *, test: str = "test/language/foo/a.js", variant: str = "strict", features=()):
        return MODULE.Result(
            test=test,
            variant=variant,
            status=status,
            category=category,
            phase=MODULE.phase_for_category(category),
            duration_ms=1,
            features=features,
            failure_signature=MODULE.result_failure_signature(
                status=status, category=category, stdout="",
                stderr={
                    "unexpected_parse_error": "parse_error at 17:9: unexpected token '*'",
                    "unexpected_compile_error": "compile error at [120, 128): unsupported binding form",
                    "unexpected_runtime_error": "ReferenceError: missing name",
                }.get(category, ""),
            ),
        )

    def test_process_classification_normal_pass(self):
        status, category = self.classify(0, MODULE.Metadata())
        self.assertEqual((status, category), (MODULE.STATUS_PASS, "normal"))

    def test_expected_parse_error_passes(self):
        status, category = self.classify(
            2,
            MODULE.Metadata(negative_phase="parse", negative_type="SyntaxError"),
            "parse_error SyntaxError",
        )
        self.assertEqual((status, category), (MODULE.STATUS_PASS, "expected_parse_error"))
        result = self.result(status, category)
        self.assertEqual(MODULE.diagnostic_class(result), "PASS_EXPECTED_PARSE_ERROR")

    def test_missing_parse_early_error_is_distinct(self):
        status, category = self.classify(
            0,
            MODULE.Metadata(negative_phase="parse", negative_type="SyntaxError"),
        )
        result = self.result(status, category)
        self.assertEqual(MODULE.diagnostic_class(result), "FRONTEND_EARLY_ERROR_MISSING")
        self.assertEqual(result.phase, "parse")

    def test_positive_test_rejected_by_parser_is_frontend_failure(self):
        status, category = self.classify(2, MODULE.Metadata(), "parse_error")
        result = self.result(status, category)
        self.assertEqual(MODULE.diagnostic_class(result), "FRONTEND_PARSE_FAILURE")
        self.assertEqual(result.phase, "parse")

    def test_compile_failure_is_separate_from_runtime(self):
        status, category = self.classify(3, MODULE.Metadata(), "compile error")
        result = self.result(status, category)
        self.assertEqual(MODULE.diagnostic_class(result), "FRONTEND_COMPILE_FAILURE")
        self.assertEqual(result.phase, "compile")

    def test_runtime_failure_is_semantic_failure(self):
        status, category = self.classify(4, MODULE.Metadata(), "ReferenceError")
        result = self.result(status, category)
        self.assertEqual(MODULE.diagnostic_class(result), "RUNTIME_SEMANTIC_FAILURE")
        self.assertEqual(result.phase, "runtime")

    def test_diagnostic_summary_aggregates_useful_dimensions(self):
        results = [
            self.result(MODULE.STATUS_FAIL, "unexpected_parse_error", features=("destructuring-binding",)),
            self.result(MODULE.STATUS_FAIL, "unexpected_compile_error", test="test/language/foo/b.js", variant="sloppy"),
            self.result(MODULE.STATUS_PASS, "normal", test="test/language/bar/c.js", variant="sloppy"),
        ]
        summary = MODULE.diagnostic_summary(results)
        self.assertEqual(summary["failures_by_diagnostic_class"]["FRONTEND_PARSE_FAILURE"], 1)
        self.assertEqual(summary["failures_by_diagnostic_class"]["FRONTEND_COMPILE_FAILURE"], 1)
        self.assertEqual(summary["failures_by_phase"]["parse"], 1)
        self.assertEqual(summary["failures_by_phase"]["compile"], 1)
        self.assertEqual(summary["failures_by_directory"]["language/foo"], 2)
        self.assertEqual(summary["failures_by_feature"]["destructuring-binding"], 1)
        self.assertEqual(summary["failures_by_feature"]["<none>"], 1)


    def test_failure_signature_removes_parse_location(self):
        signature = MODULE.normalize_failure_signature(
            "parse_error at 17:9: unexpected token '*'", "unexpected_parse_error"
        )
        self.assertEqual(signature, "unexpected token '*'")

    def test_failure_signature_removes_compile_range(self):
        signature = MODULE.normalize_failure_signature(
            "compile error at [120, 128): unsupported binding form", "unexpected_compile_error"
        )
        self.assertEqual(signature, "unsupported binding form")

    def test_failure_signature_aggregates_same_parser_gap(self):
        a = self.result(MODULE.STATUS_FAIL, "unexpected_parse_error")
        b = self.result(MODULE.STATUS_FAIL, "unexpected_parse_error", test="test/language/foo/b.js", variant="sloppy")
        b.failure_signature = MODULE.result_failure_signature(
            status=MODULE.STATUS_FAIL, category="unexpected_parse_error", stdout="",
            stderr="parse_error at 99:2: unexpected token '*'",
        )
        summary = MODULE.diagnostic_summary([a, b])
        self.assertEqual(summary["failures_by_signature"]["unexpected token '*'"], 2)

if __name__ == "__main__":
    unittest.main()

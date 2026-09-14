#!/usr/bin/env python3
"""Velune Test262 runner.

Dependency-free runner for deterministic conformance baselines. It understands
Test262 frontmatter sufficiently for script tests and explicitly classifies
host capabilities that Velune has not enabled yet.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import datetime as dt
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile
import time
from collections import Counter, defaultdict
from typing import Iterable

FRONTMATTER_RE = re.compile(r"/\*---\s*(.*?)\s*---\*/", re.DOTALL)
INLINE_LIST_RE = re.compile(r"^\[(.*)\]$")
CRASH_SIGNALS = {-4, -6, -7, -8, -9, -11}

STATUS_PASS = "PASS"
STATUS_FAIL = "FAIL"
STATUS_SKIP = "SKIP_UNSUPPORTED"
STATUS_TIMEOUT = "TIMEOUT"
STATUS_CRASH = "CRASH"
STATUS_HARNESS = "HARNESS_ERROR"


@dataclasses.dataclass(frozen=True)
class Metadata:
    flags: tuple[str, ...] = ()
    includes: tuple[str, ...] = ()
    features: tuple[str, ...] = ()
    negative_phase: str | None = None
    negative_type: str | None = None


@dataclasses.dataclass(frozen=True)
class Variant:
    name: str
    strict: bool


@dataclasses.dataclass
class Result:
    test: str
    variant: str
    status: str
    category: str
    phase: str
    duration_ms: int
    returncode: int | None = None
    expected_error: str | None = None
    stdout: str = ""
    stderr: str = ""
    features: tuple[str, ...] = ()
    failure_signature: str | None = None

    def as_dict(self) -> dict:
        return dataclasses.asdict(self)


def parse_scalar(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {'"', "'"}:
        return value[1:-1]
    return value


def parse_inline_list(value: str) -> list[str]:
    match = INLINE_LIST_RE.match(value.strip())
    if not match:
        return []
    body = match.group(1).strip()
    if not body:
        return []
    # Test262 metadata items do not use embedded commas in these fields.
    return [parse_scalar(item.strip()) for item in body.split(",") if item.strip()]


def parse_metadata(source: str) -> Metadata:
    match = FRONTMATTER_RE.search(source)
    if not match:
        return Metadata()

    lines = match.group(1).splitlines()
    values: dict[str, object] = {}
    current_list: str | None = None
    current_map: str | None = None

    for raw in lines:
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        line = raw.strip()

        if indent == 0 and ":" in line:
            key, value = line.split(":", 1)
            key = key.strip()
            value = value.strip()
            current_list = None
            current_map = None
            if value:
                inline = parse_inline_list(value)
                values[key] = inline if INLINE_LIST_RE.match(value) else parse_scalar(value)
            else:
                values[key] = [] if key in {"flags", "includes", "features"} else {}
                if key in {"flags", "includes", "features"}:
                    current_list = key
                else:
                    current_map = key
            continue

        if indent > 0 and line.startswith("-") and current_list:
            item = parse_scalar(line[1:].strip())
            cast = values.setdefault(current_list, [])
            if isinstance(cast, list):
                cast.append(item)
            continue

        if indent > 0 and ":" in line and current_map:
            key, value = line.split(":", 1)
            cast = values.setdefault(current_map, {})
            if isinstance(cast, dict):
                cast[key.strip()] = parse_scalar(value.strip())

    negative = values.get("negative", {})
    if not isinstance(negative, dict):
        negative = {}

    def tuple_field(name: str) -> tuple[str, ...]:
        value = values.get(name, [])
        if isinstance(value, list):
            return tuple(str(item) for item in value)
        return ()

    return Metadata(
        flags=tuple_field("flags"),
        includes=tuple_field("includes"),
        features=tuple_field("features"),
        negative_phase=str(negative.get("phase")) if negative.get("phase") else None,
        negative_type=str(negative.get("type")) if negative.get("type") else None,
    )


def variants_for(metadata: Metadata) -> list[Variant]:
    flags = set(metadata.flags)
    if "module" in flags:
        return [Variant("module", True)]
    if "raw" in flags:
        return [Variant("raw", False)]
    if "onlyStrict" in flags:
        return [Variant("strict", True)]
    if "noStrict" in flags:
        return [Variant("sloppy", False)]
    return [Variant("sloppy", False), Variant("strict", True)]


def load_json(path: pathlib.Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def read_revision(path: pathlib.Path) -> str:
    revision = path.read_text(encoding="utf-8").strip()
    if len(revision) != 40:
        raise SystemExit(f"invalid Test262 revision in {path}")
    return revision


def git_revision(root: pathlib.Path) -> str | None:
    try:
        completed = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=root, check=True,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
        )
        return completed.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def discover_tests(test_root: pathlib.Path, requested: list[str]) -> list[pathlib.Path]:
    """Discover tests under a concrete Test262 test/ directory.

    Paths are relative to ``test_root``. For convenience, callers may still
    spell paths with the conventional ``test/`` prefix.
    """
    if not requested:
        requested = ["."]

    found: set[pathlib.Path] = set()
    for raw in requested:
        candidate = pathlib.Path(raw)
        if not candidate.is_absolute():
            parts = candidate.parts
            if parts and parts[0] == "test":
                candidate = pathlib.Path(*parts[1:]) if len(parts) > 1 else pathlib.Path(".")
            candidate = test_root / candidate
        candidate = candidate.resolve()
        if candidate.is_file() and candidate.suffix == ".js":
            found.add(candidate)
        elif candidate.is_dir():
            found.update(path.resolve() for path in candidate.rglob("*.js"))
        else:
            raise SystemExit(f"Test262 path not found: {raw}")
    return sorted(found)


def harness_source(harness_root: pathlib.Path, metadata: Metadata) -> tuple[str | None, str | None]:
    if "raw" in metadata.flags:
        return "", None

    names = ["assert.js", "sta.js", *metadata.includes]
    chunks: list[str] = []
    for name in names:
        path = harness_root / name
        if not path.is_file():
            return None, f"missing harness include: {name}"
        chunks.append(path.read_text(encoding="utf-8"))
    return "\n".join(chunks) + "\n", None


def unsupported_reason(metadata: Metadata, capabilities: dict) -> str | None:
    flags = set(metadata.flags)
    if "module" in flags and not capabilities.get("modules", False):
        return "module_host_not_enabled"
    if "async" in flags and not capabilities.get("async", False):
        return "async_host_not_enabled"

    explicitly_skipped = set(capabilities.get("skip_features", []))
    hit = sorted(explicitly_skipped.intersection(metadata.features))
    if hit:
        return "feature_disabled:" + ",".join(hit)

    supported = set(capabilities.get("supported_features", []))
    # An empty supported_features list means "do not feature-gate". This keeps
    # early baselines useful while the feature manifest is still being built.
    if supported:
        unknown = sorted(set(metadata.features) - supported)
        if unknown:
            return "feature_unsupported:" + ",".join(unknown)
    return None


def error_type_matches(text: str, expected: str | None) -> bool:
    if not expected:
        return True
    normalized = text.lower().replace("_", "")
    return expected.lower().replace("_", "") in normalized


def phase_for_category(category: str) -> str:
    if category.startswith("expected_parse") or category.startswith("wrong_parse") or category.startswith("unexpected_parse"):
        return "parse"
    if category.startswith("expected_early") or category.startswith("wrong_early"):
        return "early"
    if category.startswith("expected_resolution") or category.startswith("wrong_resolution"):
        return "resolution"
    if category.startswith("unexpected_compile"):
        return "compile"
    if category.startswith("expected_runtime") or category.startswith("wrong_runtime") or category.startswith("unexpected_runtime"):
        return "runtime"
    if category in {"normal", "engine_process_error"}:
        return "runtime"
    if category.startswith("module_host") or category.startswith("async_host") or category.startswith("feature_"):
        return "host"
    if category.startswith("missing harness") or category.startswith("cannot_execute_engine"):
        return "harness"
    if category == "execution_timeout":
        return "runtime"
    if category == "process_crash":
        return "engine"
    return "unknown"


def diagnostic_class(result: Result) -> str:
    if result.status == STATUS_PASS:
        if result.category.startswith("expected_parse"):
            return "PASS_EXPECTED_PARSE_ERROR"
        if result.category.startswith("expected_early"):
            return "PASS_EXPECTED_EARLY_ERROR"
        if result.category.startswith("expected_resolution"):
            return "PASS_EXPECTED_RESOLUTION_ERROR"
        if result.category.startswith("expected_runtime"):
            return "PASS_EXPECTED_RUNTIME_ERROR"
        return "PASS_NORMAL"
    if result.status == STATUS_SKIP:
        return "HOST_UNSUPPORTED"
    if result.status == STATUS_TIMEOUT:
        return "TIMEOUT"
    if result.status == STATUS_CRASH:
        return "CRASH"
    if result.status == STATUS_HARNESS:
        return "HARNESS_ERROR"

    mapping = {
        "expected_parse_error_but_parsed": "FRONTEND_EARLY_ERROR_MISSING",
        "wrong_parse_error": "FRONTEND_PARSE_ERROR_TYPE_MISMATCH",
        "expected_early_error_but_succeeded": "FRONTEND_EARLY_ERROR_MISSING",
        "wrong_early_error": "FRONTEND_EARLY_ERROR_MISMATCH",
        "expected_resolution_error_but_succeeded": "FRONTEND_RESOLUTION_ERROR_MISSING",
        "wrong_resolution_error": "FRONTEND_RESOLUTION_ERROR_MISMATCH",
        "unexpected_parse_error": "FRONTEND_PARSE_FAILURE",
        "unexpected_compile_error": "FRONTEND_COMPILE_FAILURE",
        "expected_runtime_error_but_succeeded": "RUNTIME_EXPECTED_ERROR_MISSING",
        "wrong_runtime_error": "RUNTIME_ERROR_TYPE_MISMATCH",
        "unexpected_runtime_error": "RUNTIME_SEMANTIC_FAILURE",
        "engine_process_error": "ENGINE_PROCESS_FAILURE",
    }
    return mapping.get(result.category, "UNCLASSIFIED_FAILURE")



ANSI_ESCAPE_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
PARSE_LOCATION_RE = re.compile(r"^\s*parse_error\s+at\s+\d+:\d+\s*:\s*", re.IGNORECASE)
COMPILE_LOCATION_RE = re.compile(r"^\s*(?:error\([^)]*\)\s*:\s*)?compile error at \[\d+,\s*\d+\)\s*:\s*", re.IGNORECASE)
SOURCE_LOCATION_RE = re.compile(r"(?<![A-Za-z0-9_])(?:line\s+)?\d+:\d+(?![A-Za-z0-9_])", re.IGNORECASE)
TEMP_TEST_PATH_RE = re.compile(r"(?:[A-Za-z]:)?[/\\][^\s:]*velune-test262-[^\s:]*\.js")
HEX_ADDRESS_RE = re.compile(r"\b0x[0-9a-fA-F]{6,}\b")
WHITESPACE_RE = re.compile(r"\s+")


def normalize_failure_signature(text: str, category: str) -> str:
    """Return a stable diagnostic signature without volatile locations/paths."""
    text = ANSI_ESCAPE_RE.sub("", text or "").replace("\r\n", "\n").replace("\r", "\n")
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if not lines:
        return category
    signature = lines[0]
    signature = TEMP_TEST_PATH_RE.sub("<test>", signature)
    signature = PARSE_LOCATION_RE.sub("", signature)
    signature = COMPILE_LOCATION_RE.sub("", signature)
    signature = SOURCE_LOCATION_RE.sub("<loc>", signature)
    signature = HEX_ADDRESS_RE.sub("<addr>", signature)
    signature = WHITESPACE_RE.sub(" ", signature).strip()
    return signature[:240] if signature else category


def result_failure_signature(*, status: str, category: str, stdout: str, stderr: str) -> str | None:
    if status != STATUS_FAIL:
        return None
    combined = stderr.strip() or stdout.strip()
    return normalize_failure_signature(combined, category)

def directory_bucket(test: str) -> str:
    path = pathlib.PurePosixPath(test)
    parts = path.parts
    if parts and parts[0] == "test":
        parts = parts[1:]
    if len(parts) <= 1:
        return "."
    return "/".join(parts[:-1])


def compact_counter(counter: Counter) -> dict[str, int]:
    return dict(sorted(counter.items(), key=lambda item: (-item[1], item[0])))


def diagnostic_summary(results: list[Result]) -> dict:
    by_class = Counter()
    by_phase = Counter()
    by_variant = Counter()
    by_directory = Counter()
    by_feature = Counter()
    failures_by_class = Counter()
    failures_by_phase = Counter()
    failures_by_variant = Counter()
    failures_by_directory = Counter()
    failures_by_feature = Counter()
    failures_by_signature = Counter()

    for result in results:
        cls = diagnostic_class(result)
        by_class[cls] += 1
        by_phase[result.phase] += 1
        by_variant[result.variant] += 1
        directory = directory_bucket(result.test)
        by_directory[directory] += 1
        features = result.features or ("<none>",)
        for feature in features:
            by_feature[feature] += 1

        if result.status == STATUS_FAIL:
            failures_by_class[cls] += 1
            failures_by_phase[result.phase] += 1
            failures_by_variant[result.variant] += 1
            failures_by_directory[directory] += 1
            for feature in features:
                failures_by_feature[feature] += 1
            if result.failure_signature:
                failures_by_signature[result.failure_signature] += 1

    return {
        "by_diagnostic_class": compact_counter(by_class),
        "by_phase": compact_counter(by_phase),
        "by_variant": compact_counter(by_variant),
        "by_directory": compact_counter(by_directory),
        "by_feature": compact_counter(by_feature),
        "failures_by_diagnostic_class": compact_counter(failures_by_class),
        "failures_by_phase": compact_counter(failures_by_phase),
        "failures_by_variant": compact_counter(failures_by_variant),
        "failures_by_directory": compact_counter(failures_by_directory),
        "failures_by_feature": compact_counter(failures_by_feature),
        "failures_by_signature": compact_counter(failures_by_signature),
    }


def print_diagnostic_summary(diagnostics: dict) -> None:
    sections = [
        ("Failure classes", "failures_by_diagnostic_class", 20),
        ("Failure phases", "failures_by_phase", 20),
        ("Failure variants", "failures_by_variant", 20),
        ("Top failing directories", "failures_by_directory", 15),
        ("Top failing features", "failures_by_feature", 15),
        ("Top failure signatures", "failures_by_signature", 20),
    ]
    for title, key, limit in sections:
        values = diagnostics.get(key, {})
        if not values:
            continue
        print(f"\n{title}")
        print("-" * len(title))
        for name, count in list(values.items())[:limit]:
            print(f"{name:40} {count}")


def classify_process(
    *,
    returncode: int,
    stdout: str,
    stderr: str,
    metadata: Metadata,
) -> tuple[str, str]:
    negative_phase = metadata.negative_phase
    expected = metadata.negative_type
    combined = f"{stdout}\n{stderr}"

    if returncode in CRASH_SIGNALS or returncode < 0:
        return STATUS_CRASH, "process_crash"

    if negative_phase == "parse":
        if returncode == 2 and (expected in {None, "SyntaxError"} or error_type_matches(combined, expected)):
            return STATUS_PASS, "expected_parse_error"
        if returncode == 0:
            return STATUS_FAIL, "expected_parse_error_but_parsed"
        return STATUS_FAIL, "wrong_parse_error"

    if negative_phase in {"early", "resolution"}:
        # Velune currently reports parser/compile errors via exit 2/3. P0 keeps
        # these phases distinct in output even before P1/P4 refine error types.
        if returncode in {2, 3} and (expected in {None, "SyntaxError"} or error_type_matches(combined, expected)):
            return STATUS_PASS, f"expected_{negative_phase}_error"
        if returncode == 0:
            return STATUS_FAIL, f"expected_{negative_phase}_error_but_succeeded"
        return STATUS_FAIL, f"wrong_{negative_phase}_error"

    if negative_phase == "runtime":
        if returncode != 0 and error_type_matches(combined, expected):
            return STATUS_PASS, "expected_runtime_error"
        if returncode == 0:
            return STATUS_FAIL, "expected_runtime_error_but_succeeded"
        return STATUS_FAIL, "wrong_runtime_error"

    if returncode == 0:
        return STATUS_PASS, "normal"
    if returncode == 2:
        return STATUS_FAIL, "unexpected_parse_error"
    if returncode == 3:
        return STATUS_FAIL, "unexpected_compile_error"
    if returncode == 4:
        return STATUS_FAIL, "unexpected_runtime_error"
    return STATUS_FAIL, "engine_process_error"


def run_one(
    *,
    test_root: pathlib.Path,
    harness_root: pathlib.Path,
    engine: pathlib.Path,
    test: pathlib.Path,
    variant: Variant,
    metadata: Metadata,
    source: str,
    capabilities: dict,
    timeout: float,
    temp_root: pathlib.Path,
) -> Result:
    relative = "test/" + test.relative_to(test_root).as_posix()
    reason = unsupported_reason(metadata, capabilities)
    if reason:
        return Result(relative, variant.name, STATUS_SKIP, reason, "host", 0, features=metadata.features)

    parse_like = metadata.negative_phase in {"parse", "early", "resolution"}
    if parse_like:
        harness = ""
        harness_error = None
    else:
        harness, harness_error = harness_source(harness_root, metadata)
    if harness_error:
        return Result(relative, variant.name, STATUS_HARNESS, harness_error, "harness", 0, features=metadata.features)

    strict_prefix = '"use strict";\n' if variant.strict and variant.name != "module" else ""
    combined = strict_prefix + (harness or "") + source

    # Module execution requires a module-aware host entrypoint. Keep this branch
    # explicit so enabling modules later does not overload normal script mode.
    if variant.name == "module":
        return Result(relative, variant.name, STATUS_SKIP, "module_host_not_implemented", "host", 0, features=metadata.features)

    safe_name = re.sub(r"[^A-Za-z0-9_.-]+", "_", relative + "__" + variant.name)
    temp_path = temp_root / f"{safe_name}.js"
    temp_path.parent.mkdir(parents=True, exist_ok=True)
    temp_path.write_text(combined, encoding="utf-8")

    started = time.monotonic()
    try:
        command = [str(engine), str(temp_path)]
        if metadata.negative_phase == "parse":
            command = [str(engine), "--parse-only", str(temp_path)]
        elif metadata.negative_phase in {"early", "resolution"}:
            command = [str(engine), "--compile-only", str(temp_path)]
        completed = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        duration = int((time.monotonic() - started) * 1000)
        return Result(
            relative, variant.name, STATUS_TIMEOUT, "execution_timeout", "runtime", duration,
            stdout=exc.stdout or "", stderr=exc.stderr or "", features=metadata.features,
        )
    except OSError as exc:
        return Result(relative, variant.name, STATUS_HARNESS, f"cannot_execute_engine:{exc}", "harness", 0, features=metadata.features)

    duration = int((time.monotonic() - started) * 1000)
    status, category = classify_process(
        returncode=completed.returncode,
        stdout=completed.stdout,
        stderr=completed.stderr,
        metadata=metadata,
    )
    return Result(
        relative,
        variant.name,
        status,
        category,
        phase_for_category(category),
        duration,
        returncode=completed.returncode,
        expected_error=metadata.negative_type,
        stdout=completed.stdout[-4000:],
        stderr=completed.stderr[-4000:],
        features=metadata.features,
        failure_signature=result_failure_signature(
            status=status, category=category, stdout=completed.stdout, stderr=completed.stderr,
        ),
    )


def print_progress(result: Result) -> None:
    print(f"[{result.status:16}] {result.test} ({result.variant}) {diagnostic_class(result)} / {result.category}")


def summarize(results: list[Result]) -> dict[str, int]:
    counts = Counter(result.status for result in results)
    return {status: counts.get(status, 0) for status in [STATUS_PASS, STATUS_FAIL, STATUS_SKIP, STATUS_TIMEOUT, STATUS_CRASH, STATUS_HARNESS]}


def outcome_map(results: list[Result]) -> dict[str, str]:
    return {f"{result.test}::{result.variant}": result.status for result in results}


def compare_outcomes(previous: dict, current: list[Result]) -> dict[str, list[str]]:
    old = previous.get("outcomes", {}) if isinstance(previous, dict) else {}
    now = outcome_map(current)
    changes = {"new_pass": [], "new_fail": [], "new_crash": [], "new_timeout": [], "changed": []}
    for key, status in now.items():
        before = old.get(key)
        if before is None or before == status:
            continue
        changes["changed"].append(f"{key}: {before} -> {status}")
        if status == STATUS_PASS:
            changes["new_pass"].append(key)
        elif status == STATUS_FAIL:
            changes["new_fail"].append(key)
        elif status == STATUS_CRASH:
            changes["new_crash"].append(key)
        elif status == STATUS_TIMEOUT:
            changes["new_timeout"].append(key)
    return changes


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description="Run Test262 against Velune")
    parser.add_argument("paths", nargs="*", help="Test262 files/directories, relative to test/")
    parser.add_argument("--engine", type=pathlib.Path, required=True, help="path to the Velune js executable")
    parser.add_argument(
        "--test-root", type=pathlib.Path,
        default=root / "tests" / "test262" / "test",
        help="directory containing the Test262 test/ corpus (default: tracked curated corpus)",
    )
    parser.add_argument(
        "--harness-root", type=pathlib.Path,
        default=root / "tests" / "test262" / "harness",
        help="directory containing Velune's committed Test262 harness files",
    )
    parser.add_argument(
        "--test262-root", type=pathlib.Path,
        help="compatibility shortcut for a full Test262 checkout; overrides --test-root and --harness-root",
    )
    parser.add_argument("--capabilities", type=pathlib.Path, default=root / "tests" / "test262" / "config" / "capabilities.json")
    parser.add_argument("--revision-file", type=pathlib.Path, default=root / "tests" / "test262" / "TEST262_REVISION")
    parser.add_argument("--timeout", type=float, default=5.0, help="per-test timeout in seconds")
    parser.add_argument("--jobs", type=int, default=max(1, min(8, os.cpu_count() or 1)))
    parser.add_argument("--limit", type=int, default=0, help="run only the first N discovered files")
    parser.add_argument("--report", type=pathlib.Path, default=root / "tests" / "test262" / "results" / "latest.json")
    parser.add_argument("--compare", type=pathlib.Path, help="compare outcomes with a previous report/baseline")
    parser.add_argument("--write-baseline", type=pathlib.Path, help="write current outcome map as a baseline")
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--fail-on-regression", action="store_true", help="nonzero exit for new fail/crash/timeout")
    args = parser.parse_args()

    engine = args.engine.resolve()
    if args.test262_root:
        upstream_root = args.test262_root.resolve()
        test_root = upstream_root / "test"
        harness_root = upstream_root / "harness"
    else:
        upstream_root = None
        test_root = args.test_root.resolve()
        harness_root = args.harness_root.resolve()

    if not engine.is_file():
        raise SystemExit(f"Velune executable not found: {engine}")
    if not test_root.is_dir():
        raise SystemExit(f"Test262 test root not found: {test_root}")
    if not harness_root.is_dir():
        raise SystemExit(
            f"Test262 harness root not found: {harness_root}"
        )

    expected_revision = read_revision(args.revision_file)

    # The tracked curated corpus and harness live inside the Velune repository,
    # so their Git HEAD is the Velune revision, not the Test262 revision. In
    # local curated mode, TEST262_REVISION is the authoritative provenance
    # marker. Only validate Git HEAD when the caller explicitly points at a
    # standalone upstream Test262 checkout via --test262-root.
    if upstream_root is not None:
        actual_revision = git_revision(upstream_root)
        if actual_revision and actual_revision != expected_revision:
            raise SystemExit(
                f"Test262 revision mismatch: expected {expected_revision}, got {actual_revision}"
            )

    capabilities = load_json(args.capabilities)
    tests = discover_tests(test_root, args.paths)
    if args.limit:
        tests = tests[: args.limit]

    work: list[tuple[pathlib.Path, Variant, Metadata, str]] = []
    for test in tests:
        source = test.read_text(encoding="utf-8-sig")
        metadata = parse_metadata(source)
        for variant in variants_for(metadata):
            work.append((test, variant, metadata, source))

    results: list[Result] = []
    args.report.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="velune-test262-") as temp:
        temp_root = pathlib.Path(temp)
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            futures = [
                pool.submit(
                    run_one,
                    test_root=test_root,
                    harness_root=harness_root,
                    engine=engine,
                    test=test,
                    variant=variant,
                    metadata=metadata,
                    source=source,
                    capabilities=capabilities,
                    timeout=args.timeout,
                    temp_root=temp_root,
                )
                for test, variant, metadata, source in work
            ]
            for future in concurrent.futures.as_completed(futures):
                result = future.result()
                results.append(result)
                if not args.quiet:
                    print_progress(result)

    results.sort(key=lambda item: (item.test, item.variant))
    summary = summarize(results)
    diagnostics = diagnostic_summary(results)
    report = {
        "schema_version": 3,
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "engine": str(engine),
        "engine_revision": git_revision(root),
        "test262_revision": expected_revision,
        "capabilities": capabilities,
        "summary": summary,
        "diagnostics": diagnostics,
        "outcomes": outcome_map(results),
        "results": [result.as_dict() for result in results],
    }
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print("\nTest262 summary")
    print("---------------")
    for key, count in summary.items():
        print(f"{key:16} {count}")
    print(f"report           {args.report}")
    print_diagnostic_summary(diagnostics)

    changes = None
    if args.compare:
        previous = load_json(args.compare)
        changes = compare_outcomes(previous, results)
        print("\nChanges")
        print("-------")
        for key in ["new_pass", "new_fail", "new_crash", "new_timeout"]:
            print(f"{key:16} {len(changes[key])}")

    if args.write_baseline:
        baseline = {
            "schema_version": 3,
            "test262_revision": expected_revision,
            "engine_revision": git_revision(root),
            "summary": summary,
            "diagnostics": diagnostics,
            "outcomes": outcome_map(results),
        }
        args.write_baseline.parent.mkdir(parents=True, exist_ok=True)
        args.write_baseline.write_text(json.dumps(baseline, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"baseline         {args.write_baseline}")

    if summary[STATUS_CRASH] or summary[STATUS_HARNESS]:
        return 2
    if args.fail_on_regression and changes:
        if changes["new_fail"] or changes["new_crash"] or changes["new_timeout"]:
            return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

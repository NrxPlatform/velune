# Velune Test262 integration

Test262 is Velune's primary ECMAScript semantic conformance oracle. The upstream
suite is not committed to this repository. Velune pins one exact upstream Git
revision in `TEST262_REVISION` and checks it out under `third_party/test262/`.

## Policy

- Test262 decides ECMAScript observable behavior.
- Velune unit tests cover engine invariants, internal components, and fast bug regressions.
- Raw Test262 reports are generated under `tests/test262/results/` and are ignored by Git.
- The tracked `expectations/baseline.json` is an explicit baseline, not an upstream test copy.
- A new `FAIL`, `CRASH`, or `TIMEOUT` relative to a chosen baseline is a regression candidate.
- Unsupported host modes/features are reported as `SKIP_UNSUPPORTED`, never silently passed.

## Fetch the pinned suite

From the Velune source root:

```bash
python3 tools/test262/setup.py
```

Or through CMake:

```bash
cmake --build ../build-velune-debug --target test262-setup
```

The setup command verifies that the checkout's `HEAD` exactly matches
`TEST262_REVISION`.

## Run

```bash
python3 tools/test262/run.py \
  --engine ../build-velune-debug/js \
  language/expressions
```

Useful focused forms:

```bash
# One Test262 file
python3 tools/test262/run.py \
  --engine ../build-velune-debug/js \
  language/types/number/S8.5_A2.1.js

# First 100 files under language/
python3 tools/test262/run.py \
  --engine ../build-velune-debug/js \
  --limit 100 \
  language

# Compare against a previous report
python3 tools/test262/run.py \
  --engine ../build-velune-debug/js \
  --compare tests/test262/results/previous.json \
  --fail-on-regression \
  language
```

CMake convenience targets are also available after configuring with the CLI:

```bash
cmake --build ../build-velune-debug --target test262-smoke
cmake --build ../build-velune-debug --target test262
```

## Current host capabilities

`config/capabilities.json` describes host capabilities separately from language
semantics. Module and asynchronous Test262 host protocols are initially disabled
and therefore classified as `SKIP_UNSUPPORTED`. They are enabled when their host
entry points are made production-correct; the runner already recognizes the
metadata and keeps those tests distinct from semantic failures.

An empty `supported_features` array intentionally means "do not feature-gate".
This lets early Velune baselines expose missing language semantics as failures.
Specific features may be placed in `skip_features` only when there is a deliberate
reason to exclude them.

## Result classes

- `PASS`
- `FAIL`
- `SKIP_UNSUPPORTED`
- `TIMEOUT`
- `CRASH`
- `HARNESS_ERROR`

Every result also carries both a low-level `category` and a stable diagnostic
`phase`. Reports use schema version 2 and include diagnostic summaries by:

- diagnostic class
- execution phase
- strict/sloppy/module variant
- Test262 directory
- Test262 feature metadata

The diagnostic classes intentionally distinguish failures that did not reach
runtime semantics from runtime semantic failures. Important examples are:

- `FRONTEND_PARSE_FAILURE`
- `FRONTEND_EARLY_ERROR_MISSING`
- `FRONTEND_PARSE_ERROR_TYPE_MISMATCH`
- `FRONTEND_COMPILE_FAILURE`
- `RUNTIME_EXPECTED_ERROR_MISSING`
- `RUNTIME_ERROR_TYPE_MISMATCH`
- `RUNTIME_SEMANTIC_FAILURE`
- `ENGINE_PROCESS_FAILURE`

The runner does not claim an unexpected parse rejection is "unsupported syntax"
unless that can be proven. It records the objective phase/class first; deeper
feature attribution can be added later without corrupting the baseline.

## Failure signatures

Schema v3 records `failure_signature` for failed tests and aggregates
`diagnostics.failures_by_signature`. Signatures remove volatile source locations,
temporary paths, ANSI escapes, and pointer-like addresses while preserving the
actual Velune diagnostic and quoted token.

The console summary prints the 20 most frequent signatures. Raw stdout/stderr
remain available per result for detailed investigation.

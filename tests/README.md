# Velune Test262 integration

Test262 is Velune's primary ECMAScript semantic conformance oracle. During the
P1-P17 construction plan, Velune tracks a deliberately curated subset of
Test262 directly in this repository so conformance work remains focused,
reproducible, and fast.

The exact upstream revision from which tracked tests and harness helpers are
copied is recorded in `TEST262_REVISION`.

## Repository layout

```text
tests/test262/
├── TEST262_REVISION
├── config/
│   └── capabilities.json
├── expectations/
│   └── baseline.json
├── harness/
│   ├── assert.js
│   ├── sta.js
│   └── ... helpers required by tracked tests
├── results/
│   └── latest.json
├── runner/
│   └── test_runner.py
├── targets/
│   └── language-expressions-array.md
└── test/
    └── language/
        └── expressions/
            └── array/
```

`tests/test262/test/` is the **active conformance corpus**. Test directories are
added incrementally as Velune closes each language area.

``tests/test262/harness/` is also tracked and contains the complete upstream
Test262 harness from the revision in `TEST262_REVISION`. Harness files are kept
verbatim and are not selectively added or locally reimplemented.

The normal curated workflow does **not** require downloading the complete
upstream Test262 repository and does not require running `tools/test262/setup.py`.

## Policy

- Test262 decides ECMAScript observable behavior.
- Velune unit/invariant tests protect engine internals, GC, bytecode validity,
  resource rules, and fast regressions.
- Curated Test262 tests and the complete pinned Test262 harness are committed together.
- `results/latest.json` is tracked so the current project state is visible.
- `expectations/baseline.json` is the minimum accepted regression baseline.
- A new `FAIL`, `CRASH`, or `TIMEOUT` relative to the baseline is a regression
  candidate.
- Unsupported host protocols are `SKIP_UNSUPPORTED`; they are never silently
  treated as passes.
- Do not modify Test262 test or harness semantics to make Velune pass.

## Current target

The first tracked target is:

```text
language/expressions/array
```

The current locked baseline contains 52 source files and 104 sloppy/strict
variants:

```text
PASS              30
FAIL              74
SKIP_UNSUPPORTED   0
TIMEOUT            0
CRASH              0
HARNESS_ERROR       0
```

Current failures are predominantly runtime-semantic failures, with a smaller
remaining parser set. See `targets/language-expressions-array.md` and
`results/latest.json` for the detailed snapshot.

## Build

The focused Test262 workflow only requires the Velune CLI target:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target js -j$(nproc)
```

## Run the curated corpus

From the Velune source root:

```bash
python3 tools/test262/run.py \
  --engine build/js
```

Run the current array target explicitly:

```bash
python3 tools/test262/run.py \
  --engine build/js \
  language/expressions/array
```

Run one tracked test file:

```bash
python3 tools/test262/run.py \
  --engine build/js \
  language/expressions/array/11.1.4-0.js
```

CMake convenience targets use the same committed corpus and harness:

```bash
cmake --build build --target test262-smoke
cmake --build build --target test262
```

## Regression baseline

Compare the current run with the tracked baseline:

```bash
python3 tools/test262/run.py \
  --engine build/js \
  --compare tests/test262/expectations/baseline.json \
  --fail-on-regression
```

After intentionally improving the accepted baseline:

```bash
python3 tools/test262/run.py \
  --engine build/js \
  --write-baseline tests/test262/expectations/baseline.json
```

Baseline changes should be reviewed like source changes. A worse result should
never be normalized by simply rewriting the baseline.

## Harness rules

For normal positive/runtime tests, the runner injects:

```text
assert.js
sta.js
<metadata includes...>
<test source>
```

For `negative: phase: parse`, `early`, or `resolution`, harness code is excluded
from validation so a harness syntax failure cannot create a false positive.

If a test names a helper in `includes:` that is missing from
`tests/test262/harness/`, the result is `HARNESS_ERROR`. Because the repository
tracks the complete pinned harness, a missing helper means the local harness
copy is incomplete or out of sync and should be restored from the pinned
Test262 revision rather than replaced with a local shim.

## Result classes

Top-level statuses are:

- `PASS`
- `FAIL`
- `SKIP_UNSUPPORTED`
- `TIMEOUT`
- `CRASH`
- `HARNESS_ERROR`

Reports use schema v3 and also aggregate stable diagnostic dimensions:

- diagnostic class
- phase
- strict/sloppy/module variant
- Test262 directory
- feature metadata
- normalized failure signature

Important failure classes include:

- `FRONTEND_PARSE_FAILURE`
- `FRONTEND_EARLY_ERROR_MISSING`
- `FRONTEND_PARSE_ERROR_TYPE_MISMATCH`
- `FRONTEND_COMPILE_FAILURE`
- `RUNTIME_EXPECTED_ERROR_MISSING`
- `RUNTIME_ERROR_TYPE_MISMATCH`
- `RUNTIME_SEMANTIC_FAILURE`
- `ENGINE_PROCESS_FAILURE`

Failure signatures remove volatile source locations, temporary paths, ANSI
escapes, and pointer-like addresses while retaining the meaningful diagnostic.
Raw stdout/stderr remain available per result.

## Growing the corpus

Add Test262 coverage one semantic target at a time:

1. copy the target directory from the pinned Test262 revision into
   `tests/test262/test/`;
2. keep the already-tracked full harness unchanged;
3. add/update a target document under `tests/test262/targets/`;
4. run the target and investigate root causes by failure signature;
5. commit the new `latest.json` and, only after review, advance the regression
   baseline.

The P10 strategy is **language first**. Broad built-in conformance is deferred
until the corresponding later construction phases.

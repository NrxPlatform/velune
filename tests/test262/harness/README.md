# Test262 harness

This directory contains the **complete Test262 harness** used by Velune's
tracked conformance corpus under `tests/test262/test/`.

All files in this directory must be copied **verbatim** from the exact upstream
Test262 revision recorded in `../TEST262_REVISION`. Do not reimplement, simplify,
or patch harness helpers locally to make Velune pass tests: the harness is part
of the conformance oracle.

The normal Velune Test262 workflow is intentionally self-contained:

```text
tests/test262/
├── harness/   # complete pinned Test262 harness
├── test/      # curated tracked Test262 tests
├── results/
├── expectations/
├── targets/
└── runner/
```

`tools/test262/run.py` uses this directory by default, so normal conformance
work does not require `third_party/test262/` or `tools/test262/setup.py`.

For ordinary positive/runtime tests, the runner loads `assert.js`, `sta.js`,
and every helper named by the test's `includes:` metadata. Raw tests follow the
Test262 `raw` flag semantics and do not receive the ordinary harness prelude.

If the pinned Test262 revision changes, replace the harness from that revision
as a unit and review the conformance baseline separately from engine changes.

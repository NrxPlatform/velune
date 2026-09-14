# Target: language/expressions/array

## Source

Upstream Test262 path:

```text
test/language/expressions/array
```

Pinned revision:

```text
14e8c908e54ae2e770e473bcacf536f8cb654929
```

The committed corpus currently contains 52 Test262 source files. With default
sloppy and strict variants this produces 104 executions.

## Accepted starting baseline

```text
PASS              30
FAIL              74
SKIP_UNSUPPORTED   0
TIMEOUT             0
CRASH               0
HARNESS_ERROR       0
```

Failure phases:

```text
runtime 64
parse   10
```

This baseline is intentionally not a compliance claim. It is the first trusted
post-bootstrap regression point after the mandatory `sta.js` and `assert.js`
harness files became parseable/compilable by Velune.

## Current failure shape

The dominant known signatures at baseline are:

```text
type_error: call target is not a function                                      30
uncaught_exception: uncaught JavaScript exception: "TypeError: right-hand side of instanceof is not callable" 16
Unexpected token in input                                                       10
reference_error: binding 'ReferenceError' is not defined                         8
reference_error: binding 'Function' is not defined                               6
reference_error: binding 'TypeError' is not defined                              2
type_error: property access target is not an object                              2
```

The transition from parser/compile bootstrap failures to runtime failures is
expected and desirable: most cases now reach executable semantics.

## P10 ownership

This target is used to close P10 core-language behavior around:

- array literal syntax and evaluation;
- elisions and element ordering;
- spread syntax/basic spread evaluation;
- ordinary expression interaction inside array literals;
- parser/early-error correctness required by these tests.

Some cases may expose dependencies whose complete semantics belong to later
locked phases, especially iterator/generator closure (P12) and broader built-in
intrinsics (P15). Those dependencies must be documented rather than hidden or
feature-skipped without justification.

## Gate for moving to the next language target

Before expanding the curated corpus, prefer to reach:

- no unexplained parser failures owned by P10;
- no crashes;
- no timeouts;
- no harness failures;
- no regressions in previously passing variants;
- every remaining failure mapped to a concrete semantic defect or a later locked phase.

## Run

```bash
python3 tools/test262/run.py \
  --engine build/js \
  language/expressions/array
```

Regression check:

```bash
python3 tools/test262/run.py \
  --engine build/js \
  --compare tests/test262/expectations/baseline.json \
  --fail-on-regression \
  language/expressions/array
```

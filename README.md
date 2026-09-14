# Velune

Velune is a small, embeddable ECMAScript engine written in modern C++.

The project aims to provide a compact, production-quality JavaScript runtime that can be embedded into native applications without requiring a browser environment, Node.js, or a JIT compiler.

> **Status:** Work in progress. Velune currently implements a substantial JavaScript subset but is not yet fully ECMAScript compliant.

## Features

Velune currently includes:

- JavaScript tokenizer and parser
- AST-to-bytecode compiler
- bytecode verifier
- stack-based virtual machine
- lexical scopes and closures
- objects, prototypes, arrays, and `this`
- functions and constructors
- basic classes
- exceptions with `try`, `catch`, and `finally`
- mark-sweep garbage collection
- iterator protocol foundation
- ECMAScript modules with live bindings
- Promises and a host-controlled job queue
- generators and resumable execution
- RegExp integration
- JavaScript coercion and comparison operations

## Architecture

```text
JavaScript Source
        ↓
Tokenizer / Parser
        ↓
AST
        ↓
Compiler
        ↓
Bytecode
        ↓
Stack VM
        ↓
Runtime
 ├── Values
 ├── Objects / Prototypes
 ├── Functions / Closures
 ├── Arrays
 ├── Modules
 ├── Promises / Jobs
 ├── Generators
 ├── RegExp
 └── Garbage Collector
```

## Goals

Velune is being developed toward a small production-ready embeddable ECMAScript engine with:

- ECMAScript conformance
- Test262 validation
- stable C/C++ embedding APIs
- configurable memory and execution limits
- host-controlled modules and asynchronous jobs
- minimal dependencies
- no mandatory JIT compiler

Velune focuses on the ECMAScript language and runtime itself. Platform functionality such as filesystems, networking, timers, and event loops belongs to the embedding application.

## Build

Velune requires a C++20 compiler and CMake.

```bash
# From the directory above the Velune source tree:
cmake -S . -B build-velune-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-velune-debug -j$(nproc)
ctest --test-dir build-velune-debug --output-on-failure
```

### Sanitizers

```bash
cmake -S . -B build-velune-asan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DJSENGINE_ENABLE_SANITIZERS=ON

cmake --build build-velune-asan -j$(nproc)
ctest --test-dir build-velune-asan --output-on-failure
```

## Test262

Velune uses Test262 as its primary ECMAScript semantic conformance oracle.
During active construction, a curated subset of Test262 tests and the exact
harness helpers they require are tracked directly under `tests/test262/`. This
keeps the normal conformance loop reproducible without downloading the complete
upstream suite.

The upstream revision used as the source of those files is pinned in
`tests/test262/TEST262_REVISION`.

Current tracked target:

```text
language/expressions/array
52 source files
104 sloppy/strict variants

PASS  30
FAIL  74
CRASH 0
TIMEOUT 0
HARNESS_ERROR 0
```

Run the committed conformance corpus with:

```bash
cmake --build build --target js -j$(nproc)
python3 tools/test262/run.py --engine build/js
```

Or focus on the current target:

```bash
python3 tools/test262/run.py \
    --engine build/js \
    language/expressions/array
```

`tests/test262/results/latest.json` records the current public snapshot and
`tests/test262/expectations/baseline.json` is the regression baseline. See
`tests/test262/README.md` for corpus growth, harness rules, result
classification, and baseline policy.

## Project Status

Velune is under active development and is currently in **P10 — Core Language
Completion** of the production construction plan. P1-P9 established the major
semantic substrate and function machinery; P10 is now using the language portion
of Test262 to close parser, compiler, and runtime-semantic gaps before moving to
later language/runtime phases.

The engine is not yet fully ECMAScript compliant. Existing implementations of
classes, iterators, generators, promises, modules, RegExp, and other runtime
features should be treated as active implementation areas until their planned
Test262 closure phases are complete.

Current conformance work deliberately starts with `test/language`, then expands
toward later built-in and exotic-object coverage as the construction plan
reaches those phases.

The production roadmap continues through classes, iterators/generators,
Promise/async, modules, built-ins/exotics, Test262 closure, and final embedding
and hardening work. Architecture documentation lives under `docs/`.

## Design

Velune favors a small implementation architecture with correct language semantics.

The initial production target is an interpreter-first engine. Correctness, conformance, predictable resource usage, and embedding simplicity take priority over JIT compilation and aggressive optimization.

## License

License to be determined.

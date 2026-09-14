# P2 — Realm and Execution Context Architecture

## Scope

P2 separates Runtime-wide ownership from Realm-specific ECMAScript state without implementing P3 Reference semantics or P4 Environment Records.

## Ownership

```text
Runtime
├── heap
├── persistent/bytecode roots
├── active VMs
├── job queue
├── module-environment GC roots
└── Realms
    └── Realm
        ├── global object
        ├── global binding table (temporary pre-P4 GlobalEnvironment storage)
        ├── Object.prototype
        ├── Array.prototype
        ├── Promise.prototype
        ├── RegExp.prototype
        └── builtin initialization state
```

A `Context` is permanently associated with one Realm. Realms are Runtime-owned so Realm identity remains valid for heap objects/functions and persistent cross-Realm references even after the embedding `Context` that created the Realm is destroyed.

## Heap Realm affinity

`HeapObject` and `HeapFunction` carry a `Realm*`. Bytecode function prototypes are created in the compiler Context's Realm; closures inherit the prototype function Realm. Generator suspension records retain both Realm and originating function identity.

Runtime ownership remains the validation boundary: heap Values may cross Context/Realm boundaries when both Realms belong to the same Runtime.

## Execution context

Each VM frame stores a compact `ExecutionContext`:

```cpp
struct ExecutionContext {
    Realm* realm;
    const HeapFunction* function;
    HeapModuleEnvironment* module_environment;
};
```

P4 will extend the execution model with lexical/variable/private environments. P2 deliberately does not invent those representations early.

Root script/module frames use the embedding Context Realm. Function frames switch to the callee function's Realm. Object/array construction bytecodes allocate using the current frame Realm.

## Native calls

Native functions also have Realm affinity. During a native callback, `Context` temporarily exposes the callee Realm as its active semantic Realm for allocations and builtin access, while the embedding-facing `Context::realm()` identity remains unchanged.

This permits same-VM cross-Realm re-entry without creating a second VM or Runtime.

## Intrinsics and globals

Global bindings and intrinsic prototype roots are no longer Runtime-global. Builtin initialization happens independently per Realm. Each Realm therefore has distinct global-object and intrinsic identities.

The current `globals_` map inside Realm is intentionally transitional storage until P4 introduces a real `GlobalEnvironment`.

## GC

Runtime GC marks Realm roots for every Runtime-owned Realm. Cross-Realm object references are ordinary heap edges and require no special tracing mechanism.

## P2 invariants

Tests cover:

- two Contexts under one Runtime have distinct Realm identities;
- global objects are distinct;
- `Object.prototype` is distinct;
- builtin namespaces are distinct;
- a function invoked from another Realm allocates objects in the function Realm;
- cross-Realm references survive GC safely;
- destroying an embedding Context does not invalidate Runtime-owned Realm values;
- existing VM, GC, module, Promise, generator, and Test262-runner unit invariants remain green.

## Explicitly deferred

P2 does not implement:

- Environment Records / TDZ (P4);
- first-class Reference semantics (P3);
- complete intrinsic graph (P15);
- module registry redesign (P14);
- Realm-specific Error constructors, which become observable when the error intrinsic graph exists.

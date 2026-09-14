# P3 — First-Class Reference Semantics

## Scope

P3 introduces one coherent compiler representation for ECMAScript l-values while preserving
Velune's optimized local/upvalue/module bytecodes.

This phase does **not** implement P4 Environment Records, P5 PropertyKey/Symbol, or the P10
surface grammar for `delete`, `typeof`, compound/logical assignment, destructuring, optional
chaining, or `super`.

## Compiled reference model

`Compiler::CompiledReference` represents the semantic reference produced by an assignable
expression:

- `environment` — optimized local/upvalue/module binding
- `static_property` — property reference with a constant string key
- `computed_property` — property reference whose base and key are evaluated once and preserved

Property references preserve the semantic pieces needed by later phases:

- base
- property key
- receiver (`base` in the current ordinary-member subset)
- strictness slot (currently false until strict-mode semantics are wired)

The compiler centralizes reference work through:

- `compile_reference`
- `emit_get_value`
- `emit_put_value`
- `emit_get_this_value`
- `is_property_reference`
- `is_unresolvable_reference`

Fast binding bytecodes remain optimized lowerings of EnvironmentReference semantics. No heap
Reference object is allocated for ordinary local access.

## Evaluation order

`compile_reference` evaluates a member base first and a computed key second, then saves both in
temporary local slots. RHS evaluation happens only after the reference is fully formed.

Therefore:

```js
base()[key()] = rhs()
base()[key()]++
```

evaluate `base()` and `key()` once, in source order.

## Method calls

Static member calls continue to use `CALL_METHOD`.

P3 adds `CALL_ELEMENT` for computed member calls:

```text
receiver, key, arg0 ... argN
```

The VM resolves the callable through the same computed-property operation and invokes it with the
preserved receiver as `this`. This removes the previous compiler deferral for `obj[key]()`.

## Forward contracts

The P3 reference abstraction is intentionally ready for later extension:

- P4 replaces optimized/transitional binding ownership with Environment Record semantics.
- P5 replaces string property-key identity with `PropertyKey`.
- P10 routes `delete`, `typeof`, compound/logical assignment, destructuring and optional chaining
  through this same reference-lowering layer.
- P11 adds SuperReference semantics.

`typeof undeclaredName` must eventually branch on `is_unresolvable_reference` rather than invoking
normal GetValue. P3 establishes that distinction without prematurely adding the P10 grammar.

## P3 invariants

Focused tests cover:

- computed assignment evaluates reference before RHS
- computed assignment/update base and key are evaluated once
- prefix/postfix update preserve result semantics through common reference lowering
- computed method calls preserve `this`
- computed method key is evaluated once
- verifier understands `CALL_ELEMENT` stack discipline

Test262 infrastructure is unchanged.

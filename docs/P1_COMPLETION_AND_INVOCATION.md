# P1 — Completion Records and Same-VM Invocation

## Scope

P1 replaces the VM's split return/throw state with one semantic completion channel and a separate engine-failure channel. It deliberately does not introduce Realm, Reference, Environment, PropertyKey, descriptor, or object-model work from later phases.

## Contracts

### Completion

`Completion` represents ECMAScript control flow:

- `Normal(Value)`
- `Throw(Value)`
- `Return(Value)`
- `Break(ControlTarget)`
- `Continue(ControlTarget)`

`ControlTarget` is meaningful only for `Break` and `Continue`. It is currently a bytecode instruction target; later labelled-statement work may give it a richer compiler-side identity without changing completion semantics.

A JavaScript `throw` is not an engine failure. An uncaught throw therefore remains an `ExecutionResult` containing `CompletionType::Throw` until an embedding boundary decides how to report it.

### EngineFailure

`EngineFailure` is reserved for failures outside ECMAScript observable control flow:

- `OutOfMemory`
- `InvalidBytecode`
- `ResourceLimit`
- `Interrupted`
- `HostContractViolation`
- `InternalInvariant`

The current `Error` type still exists in older subsystems. P1 maps those errors into `EngineFailure` only at execution boundaries. This transitional adapter is intentionally one-way inside VM semantics; JavaScript catch/finally never receives an `EngineFailure`.

### ExecutionResult

`ExecutionResult` is exactly one of:

- a `Completion`
- an `EngineFailure`

The boolean state of `ExecutionResult` means "the engine produced a semantic completion", not "JavaScript completed normally". Callers must inspect `completion().type()`.

A temporary conversion to `Result<Value>` exists only for pre-P1 embedding helpers and tests. VM/runtime semantic code must not use it to propagate execution state.

## Same-VM invocation

`VM::invoke(callable, thisValue, arguments)` is the single semantic call boundary.

If the VM is already registered as executing, `invoke`:

1. preserves the current operand stack and frame stack,
2. records the current frame depth as an invocation boundary,
3. pushes the callee frame into the same VM,
4. executes until that boundary is reached again,
5. returns `Normal` or `Throw` to the semantic caller without resetting the VM.

If no execution is active, `invoke` owns registration/reset for the host call. Native callbacks use the same `ExecutionResult`, so they can return a JavaScript `Throw` or an `EngineFailure` without conflating the two.

`Context::invoke` forwards to the Runtime's currently active VM when one exists. This is the re-entry hook used by native semantic operations until later phases pass execution context more explicitly.

## Return / throw / finally

Frames store `PendingFinally { Completion, finally_start }`. `finally` does not have a private return-vs-throw enum.

When an abrupt completion crosses a protected region:

1. the completion is saved,
2. the operand stack is restored to the frame base,
3. control enters the innermost `finally`,
4. `END_FINALLY` resumes the saved completion.

If `finally` itself completes abruptly, `discard_overridden_completions` removes the pending completion owned by that `finally`; the new completion then propagates normally. This implements the ECMAScript replacement rule for return, throw, break, and continue.

## Break / continue

Ordinary loop-local `break` and `continue` remain direct `JUMP` bytecodes.

Only jumps that cross a protected `finally` boundary lower to `BREAK target` or `CONTINUE target`. Those opcodes create `Completion::Break/Continue`, run intervening finally blocks, and consume the completion by installing the target PC after the final protected region.

This keeps the normal loop fast path unchanged while removing the old compiler rejection for jumps across finally.

## Generators

Suspended generator frames are preserved. `Generator.next` no longer creates a second VM when called from executing JavaScript. Nested resumption restores the generator frame at the current stack boundary, runs in the same registered VM, and stores only the generator frame's operand-stack slice when yielding.

## Embedding boundaries

The CLI converts an uncaught `Completion::Throw` into the existing non-zero `uncaught_exception:` process diagnostic so Test262 process classification remains valid. This is an embedding/reporting decision, not VM exception state.

Promise reaction jobs inspect `Completion::Throw` directly and reject the chained promise. `EngineFailure` bypasses promise rejection and is returned to the host.

The module subsystem still exposes its older `Result<void>` API; it converts a thrown completion only at that embedding boundary. P2+ may revise module-facing APIs when Realm/execution-context ownership is introduced.

## P1 invariants

Covered locally:

- normal nested invocation
- nested JavaScript throw caught by caller
- re-entered JavaScript throw crossing a native callback and caught by caller
- throw through multiple frames
- return through finally
- throw through finally
- finally overriding return
- finally overriding throw
- break through finally
- continue through finally
- abrupt finally overriding pending break
- native call producing JavaScript Throw
- EngineFailure bypassing JavaScript catch
- generator resumption on the active VM

The bytecode verifier continues to validate all control targets and stack-depth merges. Test262 infrastructure is unchanged.

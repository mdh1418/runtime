# CoreCLR PAL signal handlers: async-signal-safety notes

This document records evidence from this repo that the CoreCLR PAL signal-handling implementation is **not strictly async-signal-safe end-to-end**, and therefore adding additional best-effort work (like managed stack-walking) in crash paths is consistent with existing behavior.

Scope: `src\\coreclr\\pal\\src\\exception\\signal.cpp` (CoreCLR PAL signal handlers).

> Async-signal-safe (POSIX) is a strict requirement: within a signal handler you may only call a small set of functions that are guaranteed safe (e.g., `write(2)`), and you must avoid locks, malloc, most libc, etc. The code here frequently does more than that.

## Summary

The PAL signal handlers aim to:

- translate synchronous signals into managed exceptions when possible (`common_signal_handler`)
- perform shutdown / dump hooks (`PROCNotifyProcessShutdown`, `PROCCreateCrashDumpIfEnabled`) in some cases
- chain to the previous handler (`invoke_previous_action`) when the PAL cannot handle the signal

These goals require calling into CoreCLR/VM and other runtime infrastructure and thus are inherently **best-effort** rather than strictly async-signal-safe.

## Places that use obviously non-async-signal-safe operations

### 1) `sigsegv_handler`: stack overflow spin loop uses `sleep(1)`

In the stack overflow handling path, non-first threads spin forever and sleep.

Evidence:
- `signal.cpp` (`sigsegv_handler` stack overflow path):
  - `sleep(1);`

`sleep` is not async-signal-safe per POSIX. This alone demonstrates the handlers are not strictly async-signal-safe.

### 2) Synchronous fault handlers call `common_signal_handler(...)`

Handlers such as:

- `sigsegv_handler`
- `sigbus_handler`
- `sigill_handler`
- `sigfpe_handler`

attempt to handle the fault by calling `common_signal_handler(...)`.

Evidence:
- `signal.cpp`: calls like:
  - `if (common_signal_handler(code, siginfo, context, ...)) return;`

`common_signal_handler` is not a trivial function; it constructs `EXCEPTION_RECORD`, captures/restores contexts (`RtlCaptureContext`, `RtlRestoreContext`), and drives CoreCLR exception translation/unwinding logic. This cannot be considered strictly async-signal-safe.

### 3) Context capture/restore and custom stack execution

The SIGSEGV path can bounce between an alternate signal stack and the thread’s original stack:

- `SwitchStackAndExecuteHandler(...)` uses `alloca`, `RtlCaptureContext(...)`, and `ExecuteHandlerOnCustomStack(...)`.
- `signal_handler_worker(...)` eventually calls `RtlRestoreContext(...)`.

Evidence:
- `signal.cpp`:
  - `RtlCaptureContext(&pReturnPoint->context);`
  - `ExecuteHandlerOnCustomStack(...)`
  - `RtlRestoreContext(&returnPoint->context, NULL);`

Even if some of these are “low-level”, this is far beyond the small POSIX async-signal-safe surface area.

### 4) Chaining to previous signal handlers (`invoke_previous_action`)

If the PAL cannot handle the signal, it chains to the previous handler, which can be:

- `SIG_IGN` / `SIG_DFL`, or
- a function pointer in `sa_sigaction` / `sa_handler`.

Evidence:
- `signal.cpp` `invoke_previous_action(...)`:
  - `action->sa_sigaction(code, siginfo, context);`
  - `action->sa_handler(code);`

This is inherently not something you can guarantee as async-signal-safe: you have no control over what the previously-installed handler does.

### 5) Some branches call shutdown/dump hooks

`invoke_previous_action(...)` can call:

- `PROCNotifyProcessShutdown(...)`
- `PROCCreateCrashDumpIfEnabled(...)`

Evidence:
- `signal.cpp` `invoke_previous_action(...)`:
  - `PROCNotifyProcessShutdown(IsRunningOnAlternateStack(context));`
  - `PROCCreateCrashDumpIfEnabled(code, siginfo, context, true);`

These are higher-level runtime hooks and are not expected to be async-signal-safe.

### 6) `pthread_sigmask(...)` used in handler-related flow

The helper code manipulating activation signals uses `pthread_sigmask(...)`.

Evidence:
- `signal.cpp`:
  - `pthread_sigmask(SIG_UNBLOCK, ...)`
  - `pthread_sigmask(SIG_BLOCK, ...)`

`pthread_sigmask` is not in the small POSIX async-signal-safe set.

### 7) `sigaction(...)` used to install/restore handlers

Installation and restoration use `sigaction(...)`.

Evidence:
- `signal.cpp` `handle_signal(...)` and `restore_signal(...)`:
  - `sigaction(signal_id, &newAction, previousAction)`
  - `sigaction(signal_id, previousAction, NULL)`

This is part of initialization/cleanup rather than the handler body, but it reinforces the implementation’s reliance on non-trivial libc/OS APIs.

## Implications for adding managed stack walking

Because the existing signal handling is not strictly async-signal-safe already, adding a **best-effort** call to emit a managed stack trace in crash contexts (especially before chaining to a previous handler that may abort) is consistent with the existing design.

However, it remains important to:

- keep the additional work minimal and robust against partial runtime initialization
- avoid allocations/locks where possible
- guard against recursion / repeated logging

## Notes

- Some paths do use `write(2, ...)` for extremely constrained logging, which *is* async-signal-safe.
- But the presence of `sleep(1)`, `common_signal_handler`, and calling unknown previous handlers makes the overall system non-async-signal-safe by strict definition.

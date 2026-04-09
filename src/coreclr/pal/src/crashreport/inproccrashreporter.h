// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Async-signal-safe in-proc crash report generation.
// All functions in this file MUST be async-signal-safe:
// - No malloc/free/new/delete
// - No stdio (fopen/fgets/fprintf)
// - No locks (mutex, spinlock)
// - Only POSIX async-signal-safe functions: write, open, close, read, getpid, snprintf, etc.

#pragma once

#include <signal.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Callback to resolve an instruction pointer to a method name.
// The VM registers this at startup. The implementation MUST be signal-safe
// (no malloc, no locks — use only cached metadata reads).
// Returns: 1 if resolved, 0 if not. Writes into pre-allocated buffers.
typedef int (*InProcCrashReport_ResolveMethodCallback)(
    uint64_t ip,
    char* methodNameBuf, int methodNameBufSize,    // "MethodName"
    char* classNameBuf, int classNameBufSize,      // "Namespace.ClassName"
    char* moduleNameBuf, int moduleNameBufSize,    // "Assembly.dll"
    uint32_t* nativeOffset);

// Register the method resolver callback. Called during VM initialization.
void InProcCrashReport_SetMethodResolver(InProcCrashReport_ResolveMethodCallback callback);

// Generate an in-proc crash report. Called from PROCCreateCrashDumpIfEnabled.
// All arguments come from the signal handler and are signal-safe to read.
void InProcCrashReport_Generate(int signal, siginfo_t* siginfo, void* context);

// Callback to walk the managed stack and report frames.
// The VM registers this at startup. Called from the signal handler.
// frameCallback is called for each frame with the resolved method info.
typedef void (*InProcCrashReport_FrameCallback)(
    uint64_t ip, const char* methodName, const char* className,
    const char* moduleName, uint32_t nativeOffset, uint32_t token, void* ctx);

typedef void (*InProcCrashReport_WalkStackCallback)(
    InProcCrashReport_FrameCallback frameCallback, void* ctx);

void InProcCrashReport_SetStackWalker(InProcCrashReport_WalkStackCallback callback);

// Callback to enumerate all threads and walk each one's managed stack.
// The VM registers this at startup. Called from the signal handler.
// threadCallback is called for each thread. crashingTid identifies the crashing thread.
// For each thread, the VM calls frameCallback for each managed frame.
typedef void (*InProcCrashReport_ThreadCallback)(
    uint64_t osThreadId, int isCrashThread,
    const char* exceptionType, uint32_t exceptionHResult,
    void* ctx);

typedef void (*InProcCrashReport_EnumerateThreadsCallback)(
    uint64_t crashingTid,
    InProcCrashReport_ThreadCallback threadCallback,
    InProcCrashReport_FrameCallback frameCallback,
    void* ctx);

void InProcCrashReport_SetThreadEnumerator(InProcCrashReport_EnumerateThreadsCallback callback);

// Callback to get managed exception info from the current thread.
// Writes exception type and message into pre-allocated buffers.
// Returns: 1 if an exception is available, 0 if not.
typedef int (*InProcCrashReport_GetExceptionCallback)(
    char* exceptionTypeBuf, int exceptionTypeBufSize,    // "System.NullReferenceException"
    char* exceptionMsgBuf, int exceptionMsgBufSize,      // "Object reference not set..."
    uint32_t* hresult);

void InProcCrashReport_SetExceptionResolver(InProcCrashReport_GetExceptionCallback callback);

#ifdef __cplusplus
}
#endif

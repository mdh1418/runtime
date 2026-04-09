// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// In-proc crash report generation.
//
// The default crash path is designed around signal-time inputs such as
// siginfo_t, ucontext_t, and /proc/self/maps, plus any managed data that the VM
// has already published into fixed snapshots. Live VM inspection callbacks
// remain best-effort only and are called out below where applicable.

#pragma once

#include <signal.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the in-proc crash reporter state from startup code, outside the
// signal handler. This pre-publishes options that would otherwise require
// getenv() or other non-crash-time-safe work during report generation.
void InProcCrashReport_Initialize(int writeToFile, const char* dumpPath, const char* defaultDirectory, int enableBestEffort);

// Generate an in-proc crash report. Called from PROCCreateCrashDumpIfEnabled.
// All arguments come from the signal handler and are signal-safe to read.
void InProcCrashReport_Generate(int signal, siginfo_t* siginfo, void* context);

// Callback to determine whether the current thread is attached to the runtime.
// This is used for the default crash thread entry and should rely only on
// the signal-safe thread map / pre-published state.
typedef int (*InProcCrashReport_IsManagedThreadCallback)();

void InProcCrashReport_SetCurrentThreadManagedResolver(InProcCrashReport_IsManagedThreadCallback callback);

// Callback to walk the managed stack and report frames.
// The VM registers this at startup. Called from the signal handler only in the
// best-effort mode; it is not part of the default crash-time path.
// frameCallback is called for each frame with the resolved method/module info.
typedef void (*InProcCrashReport_FrameCallback)(
    uint64_t ip, uint64_t stackPointer, const char* methodName, const char* className,
    const char* moduleName, uint32_t nativeOffset, uint32_t ilOffset, uint32_t token,
    uint32_t timeStamp, uint32_t imageSize, const char* mvid, void* ctx);

typedef void (*InProcCrashReport_WalkStackCallback)(
    InProcCrashReport_FrameCallback frameCallback, void* ctx);

void InProcCrashReport_SetStackWalker(InProcCrashReport_WalkStackCallback callback);

// Callback to enumerate all threads and walk each one's managed stack.
// The VM registers this at startup. The default crash-time path can use it when
// it only replays published thread snapshots; implementations must avoid live
// VM inspection if they are intended for the strict path.
// threadCallback is called for each thread. crashingTid identifies the crashing thread.
// For each thread, the VM calls frameCallback for each managed frame and can
// publish a precomputed managed exception object/type/HRESULT summary.
typedef void (*InProcCrashReport_ThreadCallback)(
    uint64_t osThreadId, int isCrashThread,
    uint64_t exceptionObject, const char* exceptionType, uint32_t exceptionHResult,
    void* ctx);

typedef void (*InProcCrashReport_EnumerateThreadsCallback)(
    uint64_t crashingTid,
    InProcCrashReport_ThreadCallback threadCallback,
    InProcCrashReport_FrameCallback frameCallback,
    void* ctx);

void InProcCrashReport_SetThreadEnumerator(InProcCrashReport_EnumerateThreadsCallback callback);

// Callback to refresh published thread snapshots before report generation.
// Best-effort only: the VM may do live stack walks here to seed the snapshot
// cache for the subsequent strict-style replay path.
typedef void (*InProcCrashReport_PublishThreadSnapshotsCallback)();

void InProcCrashReport_SetThreadSnapshotPublisher(InProcCrashReport_PublishThreadSnapshotsCallback callback);

// Callback to get managed exception info from the current thread.
// Best-effort only: this may inspect runtime/GC state and is therefore not part
// of the default crash-time path.
// Writes a managed exception object/type/message/HRESULT snapshot into
// pre-allocated outputs.
// Returns: 1 if an exception is available, 0 if not.
typedef int (*InProcCrashReport_GetExceptionCallback)(
    uint64_t* exceptionObject,
    char* exceptionTypeBuf, int exceptionTypeBufSize,    // "System.NullReferenceException"
    char* exceptionMsgBuf, int exceptionMsgBufSize,      // "Object reference not set..."
    uint32_t* hresult);

void InProcCrashReport_SetExceptionResolver(InProcCrashReport_GetExceptionCallback callback);

#ifdef __cplusplus
}
#endif

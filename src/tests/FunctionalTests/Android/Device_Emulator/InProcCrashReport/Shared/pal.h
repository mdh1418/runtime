// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Test-only shim standing in for CoreCLR's pal.h when the in-proc crash reporter
// sources are recompiled into the Android functional-test app (Layer 1 fidelity
// gate). It provides the *entire* PAL surface that inproccrashreporter.cpp and
// the watchdog header actually use: two integer typedefs, the Interlocked*
// atomics, GetCurrentProcessId, and the crash-report callback setter. The setter
// is a no-op because the test drives InProcCrashReportSignalDispatcher directly
// instead of through a real PAL signal.

#pragma once

#include <stdint.h>
#include <unistd.h>

typedef int32_t LONG;
typedef int32_t HRESULT;

// InterlockedCompareExchange(dest, exchange, comparand): if *dest == comparand,
// store exchange; returns the original *dest.
inline LONG InterlockedCompareExchange(LONG volatile* dest, LONG exchange, LONG comparand)
{
    return __sync_val_compare_and_swap(dest, comparand, exchange);
}

inline LONG InterlockedExchange(LONG volatile* dest, LONG value)
{
    return __sync_lock_test_and_set(dest, value);
}

// Block template argument deduction so the pointer overload accepts a literal
// nullptr comparand without ambiguity.
template <typename T>
struct InterlockedIdentity { using type = T; };

template <typename T>
inline T* InterlockedCompareExchangePointer(
    T* volatile* dest,
    typename InterlockedIdentity<T*>::type exchange,
    typename InterlockedIdentity<T*>::type comparand)
{
    return __sync_val_compare_and_swap(dest, comparand, exchange);
}

inline uint32_t GetCurrentProcessId()
{
    return static_cast<uint32_t>(getpid());
}

typedef void (*PINPROC_CRASHREPORT_CALLBACK)(int signal, void* siginfo, void* context);

inline void PAL_SetInProcCrashReportCallback(PINPROC_CRASHREPORT_CALLBACK)
{
    // No-op: the test invokes InProcCrashReportSignalDispatcher directly.
}

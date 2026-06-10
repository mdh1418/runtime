// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Test-only shim standing in for CoreCLR's volatile.h when the in-proc crash
// reporter sources are recompiled into the Android functional-test app.
// inproccrashreporter.cpp and the watchdog header only use VolatileLoad /
// VolatileStore for the reporter and watchdog singleton pointers; provide
// acquire/release helpers backed by compiler atomics.

#pragma once

template <typename T>
inline T VolatileLoad(T const* ptr)
{
    T value = *(T const volatile*)ptr;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return value;
}

template <typename T>
inline void VolatileStore(T* ptr, T value)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *(T volatile*)ptr = value;
}

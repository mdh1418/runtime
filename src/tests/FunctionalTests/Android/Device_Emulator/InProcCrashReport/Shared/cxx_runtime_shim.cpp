// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// C++ runtime operator definitions backing the minimal <new> shim (see "new").
//
// With ANDROID_STL=none the NDK toolchain provides no operator new/delete, so we
// supply malloc/free-backed implementations. The reporter only needs the nothrow
// allocating form (it constructs its singleton via `new (std::nothrow)`); the
// remaining ordinary delete forms are provided so the compiler can always resolve
// an operator delete for class cleanup paths.

#include <new>
#include <stdint.h>
#include <stdlib.h>

namespace std
{
    const nothrow_t nothrow{};
}

void* operator new(size_t size, const std::nothrow_t&) noexcept { return malloc(size); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return malloc(size); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { free(ptr); }

void operator delete(void* ptr) noexcept { free(ptr); }
void operator delete[](void* ptr) noexcept { free(ptr); }
void operator delete(void* ptr, size_t) noexcept { free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { free(ptr); }

// Function-local static initialization normally comes from libc++abi, which is
// unavailable with ANDROID_STL=none. Report generation is serialized before any
// of the reporter's local output-sink statics are accessed.
extern "C" int __cxa_guard_acquire(uint64_t* guard)
{
    return __atomic_load_n(reinterpret_cast<uint8_t*>(guard), __ATOMIC_ACQUIRE) == 0;
}

extern "C" void __cxa_guard_release(uint64_t* guard)
{
    __atomic_store_n(reinterpret_cast<uint8_t*>(guard), 1, __ATOMIC_RELEASE);
}

extern "C" void __cxa_guard_abort(uint64_t* /*guard*/)
{
}

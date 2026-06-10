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

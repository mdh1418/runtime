// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#if !__has_builtin(__cpuid)
void __cpuid(int cpuInfo[4], int function_id)
{
    // Based on the Clang implementation provided in cpuid.h:
    // https://github.com/llvm/llvm-project/blob/main/clang/lib/Headers/cpuid.h

    __asm("  cpuid\n" \
        : "=a"(cpuInfo[0]), "=b"(cpuInfo[1]), "=c"(cpuInfo[2]), "=d"(cpuInfo[3]) \
        : "0"(function_id)
        );
}
#else
void __cpuid(int cpuInfo[4], int function_id);
#endif
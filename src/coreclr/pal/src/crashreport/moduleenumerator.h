// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Async-signal-safe module enumerator.
// Reads /proc/self/maps using only open/read/close (all POSIX signal-safe).
// No fopen, no fgets, no sscanf, no malloc.

#pragma once

#include "crashjsonwriter.h"

#ifdef __cplusplus
extern "C" {
#endif

// Write the loaded module list to the JSON writer.
// Reads /proc/self/maps using only async-signal-safe syscalls.
void CrashModules_WriteToJson(CrashJsonWriter* w);

// Look up the executable mapping that contains the specified address.
// Returns 1 if a matching module is found, 0 otherwise.
int CrashModules_TryLookupModuleForAddress(uint64_t address, uint64_t* baseAddress, char* filename, int filenameLen);

// Returns the basename of the first executable image mapping for the current
// process. Returns 1 on success, 0 otherwise.
int CrashModules_TryGetProcessName(char* filename, int filenameLen);

#ifdef __cplusplus
}
#endif

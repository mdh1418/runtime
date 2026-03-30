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

// Write the loaded module list to an fd (for logcat/console).
// Uses only write() syscall.
void CrashModules_WriteToFd(int fd);

#ifdef __cplusplus
}
#endif

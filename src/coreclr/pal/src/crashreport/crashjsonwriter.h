// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Async-signal-safe JSON writer for crash reports.
// Writes to a pre-allocated fixed-size buffer using only signal-safe operations.
// No malloc, no stdio, no locks.

#pragma once

#include <stdint.h>
#include <stddef.h>

// Fixed buffer size for the JSON crash report.
// 32KB accommodates ~200 modules + managed stack frames in compact JSON.
#define CRASH_JSON_BUFFER_SIZE (32 * 1024)  // 32KB

struct CrashJsonWriter
{
    char buffer[CRASH_JSON_BUFFER_SIZE];
    int pos;
    int depth;
    int commaNeeded;  // bool-as-int for C compat

    // All methods below are async-signal-safe (no malloc, no locks)
};

#ifdef __cplusplus
extern "C" {
#endif

void CrashJson_Init(CrashJsonWriter* w);
void CrashJson_OpenObject(CrashJsonWriter* w, const char* key);
void CrashJson_CloseObject(CrashJsonWriter* w);
void CrashJson_OpenArray(CrashJsonWriter* w, const char* key);
void CrashJson_CloseArray(CrashJsonWriter* w);
void CrashJson_WriteString(CrashJsonWriter* w, const char* key, const char* value);
void CrashJson_WriteInt(CrashJsonWriter* w, const char* key, int64_t value);
void CrashJson_WriteHex(CrashJsonWriter* w, const char* key, uint64_t value);
void CrashJson_WriteBool(CrashJsonWriter* w, const char* key, int value);
int  CrashJson_GetLength(CrashJsonWriter* w);
const char* CrashJson_GetBuffer(CrashJsonWriter* w);

#ifdef __cplusplus
}
#endif

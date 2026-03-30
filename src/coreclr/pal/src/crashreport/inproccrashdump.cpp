// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Async-signal-safe in-proc crash dump generator.
// All code in this file MUST be async-signal-safe.

#include "inproccrashdump.h"
#include "crashjsonwriter.h"
#include "moduleenumerator.h"

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <ucontext.h>
#include <setjmp.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

static void WriteToLog(const char* msg, int len)
{
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_ERROR, "DOTNET", msg);
#else
    write(STDERR_FILENO, msg, len);
#endif
}

// Registered callbacks from VM
static volatile InProcCrashDump_ResolveMethodCallback g_resolveMethodCallback = NULL;
static volatile InProcCrashDump_WalkStackCallback g_walkStackCallback = NULL;
static volatile InProcCrashDump_GetExceptionCallback g_getExceptionCallback = NULL;
static volatile InProcCrashDump_EnumerateThreadsCallback g_enumerateThreadsCallback = NULL;

void InProcCrashDump_SetMethodResolver(InProcCrashDump_ResolveMethodCallback callback) { g_resolveMethodCallback = callback; }
void InProcCrashDump_SetStackWalker(InProcCrashDump_WalkStackCallback callback) { g_walkStackCallback = callback; }
void InProcCrashDump_SetExceptionResolver(InProcCrashDump_GetExceptionCallback callback) { g_getExceptionCallback = callback; }
void InProcCrashDump_SetThreadEnumerator(InProcCrashDump_EnumerateThreadsCallback callback) { g_enumerateThreadsCallback = callback; }

void InProcCrashDump_Generate(int signal, siginfo_t* siginfo, void* context)
{
    // Serialize — only one thread should generate the crash report
    static volatile int s_generating = 0;
    if (__sync_val_compare_and_swap(&s_generating, 0, 1) != 0)
        return;

    // TODO: Signal info, registers, modules, stack frames, exception info
    WriteToLog("*** DOTNET CRASH *** (in-proc crash reporter skeleton)\n", 55);
}
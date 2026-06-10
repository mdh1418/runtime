// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Captures the in-proc crash reporter's compact console report in-process.
//
// On Android the reporter emits each console line via __android_log_write under
// CRASHREPORT_LOG_TAG ("DOTNET_CRASH"), routed to logcat rather than a file
// descriptor -- so it cannot be captured by redirecting stdout/stderr. This
// translation unit is linked into the same shared library (libmonodroid.so) as
// the recompiled reporter, so the reporter's __android_log_write calls bind to
// this definition. Lines tagged DOTNET_CRASH are accumulated for later
// validation; every call is also forwarded to the real liblog implementation so
// the report still appears in logcat for debugging.

#define _GNU_SOURCE
#include "inproccrashreport_test_interop.h"

#include <android/log.h>
#include <dlfcn.h>
#include <stddef.h>
#include <string.h>

// Must match CRASHREPORT_LOG_TAG in inproccrashreporter.h.
static const char s_crashTag[] = "DOTNET_CRASH";

static char s_capture[16384];
static size_t s_captureLen;

typedef int (*android_log_write_fn)(int prio, const char* tag, const char* text);

int __android_log_write(int prio, const char* tag, const char* text)
{
    if (tag != NULL && text != NULL && strcmp(tag, s_crashTag) == 0)
    {
        size_t textLen = strlen(text);
        if (s_captureLen + textLen + 1 < sizeof(s_capture))
        {
            memcpy(s_capture + s_captureLen, text, textLen);
            s_captureLen += textLen;
            s_capture[s_captureLen++] = '\n';
            s_capture[s_captureLen] = '\0';
        }
    }

    static android_log_write_fn s_real = NULL;
    if (s_real == NULL)
    {
        s_real = (android_log_write_fn)dlsym(RTLD_NEXT, "__android_log_write");
    }
    if (s_real != NULL)
    {
        return s_real(prio, tag, text);
    }
    return 0;
}

const char* InProcCrashReportTest_GetConsoleCapture(void)
{
    return s_capture;
}

void InProcCrashReportTest_ResetConsoleCapture(void)
{
    s_captureLen = 0;
    s_capture[0] = '\0';
}

// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Shared declarations for the in-proc crash reporter synthetic test's console
// capture. The compact console report is written by the reporter via
// __android_log_write (Android logcat); android_log_interpose.c intercepts those
// writes so the report can be validated in-process alongside the JSON file.

#ifndef INPROCCRASHREPORT_TEST_INTEROP_H
#define INPROCCRASHREPORT_TEST_INTEROP_H

#ifdef __cplusplus
extern "C"
{
#endif

// Returns the accumulated console-report text captured since the last reset
// (one newline-terminated line per __android_log_write under the crash tag).
const char* InProcCrashReportTest_GetConsoleCapture(void);

// Clears the captured console-report text.
void InProcCrashReportTest_ResetConsoleCapture(void);

#ifdef __cplusplus
}
#endif

#endif // INPROCCRASHREPORT_TEST_INTEROP_H

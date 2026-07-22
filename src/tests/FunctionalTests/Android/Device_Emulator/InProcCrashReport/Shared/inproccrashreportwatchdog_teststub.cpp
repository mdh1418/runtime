// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Test-only replacement for inproccrashreportwatchdog.cpp. The synthetic fidelity
// test recompiles the real reporter sources but deliberately does NOT compile the
// real watchdog implementation, because that pulls in CoreCLR's pal/signal.hpp
// (BuildFatalSignalSet, SEHCleanupSignals, ...) and spins a pthread on a
// self-pipe -- neither is needed to validate report output. The REAL watchdog
// header is reused unchanged (so its contract stays in sync); only the
// implementation is stubbed inert here. inproccrashreporter.cpp references just
// CrashReportWatchdog::TryInitialize and the CrashReportWatchdogScope RAII guard.

#include "inproccrashreportwatchdog.h"

bool CrashReportWatchdog::TryInitialize(int /*timeoutSeconds*/)
{
    return true;
}

void CrashReportWatchdog::StartCrashReport()
{
}

void CrashReportWatchdog::StopCrashReport()
{
}

pthread_mutex_t CrashReportWatchdog::s_initializationMutex = PTHREAD_MUTEX_INITIALIZER;
CrashReportWatchdog* CrashReportWatchdog::s_instance = nullptr;

CrashReportWatchdogScope::CrashReportWatchdogScope()
{
}

CrashReportWatchdogScope::~CrashReportWatchdogScope()
{
}

// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Layer 1 fidelity-gate driver for the in-proc crash reporter.
//
// The real reporter sources (signalsafe*.cpp + inproccrashreporter.cpp) are
// recompiled into this functional-test app and driven with purely synthetic,
// deterministic callback data -- no real signal and no real crash. The reporter
// produces the same two outputs it produces in the product (a compact console
// report routed to logcat, and a *.crashreport.json file on disk); the managed
// test harness then reads back the JSON and asserts its structure/fidelity.
//
// This validates the reporter CORE (formatter / JSON writer / console writer /
// report construction). It deliberately does NOT cover the product integration
// path (real PAL signal dispatch, real ucontext, VM stack-walk callbacks, real
// module lookup, watchdog timing, libcoreclr packaging) -- that is the job of the
// host-driven Layer 2 integration test.
//
// Each fatal-signal scenario runs in its OWN process (its own functional-test
// project). The on-demand scenario exercises the reusable caller-sink API.

#include "inproccrashreporter.h"
#include "inproccrashreport_test_interop.h"

#include <minipal/guid.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

// Free dispatcher defined in inproccrashreporter.cpp but not declared in the
// header (it is published to PAL via the callback setter). The test drives it
// directly to exercise the full report-generation path without a real signal.
// Declared with C++ linkage to match the definition (the reporter does not wrap
// it in extern "C").
void InProcCrashReportSignalDispatcher(int signal, void* siginfo, void* context);

namespace
{
    // Scenario ids -- must match the managed harness (Program.cs).
    const int kScenarioRichSigsegv = 0;
    const int kScenarioAbort = 1;
    const int kScenarioStackOverflow = 2;
    const int kScenarioConsoleOnly = 3;

    // Synthetic module handles, resolved by ModuleInfoCallback below.
    const void* const kManagedModule = reinterpret_cast<const void*>(0x1000);
    const void* const kNativeModule = reinterpret_cast<const void*>(0x2000);
    const void* const kNativeModule2 = reinterpret_cast<const void*>(0x3000);

    const GUID kSyntheticGuid =
        { 0x11111111, 0x2222, 0x3333, { 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb } };

    bool IsManagedThreadCallback()
    {
        return true;
    }

    bool ModuleInfoCallback(const void* moduleHandle, const char** moduleName, GUID* moduleGuid)
    {
        if (moduleGuid != nullptr)
        {
            *moduleGuid = kSyntheticGuid;
        }
        if (moduleHandle == kManagedModule)
        {
            if (moduleName != nullptr) *moduleName = "synthetic.managed.dll";
            return true;
        }
        if (moduleHandle == kNativeModule)
        {
            if (moduleName != nullptr) *moduleName = "libsynthetic.so";
            return true;
        }
        if (moduleHandle == kNativeModule2)
        {
            if (moduleName != nullptr) *moduleName = "libnative2.so";
            return true;
        }
        return false;
    }

    // Appends a managed frame (HasManagedIdentity true: methodName + token present).
    void EmitManagedFrame(
        InProcCrashReportFrameCallback frameCallback,
        uint64_t ip,
        const char* methodName,
        const char* className,
        uint32_t token,
        void* ctx)
    {
        frameCallback(
            ip, /*stackPointer*/ ip + 0x1000,
            methodName, className,
            /*moduleName*/ "synthetic.managed.dll", /*moduleHandle*/ kManagedModule,
            /*moduleTimestamp*/ 0x600dcafe, /*moduleSize*/ 0x00010000, /*moduleGuid*/ &kSyntheticGuid,
            /*nativeOffset*/ 0x20, token, /*ilOffset*/ 0x10, ctx);
    }

    // Appends a native frame (no managed identity; native_module set).
    void EmitNativeFrame(
        InProcCrashReportFrameCallback frameCallback,
        uint64_t ip,
        const char* moduleName,
        const void* moduleHandle,
        void* ctx)
    {
        frameCallback(
            ip, /*stackPointer*/ ip + 0x1000,
            /*methodName*/ nullptr, /*className*/ nullptr,
            moduleName, moduleHandle,
            /*moduleTimestamp*/ 0x12345678, /*moduleSize*/ 0x00020000, /*moduleGuid*/ &kSyntheticGuid,
            /*nativeOffset*/ 0x40, /*token*/ 0, /*ilOffset*/ 0, ctx);
    }

    // Rich SIGSEGV snapshot: a managed NullReferenceException crash thread whose
    // stack interleaves managed and native frames and includes generic-instantiation
    // method names, plus two background threads (one managed, one native). One
    // report exercising the multithreaded, interleaved-stack, generics and
    // managed-exception format branches at once.
    void EnumerateThreadsRichSigsegv(
        uint64_t crashingTid,
        InProcCrashReportThreadCallback threadCallback,
        InProcCrashReportFrameCallback frameCallback,
        void* ctx)
    {
        threadCallback(crashingTid, /*isCrashThread*/ true, "System.NullReferenceException", 0x80004003, ctx);
        EmitManagedFrame(frameCallback, 0x000000000040aaaa,
            "DoWork", "Synthetic.App.Worker`1[System.Int32]", 0x06000001, ctx);
        EmitNativeFrame(frameCallback, 0x000000000040bbbb, "libsynthetic.so", kNativeModule, ctx);
        EmitManagedFrame(frameCallback, 0x000000000040cccc,
            "Insert", "Synthetic.App.Dictionary`2[System.String,System.Int32]", 0x06000002, ctx);
        EmitNativeFrame(frameCallback, 0x000000000040dddd, "libnative2.so", kNativeModule2, ctx);

        threadCallback(crashingTid + 1, /*isCrashThread*/ false, nullptr, 0, ctx);
        EmitManagedFrame(frameCallback, 0x000000000040eeee,
            "Listen", "Synthetic.App.Server", 0x06000003, ctx);

        threadCallback(crashingTid + 2, /*isCrashThread*/ false, nullptr, 0, ctx);
        EmitNativeFrame(frameCallback, 0x000000000040ffff, "libsynthetic.so", kNativeModule, ctx);
    }

    // Abort snapshot: a SIGABRT crash thread with NO managed exception and
    // native-only frames, plus a managed background thread. Exercises the
    // null-exception path and the SIGABRT signal formatting.
    void EnumerateThreadsAbort(
        uint64_t crashingTid,
        InProcCrashReportThreadCallback threadCallback,
        InProcCrashReportFrameCallback frameCallback,
        void* ctx)
    {
        threadCallback(crashingTid, /*isCrashThread*/ true, /*exceptionType*/ nullptr, 0, ctx);
        EmitNativeFrame(frameCallback, 0x000000000040aaaa, "libsynthetic.so", kNativeModule, ctx);
        EmitNativeFrame(frameCallback, 0x000000000040bbbb, "libnative2.so", kNativeModule2, ctx);

        threadCallback(crashingTid + 1, /*isCrashThread*/ false, nullptr, 0, ctx);
        EmitManagedFrame(frameCallback, 0x000000000040cccc,
            "Listen", "Synthetic.App.Server", 0x06000001, ctx);
    }

    // Deterministic synthetic register state for the crash thread.
    void FillSyntheticContext(ucontext_t* uc)
    {
        memset(uc, 0, sizeof(*uc));
#if defined(__x86_64__)
        uc->uc_mcontext.gregs[REG_RIP] = 0x000000000040aaaa;
        uc->uc_mcontext.gregs[REG_RSP] = 0x00007fff0000aaaa;
        uc->uc_mcontext.gregs[REG_RBP] = 0x00007fff0000aab0;
#elif defined(__aarch64__)
        uc->uc_mcontext.pc = 0x000000000040aaaa;
        uc->uc_mcontext.sp = 0x00007fff0000aaaa;
        uc->uc_mcontext.regs[29] = 0x00007fff0000aab0;
#endif
    }

    // Writes captured console-report text to consoleCapturePath so the managed
    // harness can validate the compact console report's fidelity. Uses C
    // strings/stdio: the app links ANDROID_STL=none, so <string> is unavailable.
    void WriteConsoleCapture(const char* consoleCapturePath)
    {
        if (consoleCapturePath == nullptr || consoleCapturePath[0] == '\0')
        {
            return;
        }

        const char* console = InProcCrashReportTest_GetConsoleCapture();
        FILE* file = fopen(consoleCapturePath, "w");
        if (file != nullptr)
        {
            fwrite(console, 1, strlen(console), file);
            fclose(file);
        }
    }

    struct OnDemandOutputContext
    {
        FILE* file;
        ucontext_t* signalContext;
        bool attemptReentrantReport;
        bool reentrantAttempted;
        bool reentrantResult;
    };

    bool WriteOnDemandOutput(const char* buffer, size_t length, void* context)
    {
        OnDemandOutputContext* output = static_cast<OnDemandOutputContext*>(context);
        if (output->attemptReentrantReport && !output->reentrantAttempted)
        {
            output->reentrantAttempted = true;
            output->reentrantResult = InProcCrashReportCreateReport(
                InProcCrashReportOutputFormat::Json,
                SIGSEGV,
                output->signalContext,
                &WriteOnDemandOutput,
                output);
        }

        return fwrite(buffer, 1, length, output->file) == length;
    }

    bool WriteOnDemandReport(
        InProcCrashReportOutputFormat outputFormat,
        const char* outputPath,
        ucontext_t* signalContext,
        bool attemptReentrantReport,
        bool* reentrantResult)
    {
        FILE* file = fopen(outputPath, "wb");
        if (file == nullptr)
        {
            return false;
        }

        OnDemandOutputContext output = {};
        output.file = file;
        output.signalContext = signalContext;
        output.attemptReentrantReport = attemptReentrantReport;

        bool generated = InProcCrashReportCreateReport(
            outputFormat,
            SIGSEGV,
            signalContext,
            &WriteOnDemandOutput,
            &output);

        bool closed = fclose(file) == 0;
        if (reentrantResult != nullptr)
        {
            *reentrantResult = output.reentrantResult;
        }

        return generated &&
            closed &&
            (!attemptReentrantReport || output.reentrantAttempted);
    }
}

// Drives one synthetic crash-report scenario.
//
// reporterRootPath is what the reporter is configured with: an existing directory
// enables lifecycle-managed JSON files under <root>/.dotnet/crash-reports; an
// empty string disables file output (compact-log-only mode). consoleCapturePath
// is where the captured compact
// console report is written for the harness to read; it is always a real path so
// console fidelity can be validated even in console-only mode. The compact report
// is also emitted to logcat under the DOTNET_CRASH tag.
//
// Returns 0 on success (the dispatcher ran to completion), negative on a bad
// scenario id. Because the reporter has a one-shot guard, exactly one scenario is
// driven per process.
extern "C" int InProcCrashReportTest_DriveScenario(
    int scenario,
    const char* reporterRootPath,
    const char* consoleCapturePath)
{
    InProcCrashReportTest_ResetConsoleCapture();

    InProcCrashReporterSettings settings = {};
    settings.isManagedThreadCallback = &IsManagedThreadCallback;
    settings.walkStackCallback = nullptr;
    settings.moduleInfoCallback = &ModuleInfoCallback;
    settings.frameLimitPerThread = 0;

    int signalNumber = SIGSEGV;
    switch (scenario)
    {
        case kScenarioRichSigsegv:
        case kScenarioConsoleOnly:
            signalNumber = SIGSEGV;
            settings.enumerateThreadsCallback = &EnumerateThreadsRichSigsegv;
            break;
        case kScenarioAbort:
            signalNumber = SIGABRT;
            settings.enumerateThreadsCallback = &EnumerateThreadsAbort;
            break;
        case kScenarioStackOverflow:
            signalNumber = SIGSEGV;
            settings.enumerateThreadsCallback = nullptr; // SO path does not enumerate threads
            break;
        default:
            return -1;
    }

    InProcCrashReportInitialize(settings);

    InProcCrashReporterServicesSettings services = {};
    services.enableCreateCrashDump = true;
    services.enableWatchdog = false;
    services.enableLifecycle = reporterRootPath != nullptr && reporterRootPath[0] != '\0';
    services.reportRootPath = reporterRootPath;
    services.maxFileCount = 32;
    InProcCrashReportInitializeServices(services);

    if (scenario == kScenarioStackOverflow)
    {
        // Drive the captured-stack-overflow-trace path: the runtime SO helper
        // would have recorded a compressed managed stack (with a repeated
        // recursive sequence) for the reporter to emit later.
        InProcCrashReportSetCrashKind(InProcCrashReportCrashKind::StackOverflow);
        InProcCrashReportBeginStackOverflowTrace(/*crashingTid*/ 0, /*totalFrameCount*/ 42);
        InProcCrashReportAddStackOverflowTraceFrame("Synthetic.App.Program.Main", 1, 0);
        InProcCrashReportAddStackOverflowTraceFrame("Synthetic.App.Recurse.Down", 40, 1);
        InProcCrashReportAddStackOverflowTraceFrame("Synthetic.App.Recurse.Bottom", 1, 0);
        InProcCrashReportEndStackOverflowTrace();
    }

    ucontext_t uc;
    FillSyntheticContext(&uc);

    siginfo_t si;
    memset(&si, 0, sizeof(si));
    si.si_signo = signalNumber;

    InProcCrashReportSignalDispatcher(signalNumber, &si, &uc);

    WriteConsoleCapture(consoleCapturePath);
    return 0;
}

// Exercises the on-demand entry point without enabling signal-path services.
// Returns 0 when both formats are generated repeatedly, a null callback is
// rejected, and a nested report is rejected while the outer report is in flight.
extern "C" int InProcCrashReportTest_DriveOnDemand(
    const char* firstJsonPath,
    const char* secondJsonPath,
    const char* logPath)
{
    InProcCrashReporterSettings settings = {};
    settings.isManagedThreadCallback = &IsManagedThreadCallback;
    settings.walkStackCallback = nullptr;
    settings.enumerateThreadsCallback = &EnumerateThreadsRichSigsegv;
    settings.moduleInfoCallback = &ModuleInfoCallback;
    settings.frameLimitPerThread = 0;
    InProcCrashReportInitialize(settings);

    ucontext_t signalContext;
    FillSyntheticContext(&signalContext);

    if (InProcCrashReportCreateReport(
            InProcCrashReportOutputFormat::Json,
            SIGSEGV,
            &signalContext,
            nullptr,
            nullptr))
    {
        return -1;
    }

    bool reentrantResult = true;
    if (!WriteOnDemandReport(
            InProcCrashReportOutputFormat::Json,
            firstJsonPath,
            &signalContext,
            /*attemptReentrantReport*/ true,
            &reentrantResult))
    {
        return -2;
    }
    if (reentrantResult)
    {
        return -3;
    }

    if (!WriteOnDemandReport(
            InProcCrashReportOutputFormat::Json,
            secondJsonPath,
            &signalContext,
            /*attemptReentrantReport*/ false,
            nullptr))
    {
        return -4;
    }

    if (!WriteOnDemandReport(
            InProcCrashReportOutputFormat::Log,
            logPath,
            &signalContext,
            /*attemptReentrantReport*/ false,
            nullptr))
    {
        return -5;
    }

    return 0;
}

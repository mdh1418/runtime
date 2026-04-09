// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Async-signal-safe in-proc crash report generator.
// Generates a JSON crash report compatible with CrashReportWriter's schema.
// All code in this file MUST be async-signal-safe.

#include "inproccrashreporter.h"
#include "crashjsonwriter.h"
#include "moduleenumerator.h"

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>   // snprintf only
#include <stdlib.h>   // getenv
#include <signal.h>
#include <ucontext.h>
#include <setjmp.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

// Signal-safe write to stderr/logcat
static void WriteToLog(const char* msg, int len)
{
#ifdef __ANDROID__
    // __android_log_write is documented as safe for use in signal handlers
    __android_log_write(ANDROID_LOG_ERROR, "DOTNET", msg);
#else
    write(STDERR_FILENO, msg, len);
#endif
}

static const char* GetSignalName(int signal)
{
    switch (signal)
    {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGTRAP: return "SIGTRAP";
        case SIGTERM: return "SIGTERM";
        default:      return "UNKNOWN";
    }
}

// Map signal to CrashReportWriter's ExceptionType codes
static const char* GetExceptionTypeCode(int signal)
{
    switch (signal)
    {
        case SIGSEGV: return "0x20000000";
        case SIGABRT: return "0x30000000";
        case SIGBUS:  return "0x60000000";
        case SIGILL:  return "0x50000000";
        case SIGFPE:  return "0x70000000";
        case SIGTRAP: return "0x03000000";
        case SIGTERM: return "0x02000000";
        default:      return "0x00000000";
    }
}

// sigsetjmp buffer for catching secondary crashes
static sigjmp_buf s_crashGuardJmpBuf;
static volatile int s_inCrashGuard = 0;

static void CrashGuardSignalHandler(int sig, siginfo_t* info, void* context)
{
    if (s_inCrashGuard)
        siglongjmp(s_crashGuardJmpBuf, sig);
    _exit(128 + sig);
}

// Registered callbacks from VM
static volatile InProcCrashReport_ResolveMethodCallback g_resolveMethodCallback = NULL;
static volatile InProcCrashReport_WalkStackCallback g_walkStackCallback = NULL;
static volatile InProcCrashReport_GetExceptionCallback g_getExceptionCallback = NULL;
static volatile InProcCrashReport_EnumerateThreadsCallback g_enumerateThreadsCallback = NULL;

void InProcCrashReport_SetMethodResolver(InProcCrashReport_ResolveMethodCallback callback) { g_resolveMethodCallback = callback; }
void InProcCrashReport_SetStackWalker(InProcCrashReport_WalkStackCallback callback) { g_walkStackCallback = callback; }
void InProcCrashReport_SetExceptionResolver(InProcCrashReport_GetExceptionCallback callback) { g_getExceptionCallback = callback; }
void InProcCrashReport_SetThreadEnumerator(InProcCrashReport_EnumerateThreadsCallback callback) { g_enumerateThreadsCallback = callback; }

// Extract register values from ucontext_t (platform-specific)
static void WriteRegistersToJson(CrashJsonWriter* w, void* context)
{
    if (context == NULL) return;

    ucontext_t* uctx = (ucontext_t*)context;
    CrashJson_OpenObject(w, "ctx");

#if defined(__x86_64__)
    CrashJson_WriteHex(w, "IP", uctx->uc_mcontext.gregs[REG_RIP]);
    CrashJson_WriteHex(w, "SP", uctx->uc_mcontext.gregs[REG_RSP]);
    CrashJson_WriteHex(w, "BP", uctx->uc_mcontext.gregs[REG_RBP]);
#elif defined(__aarch64__)
    CrashJson_WriteHex(w, "IP", uctx->uc_mcontext.pc);
    CrashJson_WriteHex(w, "SP", uctx->uc_mcontext.sp);
    CrashJson_WriteHex(w, "BP", uctx->uc_mcontext.regs[29]);
#elif defined(__arm__)
    CrashJson_WriteHex(w, "IP", uctx->uc_mcontext.arm_pc);
    CrashJson_WriteHex(w, "SP", uctx->uc_mcontext.arm_sp);
    CrashJson_WriteHex(w, "BP", uctx->uc_mcontext.arm_fp);
#endif

    CrashJson_CloseObject(w);
}

// Write registers to fd for logcat/console display
static void WriteRegistersToFd(int fd, void* context)
{
    if (context == NULL) return;

    ucontext_t* uctx = (ucontext_t*)context;
    char line[128];
    int len;

#if defined(__x86_64__)
    len = snprintf(line, sizeof(line), "Registers: IP=0x%llx SP=0x%llx FP=0x%llx\n",
        (unsigned long long)uctx->uc_mcontext.gregs[REG_RIP],
        (unsigned long long)uctx->uc_mcontext.gregs[REG_RSP],
        (unsigned long long)uctx->uc_mcontext.gregs[REG_RBP]);
#elif defined(__aarch64__)
    len = snprintf(line, sizeof(line), "Registers: IP=0x%llx SP=0x%llx FP=0x%llx\n",
        (unsigned long long)uctx->uc_mcontext.pc,
        (unsigned long long)uctx->uc_mcontext.sp,
        (unsigned long long)uctx->uc_mcontext.regs[29]);
#elif defined(__arm__)
    len = snprintf(line, sizeof(line), "Registers: IP=0x%lx SP=0x%lx FP=0x%lx\n",
        (unsigned long)uctx->uc_mcontext.arm_pc,
        (unsigned long)uctx->uc_mcontext.arm_sp,
        (unsigned long)uctx->uc_mcontext.arm_fp);
#else
    len = snprintf(line, sizeof(line), "Registers: (unsupported architecture)\n");
#endif

    if (len > 0) write(fd, line, len);
}

void InProcCrashReport_Generate(int signal, siginfo_t* siginfo, void* context)
{
    // Serialize — only one thread should generate the crash report
    static volatile int s_generating = 0;
    if (__sync_val_compare_and_swap(&s_generating, 0, 1) != 0)
    {
        return;
    }


    int signalCode = (siginfo != NULL) ? siginfo->si_code : 0;
    uint64_t faultAddr = (siginfo != NULL) ? (uint64_t)siginfo->si_addr : 0;
    pid_t pid = getpid();
    // gettid() is async-signal-safe on Linux
    pid_t tid = 0;
#ifdef __linux__
    tid = gettid();
#endif

    // --- Phase 1: Write crash summary to logcat/console ---
    // Install sigsetjmp guard for the entire crash report generation.
    // On SIGSEGV, the context pointer or runtime state may be invalid.
    {
        struct sigaction guardAction = {0}, oldSigsegv = {0}, oldSigbus = {0};
        guardAction.sa_sigaction = CrashGuardSignalHandler;
        guardAction.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&guardAction.sa_mask);
        sigaction(SIGSEGV, &guardAction, &oldSigsegv);
        sigaction(SIGBUS, &guardAction, &oldSigbus);

        s_inCrashGuard = 1;
        int guardResult = sigsetjmp(s_crashGuardJmpBuf, 1);
        if (guardResult != 0)
        {
            // Secondary crash during report generation — output what we have and bail
            s_inCrashGuard = 0;
            sigaction(SIGSEGV, &oldSigsegv, NULL);
            sigaction(SIGBUS, &oldSigbus, NULL);

            char msg[128];
            int msgLen = snprintf(msg, sizeof(msg),
                "Crash report aborted (secondary signal %d during generation)\n", guardResult);
            if (msgLen > 0) WriteToLog(msg, msgLen);
            return;
        }

        char header[512];
        int len = snprintf(header, sizeof(header),
            "\n*** DOTNET CRASH ***\n"
            "Signal: %s (%d), code=%d, addr=0x%llx\n"
            "PID: %d, TID: %d\n"
#if defined(__x86_64__)
            "Architecture: x64\n",
#elif defined(__aarch64__)
            "Architecture: arm64\n",
#elif defined(__arm__)
            "Architecture: arm\n",
#else
            "Architecture: unknown\n",
#endif
            GetSignalName(signal), signal, signalCode,
            (unsigned long long)faultAddr, pid, tid);

        WriteToLog(header, len);
        WriteRegistersToFd(STDERR_FILENO, context);

        // Walk managed stack for logcat output (covered by outer sigsetjmp guard)
        if (g_walkStackCallback != NULL)
        {
            const char* hdr = "Managed callstack:\n";
            write(STDERR_FILENO, hdr, 19);
            g_walkStackCallback(LogcatFrameCallback, NULL);
        }

        CrashModules_WriteToFd(STDERR_FILENO);
    }

    // --- Phase 2: Build JSON crash report ---
    // CrashJsonWriter uses a 32KB buffer. Declared static to avoid overflowing
    // the alternate signal stack (typically 8-64KB) used by SIGSEGV handler.
    // Thread safety: s_generating CAS ensures only one thread enters this code.
    static CrashJsonWriter jsonWriter;
    CrashJson_Init(&jsonWriter);

    // payload object
    CrashJson_OpenObject(&jsonWriter, NULL);
    CrashJson_OpenObject(&jsonWriter, "payload");
    CrashJson_WriteString(&jsonWriter, "protocol_version", "1.0.0");

    // configuration
    CrashJson_OpenObject(&jsonWriter, "configuration");
#if defined(__x86_64__)
    CrashJson_WriteString(&jsonWriter, "architecture", "amd64");
#elif defined(__aarch64__)
    CrashJson_WriteString(&jsonWriter, "architecture", "arm64");
#elif defined(__arm__)
    CrashJson_WriteString(&jsonWriter, "architecture", "arm");
#endif
    CrashJson_CloseObject(&jsonWriter);

    CrashJson_WriteInt(&jsonWriter, "pid", pid);
    CrashJson_WriteInt(&jsonWriter, "tid", tid);

    // threads array — single crashing thread
    CrashJson_OpenArray(&jsonWriter, "threads");

    if (g_walkStackCallback != NULL)
    {
        CrashJson_OpenObject(&jsonWriter, NULL);
        CrashJson_WriteBool(&jsonWriter, "crashed", 1);
        CrashJson_WriteHex(&jsonWriter, "native_thread_id", tid);

        WriteRegistersToJson(&jsonWriter, context);

        CrashJson_OpenArray(&jsonWriter, "stack_frames");
        g_walkStackCallback(JsonFrameCallback, &jsonWriter);
        CrashJson_CloseArray(&jsonWriter);

        CrashJson_CloseObject(&jsonWriter);
    }

    CrashJson_CloseArray(&jsonWriter);   // threads

    // Modules
    CrashModules_WriteToJson(&jsonWriter);

    CrashJson_CloseObject(&jsonWriter);  // payload

    // parameters
    CrashJson_OpenObject(&jsonWriter, "parameters");
    CrashJson_WriteString(&jsonWriter, "ExceptionType", GetExceptionTypeCode(signal));
    CrashJson_CloseObject(&jsonWriter);

    CrashJson_CloseObject(&jsonWriter);  // root

    // --- Phase 3: Output JSON to logcat/console ---
    const char* json = CrashJson_GetBuffer(&jsonWriter);
    int jsonLen = CrashJson_GetLength(&jsonWriter);

    WriteToLog("Crash report JSON:", 18);
    WriteToLog(json, jsonLen);

    // --- Phase 4: Write JSON to file ---
    // Check if file output is enabled via DOTNET_DbgMiniDumpName or DOTNET_EnableCrashReport
    // getenv is signal-safe per POSIX (as long as no thread is modifying the environment)
    const char* dumpPath = getenv("DOTNET_DbgMiniDumpName");
    const char* enableReport = getenv("DOTNET_EnableCrashReport");
    const char* enableReportOnly = getenv("DOTNET_EnableCrashReportOnly");
    const char* enableMiniDump = getenv("DOTNET_DbgEnableMiniDump");

    int writeFile = 0;
    if (dumpPath != NULL)
        writeFile = 1;
    if (enableReport != NULL && enableReport[0] == '1')
        writeFile = 1;
    if (enableReportOnly != NULL && enableReportOnly[0] == '1')
        writeFile = 1;
    if (enableMiniDump != NULL && enableMiniDump[0] == '1')
        writeFile = 1;

    if (writeFile)
    {
        char reportPath[256];
        if (dumpPath != NULL)
        {
            snprintf(reportPath, sizeof(reportPath), "%s.crashreport.json", dumpPath);
        }
        else
        {
            // Default: use TMPDIR (app cache on Android) or /tmp
            const char* tmpDir = getenv("TMPDIR");
            if (tmpDir == NULL) tmpDir = getenv("HOME");
            if (tmpDir == NULL) tmpDir = "/tmp";
            snprintf(reportPath, sizeof(reportPath), "%s/dotnet_crash_%d.crashreport.json", tmpDir, pid);
        }

        int fd = open(reportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd != -1)
        {
            write(fd, json, jsonLen);
            write(fd, "\n", 1);
            close(fd);

            char msg[384];
            int msgLen = snprintf(msg, sizeof(msg), "Crash report written to: %s\n", reportPath);
            if (msgLen > 0) WriteToLog(msg, msgLen);
        }
    }

    // The outer sigsetjmp guard's signal handlers are intentionally not restored.
    // We're in a crash handler — the process will terminate after this returns.
    s_inCrashGuard = 0;
    sigaction(SIGSEGV, &oldSigsegv, NULL);
    sigaction(SIGBUS, &oldSigbus, NULL);
}

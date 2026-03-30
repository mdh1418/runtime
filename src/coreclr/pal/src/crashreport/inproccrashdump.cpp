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
static volatile InProcCrashDump_ResolveMethodCallback g_resolveMethodCallback = NULL;
static volatile InProcCrashDump_WalkStackCallback g_walkStackCallback = NULL;
static volatile InProcCrashDump_GetExceptionCallback g_getExceptionCallback = NULL;
static volatile InProcCrashDump_EnumerateThreadsCallback g_enumerateThreadsCallback = NULL;

void InProcCrashDump_SetMethodResolver(InProcCrashDump_ResolveMethodCallback callback) { g_resolveMethodCallback = callback; }
void InProcCrashDump_SetStackWalker(InProcCrashDump_WalkStackCallback callback) { g_walkStackCallback = callback; }
void InProcCrashDump_SetExceptionResolver(InProcCrashDump_GetExceptionCallback callback) { g_getExceptionCallback = callback; }
void InProcCrashDump_SetThreadEnumerator(InProcCrashDump_EnumerateThreadsCallback callback) { g_enumerateThreadsCallback = callback; }

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

void InProcCrashDump_Generate(int signal, siginfo_t* siginfo, void* context)
{
    static volatile int s_generating = 0;
    if (__sync_val_compare_and_swap(&s_generating, 0, 1) != 0)
        return;

    int signalCode = (siginfo != NULL) ? siginfo->si_code : 0;
    uint64_t faultAddr = (siginfo != NULL) ? (uint64_t)siginfo->si_addr : 0;
    pid_t pid = getpid();
    pid_t tid = 0;
#ifdef __linux__
    tid = gettid();
#endif

    // Install outer sigsetjmp guard for the entire crash report
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
        s_inCrashGuard = 0;
        sigaction(SIGSEGV, &oldSigsegv, NULL);
        sigaction(SIGBUS, &oldSigbus, NULL);
        char msg[128];
        int msgLen = snprintf(msg, sizeof(msg),
            "Crash report aborted (secondary signal %d during generation)\n", guardResult);
        if (msgLen > 0) WriteToLog(msg, msgLen);
        return;
    }

    // --- Signal info to logcat ---
    {
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
    }

    // TODO: Modules, stack frames, exception info, JSON report

    s_inCrashGuard = 0;
    sigaction(SIGSEGV, &oldSigsegv, NULL);
    sigaction(SIGBUS, &oldSigbus, NULL);
}
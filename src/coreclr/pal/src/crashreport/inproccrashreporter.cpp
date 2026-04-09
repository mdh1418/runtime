// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// In-proc crash report generator.
// The default crash-time path avoids live managed introspection and relies on
// signal-time inputs such as siginfo_t, ucontext_t, and /proc/self/maps.
// Richer managed details are only added through the best-effort callbacks when
// they are explicitly enabled.

#include "inproccrashreporter.h"
#include "crashjsonwriter.h"
#include "moduleenumerator.h"

// Include the .NET Core version string directly because sccsid has internal linkage.
#include "_version.c"

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>   // snprintf for bounded local formatting
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
    if (msg == NULL)
    {
        return;
    }

    if (len < 0)
    {
        len = 0;
        while (msg[len] != '\0')
        {
            len++;
        }
    }

    // __android_log_write is documented as safe for use in signal handlers.
    // Emit long payloads in chunks so the JSON isn't silently truncated by
    // Android's per-entry log size limit.
    int offset = 0;
    while (offset < len)
    {
        int chunk = len - offset;
        if (chunk > 3000)
        {
            chunk = 3000;
        }

        char buffer[3001];
        memcpy(buffer, msg + offset, chunk);
        buffer[chunk] = '\0';
        __android_log_write(ANDROID_LOG_ERROR, "DOTNET", buffer);
        offset += chunk;
    }
#else
    write(STDERR_FILENO, msg, len);
#endif
}

static void AppendChar(char* buffer, int bufferSize, int* pos, char value)
{
    if (*pos < bufferSize - 1)
    {
        buffer[*pos] = value;
        (*pos)++;
    }
}

static void AppendString(char* buffer, int bufferSize, int* pos, const char* value)
{
    if (value == NULL)
    {
        return;
    }

    while (*value != '\0' && *pos < bufferSize - 1)
    {
        buffer[*pos] = *value;
        (*pos)++;
        value++;
    }
}

static void AppendUnsignedDecimal(char* buffer, int bufferSize, int* pos, uint64_t value)
{
    char reverse[32];
    int reversePos = 0;

    if (value == 0)
    {
        AppendChar(buffer, bufferSize, pos, '0');
        return;
    }

    while (value != 0 && reversePos < (int)sizeof(reverse))
    {
        reverse[reversePos++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (reversePos > 0)
    {
        AppendChar(buffer, bufferSize, pos, reverse[--reversePos]);
    }
}

static void AppendSignedDecimal(char* buffer, int bufferSize, int* pos, int64_t value)
{
    uint64_t magnitude = (uint64_t)value;
    if (value < 0)
    {
        AppendChar(buffer, bufferSize, pos, '-');
        magnitude = (uint64_t)(-(value + 1)) + 1;
    }

    AppendUnsignedDecimal(buffer, bufferSize, pos, magnitude);
}

static void TerminateBuffer(char* buffer, int bufferSize, int* pos)
{
    if (*pos >= bufferSize)
    {
        *pos = bufferSize - 1;
    }
    buffer[*pos] = '\0';
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

// Startup-published configuration and callbacks — set during initialization and
// only read from the signal handler.
static volatile int g_writeReportToFile = 0;
static volatile int g_bestEffortEnabled = 0;
static char g_reportPath[256];
static char g_defaultReportDirectory[256];

// Registered callbacks from VM — best-effort callbacks are only used when
// explicitly enabled. The current-thread managed check is used in the strict
// path and should rely only on the signal-safe thread map.
static volatile InProcCrashReport_IsManagedThreadCallback g_isManagedThreadCallback = NULL;
static volatile InProcCrashReport_WalkStackCallback g_walkStackCallback = NULL;
static volatile InProcCrashReport_GetExceptionCallback g_getExceptionCallback = NULL;
static volatile InProcCrashReport_EnumerateThreadsCallback g_enumerateThreadsCallback = NULL;

void InProcCrashReport_Initialize(int writeToFile, const char* dumpPath, const char* defaultDirectory, int enableBestEffort)
{
    g_reportPath[0] = '\0';
    if (dumpPath != NULL)
    {
        int i = 0;
        while (dumpPath[i] != '\0' && i < (int)sizeof(g_reportPath) - 1)
        {
            g_reportPath[i] = dumpPath[i];
            i++;
        }
        g_reportPath[i] = '\0';
    }

    g_defaultReportDirectory[0] = '\0';
    if (defaultDirectory != NULL)
    {
        int i = 0;
        while (defaultDirectory[i] != '\0' && i < (int)sizeof(g_defaultReportDirectory) - 1)
        {
            g_defaultReportDirectory[i] = defaultDirectory[i];
            i++;
        }
        g_defaultReportDirectory[i] = '\0';
    }

    __sync_synchronize();
    g_bestEffortEnabled = enableBestEffort;
    g_writeReportToFile = writeToFile;
}

void InProcCrashReport_SetCurrentThreadManagedResolver(InProcCrashReport_IsManagedThreadCallback callback)
{
    g_isManagedThreadCallback = callback;
}

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

static uint64_t GetInstructionPointer(void* context)
{
    if (context == NULL)
        return 0;

    ucontext_t* uctx = (ucontext_t*)context;

#if defined(__x86_64__)
    return (uint64_t)uctx->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    return (uint64_t)uctx->uc_mcontext.pc;
#elif defined(__arm__)
    return (uint64_t)uctx->uc_mcontext.arm_pc;
#else
    return 0;
#endif
}

static uint64_t GetStackPointer(void* context)
{
    if (context == NULL)
        return 0;

    ucontext_t* uctx = (ucontext_t*)context;

#if defined(__x86_64__)
    return (uint64_t)uctx->uc_mcontext.gregs[REG_RSP];
#elif defined(__aarch64__)
    return (uint64_t)uctx->uc_mcontext.sp;
#elif defined(__arm__)
    return (uint64_t)uctx->uc_mcontext.arm_sp;
#else
    return 0;
#endif
}

static void WriteCrashSiteFrameToJson(CrashJsonWriter* w, void* context)
{
    uint64_t ip = GetInstructionPointer(context);
    uint64_t sp = GetStackPointer(context);
    uint64_t moduleBase = 0;
    char moduleName[256];

    CrashJson_OpenObject(w, NULL);
    CrashJson_WriteBool(w, "is_managed", 0);
    CrashJson_WriteHex(w, "stack_pointer", sp);
    CrashJson_WriteHex(w, "native_address", ip);

    if (CrashModules_TryLookupModuleForAddress(ip, &moduleBase, moduleName, sizeof(moduleName)))
    {
        CrashJson_WriteHex(w, "module_address", moduleBase);
        CrashJson_WriteHex(w, "native_image_offset", ip - moduleBase);
        CrashJson_WriteString(w, "native_module", moduleName);
    }

    CrashJson_CloseObject(w);
}

// sigsetjmp buffer for catching secondary crashes during risky operations
static sigjmp_buf s_crashGuardJmpBuf;
static volatile sig_atomic_t s_inCrashGuard = 0;
static volatile sig_atomic_t s_crashGuardThreadId = 0;

// Secondary signal handler — catches crashes within the crash reporter
static void CrashGuardSignalHandler(int sig, siginfo_t* info, void* context)
{
    (void)info;
    (void)context;

    if (s_inCrashGuard)
    {
#ifdef __linux__
        if ((sig_atomic_t)gettid() != s_crashGuardThreadId)
        {
            _exit(128 + sig);
        }
#endif
        siglongjmp(s_crashGuardJmpBuf, sig);
    }
    // If not in guard, let the default handler deal with it
    _exit(128 + sig);
}

// Callback for writing stack frames to JSON
static void JsonFrameCallback(uint64_t ip, uint64_t stackPointer, const char* methodName, const char* className,
    const char* moduleName, uint32_t nativeOffset, uint32_t token, void* ctx)
{
    CrashJsonWriter* w = (CrashJsonWriter*)ctx;
    uint64_t moduleBase = 0;
    char nativeModuleName[256];
    nativeModuleName[0] = '\0';

    CrashJson_OpenObject(w, NULL);
    CrashJson_WriteHex(w, "stack_pointer", stackPointer);
    CrashJson_WriteHex(w, "native_address", ip);
    CrashJson_WriteHex(w, "native_offset", nativeOffset);
    if (CrashModules_TryLookupModuleForAddress(ip, &moduleBase, nativeModuleName, sizeof(nativeModuleName)))
    {
        CrashJson_WriteHex(w, "module_address", moduleBase);
        CrashJson_WriteHex(w, "native_image_offset", ip - moduleBase);
    }

    if (methodName != NULL)
    {
        // Build "ClassName.MethodName" into a stack buffer
        char fullName[256];
        if (className != NULL && className[0] != '\0')
            snprintf(fullName, sizeof(fullName), "%s.%s", className, methodName);
        else
            snprintf(fullName, sizeof(fullName), "%s", methodName);
        CrashJson_WriteString(w, "method_name", fullName);
        CrashJson_WriteBool(w, "is_managed", 1);
        CrashJson_WriteHex(w, "token", token);
        CrashJson_WriteHex(w, "il_offset", ilOffset);
        CrashJson_WriteHex(w, "timestamp", timeStamp);
        CrashJson_WriteHex(w, "sizeofimage", imageSize);
        if (moduleName != NULL)
            CrashJson_WriteString(w, "filename", moduleName);
    }
    else
    {
        CrashJson_WriteBool(w, "is_managed", 0);
        if (nativeModuleName[0] != '\0')
            CrashJson_WriteString(w, "native_module", nativeModuleName);
        else if (moduleName != NULL)
            CrashJson_WriteString(w, "native_module", moduleName);
    }

    CrashJson_CloseObject(w);
}

void InProcCrashReport_Generate(int signal, siginfo_t* siginfo, void* context)
{
    // Serialize — only one thread should generate the crash report
    static volatile int s_generating = 0;
    if (__sync_val_compare_and_swap(&s_generating, 0, 1) != 0)
    {
        return;
    }


    pid_t pid = getpid();
    // gettid() is async-signal-safe on Linux
    pid_t tid = 0;
#ifdef __linux__
    tid = gettid();
#endif
    int bestEffortEnabled = g_bestEffortEnabled != 0;

    // Gather optional managed exception data for the JSON report.
    // Only attempt for signals that may have managed exception state (SIGABRT from managed FailFast).
    // SIGSEGV crashes in native code don't have a managed throwable — attempting to read one faults.
    uint64_t exObject = 0;
    char exTypeBuf[256] = {0};
    char exMsgBuf[512] = {0};
    uint32_t exHresult = 0;
    int hasException = 0;
    if (bestEffortEnabled && g_getExceptionCallback != NULL && signal != SIGSEGV && signal != SIGBUS)
    {
        struct sigaction guardAction = {0}, oldSigsegv = {0}, oldSigbus = {0};
        guardAction.sa_sigaction = CrashGuardSignalHandler;
        guardAction.sa_flags = SA_SIGINFO;
        sigemptyset(&guardAction.sa_mask);
        sigaction(SIGSEGV, &guardAction, &oldSigsegv);
        sigaction(SIGBUS, &guardAction, &oldSigbus);

        s_crashGuardThreadId = (sig_atomic_t)tid;
        s_inCrashGuard = 1;
        int guardResult = sigsetjmp(s_crashGuardJmpBuf, 1);
        if (guardResult == 0)
        {
            hasException = g_getExceptionCallback(&exObject, exTypeBuf, sizeof(exTypeBuf),
                exMsgBuf, sizeof(exMsgBuf), &exHresult);
        }
        // If guardResult != 0, exception info faulted — skip it
        s_inCrashGuard = 0;
        s_crashGuardThreadId = 0;

        sigaction(SIGSEGV, &oldSigsegv, NULL);
        sigaction(SIGBUS, &oldSigbus, NULL);
    }

    // Install sigsetjmp guard around the crash report generation path so a
    // secondary fault while gathering optional enrichment does not recurse.
    {
        struct sigaction guardAction = {0}, oldSigsegv = {0}, oldSigbus = {0};
        guardAction.sa_sigaction = CrashGuardSignalHandler;
        guardAction.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&guardAction.sa_mask);
        sigaction(SIGSEGV, &guardAction, &oldSigsegv);
        sigaction(SIGBUS, &guardAction, &oldSigbus);

        s_crashGuardThreadId = (sig_atomic_t)tid;
        s_inCrashGuard = 1;
        int guardResult = sigsetjmp(s_crashGuardJmpBuf, 1);
        if (guardResult != 0)
        {
            // Secondary crash during report generation — output what we have and bail
            s_inCrashGuard = 0;
            s_crashGuardThreadId = 0;
            sigaction(SIGSEGV, &oldSigsegv, NULL);
            sigaction(SIGBUS, &oldSigbus, NULL);

            char msg[128];
            int msgLen = 0;
            AppendString(msg, sizeof(msg), &msgLen, "Crash report aborted (secondary signal ");
            AppendSignedDecimal(msg, sizeof(msg), &msgLen, (int64_t)guardResult);
            AppendString(msg, sizeof(msg), &msgLen, " during generation)\n");
            TerminateBuffer(msg, sizeof(msg), &msgLen);
            if (msgLen > 0) WriteToLog(msg, msgLen);
            return;
        }
    }

    // --- Build the JSON crash report ---
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
    if (strncmp(sccsid, "@(#)Version ", 12) == 0)
    {
        char version[128];
        int versionLen = 0;
        version[0] = '\0';
        AppendString(version, sizeof(version), &versionLen, sccsid + 12);
        AppendString(version, sizeof(version), &versionLen, " ");
        TerminateBuffer(version, sizeof(version), &versionLen);
        CrashJson_WriteString(&jsonWriter, "version", version);
    }
    CrashJson_CloseObject(&jsonWriter);

    char processName[256];
    if (CrashModules_TryGetProcessName(processName, sizeof(processName)))
    {
        CrashJson_WriteString(&jsonWriter, "process_name", processName);
    }

    // threads array — the strict path can enumerate managed threads through
    // published snapshots, while best-effort mode can still augment the report
    // with live current-thread exception or stack inspection when enabled.
    CrashJson_OpenArray(&jsonWriter, "threads");

    if (bestEffortEnabled && g_enumerateThreadsCallback != NULL)
    {
        // Context struct passed through callbacks to build JSON.
        // threadCount tracks whether we need to close the previous thread's JSON.
        struct MultiThreadJsonCtx {
            CrashJsonWriter* writer;
            void* signalContext;
            int threadCount;
            int sawCrashThread;
            int hasCrashException;
            uint64_t crashExceptionObject;
            const char* crashExceptionType;
            uint32_t crashExceptionHResult;
        };
        MultiThreadJsonCtx mtCtx = { &jsonWriter, context, 0, 0, hasException, exObject, exTypeBuf, exHresult };

        g_enumerateThreadsCallback(
            tid,
            // threadCallback — called at the start of each thread
            [](uint64_t osThreadId, int isCrashThread,
               uint64_t exceptionObject, const char* exceptionType, uint32_t exceptionHResult,
               void* ctx) {
                MultiThreadJsonCtx* mtCtx = (MultiThreadJsonCtx*)ctx;
                CrashJsonWriter* w = mtCtx->writer;

                // Close previous thread's stack_frames + object if not the first
                if (mtCtx->threadCount > 0)
                {
                    CrashJson_CloseArray(w);   // stack_frames
                    CrashJson_CloseObject(w);  // thread
                }
                mtCtx->threadCount++;
                if (isCrashThread)
                {
                    mtCtx->sawCrashThread = 1;
                }

                CrashJson_OpenObject(w, NULL);
                CrashJson_WriteBool(w, "is_managed", 1);
                CrashJson_WriteBool(w, "crashed", isCrashThread);
                CrashJson_WriteHex(w, "native_thread_id", osThreadId);

                if (isCrashThread && mtCtx->hasCrashException)
                {
                    if (mtCtx->crashExceptionObject != 0)
                    {
                        CrashJson_WriteHex(w, "managed_exception_object", mtCtx->crashExceptionObject);
                    }
                    CrashJson_WriteString(w, "managed_exception_type", mtCtx->crashExceptionType);
                    CrashJson_WriteHex(w, "managed_exception_hresult", mtCtx->crashExceptionHResult);
                }
                else if (exceptionType != NULL && exceptionType[0] != '\0')
                {
                    if (exceptionObject != 0)
                    {
                        CrashJson_WriteHex(w, "managed_exception_object", exceptionObject);
                    }
                    CrashJson_WriteString(w, "managed_exception_type", exceptionType);
                    CrashJson_WriteHex(w, "managed_exception_hresult", exceptionHResult);
                }

                if (isCrashThread)
                {
                    WriteRegistersToJson(w, mtCtx->signalContext);
                }

                CrashJson_OpenArray(w, "stack_frames");
                if (isCrashThread)
                {
                    WriteCrashSiteFrameToJson(w, mtCtx->signalContext);
                }
            },
            // frameCallback — extracts CrashJsonWriter from MultiThreadJsonCtx
            [](uint64_t ip, uint64_t stackPointer, const char* methodName, const char* className,
               const char* moduleName, uint32_t nativeOffset, uint32_t token, void* ctx) {
                MultiThreadJsonCtx* mtCtx = (MultiThreadJsonCtx*)ctx;
                JsonFrameCallback(ip, stackPointer, methodName, className, moduleName, nativeOffset, token, mtCtx->writer);
            },
            &mtCtx);

        // Close the last thread's stack_frames + object
        if (mtCtx.threadCount > 0)
        {
            CrashJson_CloseArray(&jsonWriter);   // stack_frames
            CrashJson_CloseObject(&jsonWriter);  // thread
        }

        if (mtCtx.threadCount == 0 || !mtCtx.sawCrashThread)
        {
            CrashJson_OpenObject(&jsonWriter, NULL);
            int isManagedThread = g_isManagedThreadCallback != NULL ? g_isManagedThreadCallback() : 0;
            CrashJson_WriteBool(&jsonWriter, "is_managed", isManagedThread);
            CrashJson_WriteBool(&jsonWriter, "crashed", 1);
            CrashJson_WriteHex(&jsonWriter, "native_thread_id", tid);
            if (mtCtx.hasCrashException)
            {
                if (mtCtx.crashExceptionObject != 0)
                {
                    CrashJson_WriteHex(&jsonWriter, "managed_exception_object", mtCtx.crashExceptionObject);
                }
                CrashJson_WriteString(&jsonWriter, "managed_exception_type", mtCtx.crashExceptionType);
                CrashJson_WriteHex(&jsonWriter, "managed_exception_hresult", mtCtx.crashExceptionHResult);
            }
            WriteRegistersToJson(&jsonWriter, context);
            CrashJson_OpenArray(&jsonWriter, "stack_frames");
            WriteCrashSiteFrameToJson(&jsonWriter, context);
            CrashJson_CloseArray(&jsonWriter);
            CrashJson_CloseObject(&jsonWriter);
        }
    }
    else
    {
        // Default fallback: crashing thread only, with a synthetic native
        // crash-site frame derived from ucontext_t and /proc/self/maps.
        CrashJson_OpenObject(&jsonWriter, NULL);
        int isManagedThread = g_isManagedThreadCallback != NULL ? g_isManagedThreadCallback() : 0;
        CrashJson_WriteBool(&jsonWriter, "is_managed", isManagedThread);
        CrashJson_WriteBool(&jsonWriter, "crashed", 1);
        CrashJson_WriteHex(&jsonWriter, "native_thread_id", tid);

        if (bestEffortEnabled && hasException)
        {
            if (exObject != 0)
            {
                CrashJson_WriteHex(&jsonWriter, "managed_exception_object", exObject);
            }
            CrashJson_WriteString(&jsonWriter, "managed_exception_type", exTypeBuf);
            CrashJson_WriteHex(&jsonWriter, "managed_exception_hresult", exHresult);
        }

        WriteRegistersToJson(&jsonWriter, context);

        CrashJson_OpenArray(&jsonWriter, "stack_frames");
        WriteCrashSiteFrameToJson(&jsonWriter, context);
        if (bestEffortEnabled && g_walkStackCallback != NULL)
        {
            g_walkStackCallback(JsonFrameCallback, &jsonWriter);
        }
        CrashJson_CloseArray(&jsonWriter);

        CrashJson_CloseObject(&jsonWriter);
    }

    CrashJson_CloseArray(&jsonWriter);   // threads

    CrashJson_CloseObject(&jsonWriter);  // payload

    // parameters
    CrashJson_OpenObject(&jsonWriter, "parameters");
    CrashJson_WriteString(&jsonWriter, "ExceptionType", hasException ? "0x05000000" : GetExceptionTypeCode(signal));
    CrashJson_CloseObject(&jsonWriter);

    CrashJson_CloseObject(&jsonWriter);  // root

    // Emit only the JSON payload to logcat/console.
    const char* json = CrashJson_GetBuffer(&jsonWriter);
    int jsonLen = CrashJson_GetLength(&jsonWriter);

    WriteToLog(json, jsonLen);

    // Write the same JSON payload to a file when configured.
    // File output enablement and the dump path are pre-published during startup
    // by PROCAbortInitialize so the signal handler does not need getenv().
    if (g_writeReportToFile != 0)
    {
        char reportPath[256];
        if (g_reportPath[0] != '\0')
        {
            int pathLen = 0;
            reportPath[0] = '\0';
            AppendString(reportPath, sizeof(reportPath), &pathLen, g_reportPath);
            AppendString(reportPath, sizeof(reportPath), &pathLen, ".crashreport.json");
            TerminateBuffer(reportPath, sizeof(reportPath), &pathLen);
        }
        else
        {
            const char* directory = g_defaultReportDirectory[0] != '\0' ? g_defaultReportDirectory : "/tmp";
            int pathLen = 0;
            reportPath[0] = '\0';
            AppendString(reportPath, sizeof(reportPath), &pathLen, directory);
            AppendString(reportPath, sizeof(reportPath), &pathLen, "/dotnet_crash_");
            AppendUnsignedDecimal(reportPath, sizeof(reportPath), &pathLen, (uint64_t)pid);
            AppendString(reportPath, sizeof(reportPath), &pathLen, ".crashreport.json");
            TerminateBuffer(reportPath, sizeof(reportPath), &pathLen);
        }

        int fd = open(reportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd != -1)
        {
            write(fd, json, jsonLen);
            write(fd, "\n", 1);
            close(fd);
        }
    }

    // The outer sigsetjmp guard's signal handlers are intentionally not restored.
    // We're in a crash handler — the process will terminate after this returns.
    s_inCrashGuard = 0;
    s_crashGuardThreadId = 0;
}

// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// VM-side implementation of the in-proc crash report callbacks.
// Registered with the PAL crash reporter at startup.
//
// Important: the managed stack walking and exception inspection callbacks below
// are best-effort helpers for the richer report mode; they are not the strict
// async-signal-safe path. The current-thread managed check uses GetThreadAsyncSafe()
// and is intended for the strict crash-thread entry.

#include "common.h"
#include "method.hpp"
#include "codeman.h"
#include "SignalSafeThreadMap.h"

#ifdef HOST_ANDROID

#include "../pal/src/crashreport/inproccrashreporter.h"

// Shared frame callback that resolves method info from a CrawlFrame.
// Used by both single-thread and multi-thread walkers.
struct WalkContext {
    InProcCrashReport_FrameCallback callback;
    void* userCtx;
};

static StackWalkAction FrameCallbackAdapter(CrawlFrame* pCF, VOID* pData)
{
    WalkContext* ctx = (WalkContext*)pData;
    MethodDesc* pMD = pCF->GetFunction();
    if (pMD == NULL)
        return SWA_CONTINUE;

    LPCUTF8 methodName = pMD->GetName();
    mdMethodDef token = pMD->GetMemberDef();

    LPCUTF8 className = NULL;
    LPCUTF8 namespaceName = NULL;
    MethodTable* pMT = pMD->GetMethodTable();
    if (pMT != NULL)
    {
        mdTypeDef cl = pMT->GetCl();
        IMDInternalImport* pImport = pMD->GetMDImport();
        if (pImport != NULL && cl != mdTypeDefNil)
        {
            pImport->GetNameOfTypeDef(cl, &className, &namespaceName);
        }
    }

    char classNameBuf[256] = {0};
    if (namespaceName != NULL && namespaceName[0] != '\0')
        snprintf(classNameBuf, sizeof(classNameBuf), "%s.%s", namespaceName, className ? className : "");
    else if (className != NULL)
        snprintf(classNameBuf, sizeof(classNameBuf), "%s", className);

    const char* moduleName = NULL;
    Module* pModule = pMD->GetModule();
    if (pModule != NULL)
    {
        Assembly* pAssembly = pModule->GetAssembly();
        if (pAssembly != NULL)
            moduleName = pAssembly->GetSimpleName();
    }

    uint32_t nativeOffset = pCF->HasFaulted() ? 0 : pCF->GetRelOffset();

    uint64_t ip = 0;
    uint64_t stackPointer = 0;
    PREGDISPLAY pRD = pCF->GetRegisterSet();
    if (pRD != NULL)
    {
        ip = (uint64_t)GetControlPC(pRD);
        stackPointer = (uint64_t)GetRegdisplaySP(pRD);
    }

    ctx->callback(ip, stackPointer, methodName, classNameBuf, moduleName, nativeOffset, (uint32_t)token, ctx->userCtx);
    return SWA_CONTINUE;
}

// Walk the managed stack of the current thread.
static void CrashReport_WalkStack(InProcCrashReport_FrameCallback frameCallback, void* ctx)
{
    Thread* pThread = GetThreadAsyncSafe();
    if (pThread == NULL)
        return;

    WalkContext walkCtx = { frameCallback, ctx };
    pThread->StackWalkFrames(FrameCallbackAdapter, &walkCtx,
        QUICKUNWIND | FUNCTIONSONLY | ALLOW_ASYNC_STACK_WALK);
}

static int CrashReport_IsCurrentThreadManaged()
{
    return GetThreadAsyncSafe() != NULL;
}

// Get exception info for a specific thread.
static int CrashReport_GetExceptionForThread(
    Thread* pThread,
    char* exceptionTypeBuf, int exceptionTypeBufSize,
    uint32_t* hresult)
{
    OBJECTREF throwable = pThread->GetThrowable();
    if (throwable == NULL)
        return 0;

    MethodTable* pMT = throwable->GetMethodTable();
    if (pMT != NULL)
    {
        mdTypeDef cl = pMT->GetCl();
        Module* pModule = pMT->GetModule();
        if (pModule != NULL)
        {
            IMDInternalImport* pImport = pModule->GetMDImport();
            if (pImport != NULL && cl != mdTypeDefNil)
            {
                LPCUTF8 className = NULL;
                LPCUTF8 namespaceName = NULL;
                pImport->GetNameOfTypeDef(cl, &className, &namespaceName);
                if (namespaceName != NULL && namespaceName[0] != '\0')
                    snprintf(exceptionTypeBuf, exceptionTypeBufSize, "%s.%s", namespaceName, className ? className : "");
                else if (className != NULL)
                    snprintf(exceptionTypeBuf, exceptionTypeBufSize, "%s", className);
            }
        }
    }

    *hresult = ((EXCEPTIONREF)throwable)->GetHResult();
    return 1;
}

struct EnumerateThreadsContext
{
    uint64_t crashingTid;
    Thread* pCrashThread;
    InProcCrashDump_ThreadCallback threadCallback;
    InProcCrashDump_FrameCallback frameCallback;
    void* callbackContext;
};

static void EnumerateThreadFromSignalSafeMap(size_t osThread, void* pThreadObject, void* context)
{
    EnumerateThreadsContext* enumerateContext = (EnumerateThreadsContext*)context;
    Thread* pThread = (Thread*)pThreadObject;
    int isCrashThread = osThread == enumerateContext->crashingTid ? 1 : 0;

    enumerateContext->threadCallback((uint64_t)osThread, isCrashThread, "", 0, enumerateContext->callbackContext);

    if (isCrashThread && pThread == enumerateContext->pCrashThread)
    {
        WalkContext walkCtx = { enumerateContext->frameCallback, enumerateContext->callbackContext };
        pThread->StackWalkFrames(FrameCallbackAdapter, &walkCtx,
            QUICKUNWIND | FUNCTIONSONLY | ALLOW_ASYNC_STACK_WALK);
    }
}

// Enumerate managed threads through the signal-safe thread map. This avoids
// walking the live ThreadStore list from the crash signal path.
static void CrashReport_EnumerateThreads(
    uint64_t crashingTid,
    InProcCrashReport_ThreadCallback threadCallback,
    InProcCrashReport_FrameCallback frameCallback,
    void* ctx)
{
    Thread* pCrashThread = GetThreadAsyncSafe();
    EnumerateThreadsContext enumerateContext = { crashingTid, pCrashThread, threadCallback, frameCallback, ctx };
    EnumerateThreadsInSignalSafeMap(EnumerateThreadFromSignalSafeMap, &enumerateContext);
}

// Get managed exception info from the current thread (legacy single-thread callback).
static int CrashReport_GetException(
    char* exceptionTypeBuf, int exceptionTypeBufSize,
    char* exceptionMsgBuf, int exceptionMsgBufSize,
    uint32_t* hresult)
{
    Thread* pThread = GetThreadAsyncSafe();
    if (pThread == NULL)
        return 0;

    exceptionMsgBuf[0] = '\0';
    return CrashReport_GetExceptionForThread(pThread, exceptionTypeBuf, exceptionTypeBufSize, hresult);
}

// Called during VM initialization to register callbacks with the PAL crash reporter
void CrashReport_RegisterStackWalker()
{
    InProcCrashReport_SetStackWalker(CrashReport_WalkStack);
    InProcCrashReport_SetExceptionResolver(CrashReport_GetException);
    InProcCrashReport_SetThreadEnumerator(CrashReport_EnumerateThreads);
    InProcCrashReport_SetCurrentThreadManagedResolver(CrashReport_IsCurrentThreadManaged);
}

#endif // HOST_ANDROID

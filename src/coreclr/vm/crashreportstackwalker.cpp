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

static void CopyStringRefToAscii(STRINGREF source, char* destination, int destinationLength)
{
    if (destination == NULL || destinationLength <= 0)
    {
        return;
    }

    int position = 0;
    if (source != NULL)
    {
        DWORD stringLength = source->GetStringLength();
        const WCHAR* chars = source->GetBuffer();
        for (DWORD i = 0; i < stringLength && position < destinationLength - 1; i++)
        {
            WCHAR ch = chars[i];
            destination[position++] = ch <= 0x7f ? (char)ch : '?';
        }
    }

    destination[position] = '\0';
}

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
    uint64_t* exceptionObject,
    char* exceptionTypeBuf, int exceptionTypeBufSize,
    char* exceptionMessageBuf, int exceptionMessageBufSize,
    uint32_t* hresult)
{
    if (exceptionObject != NULL)
    {
        *exceptionObject = 0;
    }
    if (exceptionTypeBuf != NULL && exceptionTypeBufSize > 0)
    {
        exceptionTypeBuf[0] = '\0';
    }
    if (exceptionMessageBuf != NULL && exceptionMessageBufSize > 0)
    {
        exceptionMessageBuf[0] = '\0';
    }
    if (hresult != NULL)
    {
        *hresult = 0;
    }

    if (pThread->GetExceptionState()->TryGetPublishedCrashReportException(
            exceptionObject,
            exceptionTypeBuf, exceptionTypeBufSize,
            exceptionMessageBuf, exceptionMessageBufSize,
            hresult))
    {
        return 1;
    }

    OBJECTREF throwable = pThread->GetThrowable();
    if (throwable == NULL)
        return 0;

    if (exceptionObject != NULL)
    {
        *exceptionObject = (uint64_t)(TADDR)OBJECTREFToObject(throwable);
    }

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

    if (hresult != NULL)
    {
        *hresult = ((EXCEPTIONREF)throwable)->GetHResult();
    }
    CopyStringRefToAscii(((EXCEPTIONREF)throwable)->GetMessage(), exceptionMessageBuf, exceptionMessageBufSize);
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
    uint64_t exceptionObject = 0;
    uint32_t exceptionHResult = 0;
    char exceptionType[128];
    exceptionType[0] = '\0';

    pThread->GetExceptionState()->TryGetPublishedCrashReportException(
        &exceptionObject,
        exceptionType, (int)ARRAY_SIZE(exceptionType),
        NULL, 0,
        &exceptionHResult);

    enumerateContext->threadCallback(
        (uint64_t)osThread,
        isCrashThread,
        exceptionObject,
        exceptionType,
        exceptionHResult,
        enumerateContext->callbackContext);

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
    uint64_t* exceptionObject,
    char* exceptionTypeBuf, int exceptionTypeBufSize,
    char* exceptionMsgBuf, int exceptionMsgBufSize,
    uint32_t* hresult)
{
    Thread* pThread = GetThreadAsyncSafe();
    if (pThread == NULL)
        return 0;

    return CrashReport_GetExceptionForThread(
        pThread,
        exceptionObject,
        exceptionTypeBuf, exceptionTypeBufSize,
        exceptionMsgBuf, exceptionMsgBufSize,
        hresult);
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

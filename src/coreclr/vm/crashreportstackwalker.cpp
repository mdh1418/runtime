// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// VM-side implementation of the in-proc crash report stack walker.
// Registered with the PAL crash reporter at startup.
// Uses MethodDesc::GetName() and metadata APIs that return cached const char*
// without allocation. These are NOT fully async-signal-safe (they read runtime
// data structures) but they do not allocate or acquire locks.

#include "common.h"
#include "method.hpp"
#include "codeman.h"

#ifdef HOST_ANDROID

#include "../pal/src/crashreport/inproccrashdump.h"

// Shared frame callback that resolves method info from a CrawlFrame.
// Used by both single-thread and multi-thread walkers.
struct WalkContext {
    InProcCrashDump_FrameCallback callback;
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
    PREGDISPLAY pRD = pCF->GetRegisterSet();
    if (pRD != NULL)
        ip = (uint64_t)GetControlPC(pRD);

    ctx->callback(ip, methodName, classNameBuf, moduleName, nativeOffset, (uint32_t)token, ctx->userCtx);
    return SWA_CONTINUE;
}

// Walk the managed stack of the current thread.
static void CrashReport_WalkStack(InProcCrashDump_FrameCallback frameCallback, void* ctx)
{
    Thread* pThread = GetThreadNULLOk();
    if (pThread == NULL)
        return;

    WalkContext walkCtx = { frameCallback, ctx };
    pThread->StackWalkFrames(FrameCallbackAdapter, &walkCtx,
        QUICKUNWIND | FUNCTIONSONLY | ALLOW_ASYNC_STACK_WALK);
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

// Get managed exception info from the current thread (legacy single-thread callback).
static int CrashReport_GetException(
    char* exceptionTypeBuf, int exceptionTypeBufSize,
    char* exceptionMsgBuf, int exceptionMsgBufSize,
    uint32_t* hresult)
{
    Thread* pThread = GetThreadNULLOk();
    if (pThread == NULL)
        return 0;

    exceptionMsgBuf[0] = '\0';
    return CrashReport_GetExceptionForThread(pThread, exceptionTypeBuf, exceptionTypeBufSize, hresult);
}

// Called during VM initialization to register callbacks with the PAL crash reporter
void CrashReport_RegisterStackWalker()
{
    InProcCrashDump_SetStackWalker(CrashReport_WalkStack);
    InProcCrashDump_SetExceptionResolver(CrashReport_GetException);
}

#endif // HOST_ANDROID
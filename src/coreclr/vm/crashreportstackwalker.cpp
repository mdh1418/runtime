// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// VM-side implementation of the in-proc crash report callbacks.
// Registered with the PAL crash reporter at startup.
//
// Important: crash-time thread discovery and per-thread exception / frame data
// are sourced from published snapshots so the signal handler can avoid live VM
// walks. The direct StackWalkFrames usage below is kept only as a best-effort
// fallback when no published snapshot is available yet.

#include "common.h"
#include "method.hpp"
#include "codeman.h"
#include "dbginterface.h"
#include "SignalSafeThreadMap.h"

#ifdef HOST_ANDROID

#include "../pal/src/crashreport/inproccrashreporter.h"

// Shared frame callback that resolves method info from a CrawlFrame.
// Used by both single-thread and multi-thread walkers.
struct WalkContext {
    InProcCrashReport_FrameCallback callback;
    void* userCtx;
};

struct PublishedStackSnapshotContext
{
    Thread::PublishedCrashReportFrame* frames;
    int frameCapacity;
    int frameCount;
    PEXCEPTION_POINTERS exceptionInfo;
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

static void CopyUtf8String(const char* source, char* destination, int destinationLength)
{
    if (destination == NULL || destinationLength <= 0)
    {
        return;
    }

    int index = 0;
    if (source != NULL)
    {
        while (source[index] != '\0' && index < destinationLength - 1)
        {
            destination[index] = source[index];
            index++;
        }
    }

    destination[index] = '\0';
}

static void AppendUtf8String(char* destination, int destinationLength, int* position, const char* source)
{
    if (destination == NULL || position == NULL || source == NULL)
    {
        return;
    }

    while (*source != '\0' && *position < destinationLength - 1)
    {
        destination[*position] = *source;
        (*position)++;
        source++;
    }
}

static void FormatGuidToString(const GUID& guid, char* destination, int destinationLength)
{
    if (destination == NULL || destinationLength <= 0)
    {
        return;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&guid);
    snprintf(
        destination,
        destinationLength,
        "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
        bytes[3], bytes[2], bytes[1], bytes[0],
        bytes[5], bytes[4], bytes[7], bytes[6],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
}

static DWORD GetPublishedIlOffset(MethodDesc* pMethodDesc, TADDR instructionPointer, DWORD nativeOffset)
{
    WRAPPER_NO_CONTRACT;

    if (pMethodDesc == NULL || instructionPointer == 0 || g_pDebugInterface == NULL)
    {
        return 0;
    }

    DWORD ilOffset = 0;
    if (!g_pDebugInterface->GetILOffsetFromNative(
            pMethodDesc,
            reinterpret_cast<LPCBYTE>(instructionPointer),
            nativeOffset,
            &ilOffset))
    {
        return 0;
    }

    return ilOffset;
}

static void PopulateModuleMetadata(
    Module* pModule,
    char* moduleName,
    int moduleNameLength,
    DWORD* timeStamp,
    DWORD* imageSize,
    char* mvid,
    int mvidLength)
{
    if (moduleName != NULL && moduleNameLength > 0)
    {
        moduleName[0] = '\0';
    }
    if (timeStamp != NULL)
    {
        *timeStamp = 0;
    }
    if (imageSize != NULL)
    {
        *imageSize = 0;
    }
    if (mvid != NULL && mvidLength > 0)
    {
        mvid[0] = '\0';
    }

    if (pModule == NULL)
    {
        return;
    }

    Assembly* pAssembly = pModule->GetAssembly();
    if (pAssembly != NULL)
    {
        CopyUtf8String(pAssembly->GetSimpleName(), moduleName, moduleNameLength);
    }

    PEAssembly* pPEAssembly = pModule->GetPEAssembly();
    if (pPEAssembly != NULL)
    {
        if (timeStamp != NULL)
        {
            *timeStamp = pPEAssembly->GetPEImageTimeDateStamp();
        }

        if (imageSize != NULL)
        {
            COUNT_T loadedImageSize = 0;
            pPEAssembly->GetLoadedImageContents(&loadedImageSize);
            *imageSize = loadedImageSize > UINT32_MAX ? UINT32_MAX : static_cast<DWORD>(loadedImageSize);
        }
    }

    IMDInternalImport* pImport = pModule->GetMDImport();
    if (pImport != NULL && mvid != NULL && mvidLength > 0)
    {
        GUID guid = {};
        if (SUCCEEDED(pImport->GetScopeProps(NULL, &guid)))
        {
            FormatGuidToString(guid, mvid, mvidLength);
        }
    }
}

static StackWalkAction PublishedFrameCallbackAdapter(CrawlFrame* pCF, VOID* pData)
{
    PublishedStackSnapshotContext* context = (PublishedStackSnapshotContext*)pData;
    if (context->frameCount >= context->frameCapacity)
    {
        return SWA_ABORT;
    }

    if (context->exceptionInfo != NULL)
    {
        PREGDISPLAY registerSet = pCF->GetRegisterSet();
        if (registerSet != NULL && GetRegdisplaySP(registerSet) < GetSP(context->exceptionInfo->ContextRecord))
        {
            return SWA_CONTINUE;
        }
    }

    MethodDesc* pMethodDesc = pCF->GetFunction();
    if (pMethodDesc == NULL)
    {
        return SWA_CONTINUE;
    }

    Thread::PublishedCrashReportFrame* frame = &context->frames[context->frameCount];
    memset(frame, 0, sizeof(Thread::PublishedCrashReportFrame));

    frame->Token = (DWORD)pMethodDesc->GetMemberDef();
    frame->NativeOffset = pCF->HasFaulted() ? 0 : pCF->GetRelOffset();

    PREGDISPLAY registerSet = pCF->GetRegisterSet();
    if (registerSet != NULL)
    {
        frame->InstructionPointer = (TADDR)GetControlPC(registerSet);
        frame->StackPointer = (TADDR)GetRegdisplaySP(registerSet);
    }
    frame->IlOffset = GetPublishedIlOffset(pMethodDesc, frame->InstructionPointer, frame->NativeOffset);

    CopyUtf8String(pMethodDesc->GetName(), frame->MethodName, Thread::CrashReportMethodNameLength);

    LPCUTF8 className = NULL;
    LPCUTF8 namespaceName = NULL;
    MethodTable* pMethodTable = pMethodDesc->GetMethodTable();
    if (pMethodTable != NULL)
    {
        mdTypeDef classToken = pMethodTable->GetCl();
        IMDInternalImport* pImport = pMethodDesc->GetMDImport();
        if (pImport != NULL && classToken != mdTypeDefNil)
        {
            pImport->GetNameOfTypeDef(classToken, &className, &namespaceName);
        }
    }

    int classNamePosition = 0;
    AppendUtf8String(frame->ClassName, Thread::CrashReportClassNameLength, &classNamePosition, namespaceName);
    if (namespaceName != NULL && namespaceName[0] != '\0' && className != NULL && className[0] != '\0')
    {
        AppendUtf8String(frame->ClassName, Thread::CrashReportClassNameLength, &classNamePosition, ".");
    }
    AppendUtf8String(frame->ClassName, Thread::CrashReportClassNameLength, &classNamePosition, className);
    frame->ClassName[classNamePosition] = '\0';

    Module* pModule = pMethodDesc->GetModule();
    PopulateModuleMetadata(
        pModule,
        frame->ModuleName,
        Thread::CrashReportModuleNameLength,
        &frame->TimeStamp,
        &frame->ImageSize,
        frame->Mvid,
        Thread::CrashReportModuleMvidLength);
    context->frameCount++;
    return SWA_CONTINUE;
}

static unsigned GetPublishedStackWalkFlags(Thread* pThread, PEXCEPTION_POINTERS pExceptionInfo)
{
    WRAPPER_NO_CONTRACT;

    unsigned flags = QUICKUNWIND | FUNCTIONSONLY | HANDLESKIPPEDFRAMES;
    if (pExceptionInfo != NULL || pThread != GetThreadNULLOk())
    {
        flags |= ALLOW_ASYNC_STACK_WALK | DISABLE_MISSING_FRAME_DETECTION | ALLOW_INVALID_OBJECTS;
    }

    return flags;
}

void Thread::RefreshPublishedCrashReportStackFrames(PEXCEPTION_POINTERS pExceptionInfo)
{
    WRAPPER_NO_CONTRACT;

    PublishedCrashReportFrame newFrames[CrashReportStackFrameCount];
    memset(newFrames, 0, sizeof(newFrames));

    PublishedStackSnapshotContext snapshotContext = { newFrames, CrashReportStackFrameCount, 0, pExceptionInfo };
    StackWalkFrames(PublishedFrameCallbackAdapter, &snapshotContext, GetPublishedStackWalkFlags(this, pExceptionInfo));

    // If the thread is currently executing native code, a fatal-time walk may
    // find no managed frames. Keep the previous published snapshot rather than
    // replacing useful data with an empty array.
    if (snapshotContext.frameCount == 0)
    {
        return;
    }

    __atomic_add_fetch(&m_crashReportStackFramesVersion, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&m_crashReportStackFramesCount, (ULONG)0, __ATOMIC_RELAXED);
    memset(m_crashReportStackFrames, 0, sizeof(m_crashReportStackFrames));
    memcpy(m_crashReportStackFrames, newFrames, snapshotContext.frameCount * sizeof(PublishedCrashReportFrame));
    __atomic_store_n(&m_crashReportStackFramesCount, (ULONG)snapshotContext.frameCount, __ATOMIC_RELAXED);
    __atomic_add_fetch(&m_crashReportStackFramesVersion, 1, __ATOMIC_RELEASE);
}

BOOL Thread::TryGetPublishedCrashReportStackFrames(PublishedCrashReportFrame* frames, int frameCapacity, int* frameCount)
{
    LIMITED_METHOD_CONTRACT;

    if (frameCount != NULL)
    {
        *frameCount = 0;
    }

    ULONG startVersion = __atomic_load_n(&m_crashReportStackFramesVersion, __ATOMIC_ACQUIRE);
    if ((startVersion & 1) != 0)
    {
        return FALSE;
    }

    ULONG localFrameCount = __atomic_load_n(&m_crashReportStackFramesCount, __ATOMIC_RELAXED);
    int copiedFrameCount = min((int)localFrameCount, frameCapacity);
    if (frames != NULL && copiedFrameCount > 0)
    {
        memcpy(frames, m_crashReportStackFrames, copiedFrameCount * sizeof(PublishedCrashReportFrame));
    }

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    ULONG endVersion = __atomic_load_n(&m_crashReportStackFramesVersion, __ATOMIC_ACQUIRE);
    if (startVersion != endVersion || (endVersion & 1) != 0)
    {
        if (frameCount != NULL)
        {
            *frameCount = 0;
        }
        return FALSE;
    }

    if (frameCount != NULL)
    {
        *frameCount = copiedFrameCount;
    }

    return copiedFrameCount != 0;
}

static BOOL WalkPublishedStackSnapshot(Thread* pThread, InProcCrashReport_FrameCallback frameCallback, void* context)
{
    static Thread::PublishedCrashReportFrame frames[Thread::CrashReportStackFrameCount];
    int frameCount = 0;
    if (!pThread->TryGetPublishedCrashReportStackFrames(frames, (int)ARRAY_SIZE(frames), &frameCount))
    {
        return FALSE;
    }

    for (int i = 0; i < frameCount; i++)
    {
        frameCallback(
            (uint64_t)frames[i].InstructionPointer,
            (uint64_t)frames[i].StackPointer,
            frames[i].MethodName,
            frames[i].ClassName,
            frames[i].ModuleName,
            frames[i].NativeOffset,
            frames[i].IlOffset,
            frames[i].Token,
            frames[i].TimeStamp,
            frames[i].ImageSize,
            frames[i].Mvid,
            context);
    }

    return TRUE;
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

    Module* pModule = pMD->GetModule();
    char moduleName[Thread::CrashReportModuleNameLength];
    char mvid[Thread::CrashReportModuleMvidLength];
    DWORD timeStamp = 0;
    DWORD imageSize = 0;
    PopulateModuleMetadata(
        pModule,
        moduleName,
        (int)ARRAY_SIZE(moduleName),
        &timeStamp,
        &imageSize,
        mvid,
        (int)ARRAY_SIZE(mvid));
    uint32_t nativeOffset = pCF->HasFaulted() ? 0 : pCF->GetRelOffset();
    uint32_t ilOffset = 0;

    uint64_t ip = 0;
    uint64_t stackPointer = 0;
    PREGDISPLAY pRD = pCF->GetRegisterSet();
    if (pRD != NULL)
    {
        ip = (uint64_t)GetControlPC(pRD);
        stackPointer = (uint64_t)GetRegdisplaySP(pRD);
    }
    ilOffset = GetPublishedIlOffset(pMD, (TADDR)ip, nativeOffset);

    ctx->callback(
        ip,
        stackPointer,
        methodName,
        classNameBuf,
        moduleName,
        nativeOffset,
        ilOffset,
        (uint32_t)token,
        timeStamp,
        imageSize,
        mvid,
        ctx->userCtx);
    return SWA_CONTINUE;
}

// Walk the current thread's published managed stack snapshot. If no snapshot is
// available yet, fall back to a live walk for the richer best-effort path.
static void CrashReport_WalkStack(InProcCrashReport_FrameCallback frameCallback, void* ctx)
{
    Thread* pThread = GetThreadAsyncSafe();
    if (pThread == NULL)
        return;

    if (WalkPublishedStackSnapshot(pThread, frameCallback, ctx))
    {
        return;
    }

    WalkContext walkCtx = { frameCallback, ctx };
    pThread->StackWalkFrames(FrameCallbackAdapter, &walkCtx,
        QUICKUNWIND | FUNCTIONSONLY | HANDLESKIPPEDFRAMES |
        DISABLE_MISSING_FRAME_DETECTION | ALLOW_ASYNC_STACK_WALK | ALLOW_INVALID_OBJECTS);
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
    InProcCrashReport_ThreadCallback threadCallback;
    InProcCrashReport_FrameCallback frameCallback;
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

    WalkPublishedStackSnapshot(pThread, enumerateContext->frameCallback, enumerateContext->callbackContext);
}

struct PublishThreadSnapshotsContext
{
    Thread* pCurrentThread;
};

static void PublishThreadSnapshotFromSignalSafeMap(size_t osThread, void* pThreadObject, void* context)
{
    (void)osThread;

    PublishThreadSnapshotsContext* publishContext = (PublishThreadSnapshotsContext*)context;
    Thread* pThread = (Thread*)pThreadObject;

    if (pThread == publishContext->pCurrentThread || pThread->PreemptiveGCDisabledOther())
    {
        return;
    }

    pThread->RefreshPublishedCrashReportStackFrames(NULL);
}

// Enumerate managed threads through the signal-safe thread map and replay the
// published per-thread snapshots. This avoids live ThreadStore / StackWalkFrames
// work from the crash signal path.
static void CrashReport_EnumerateThreads(
    uint64_t crashingTid,
    InProcCrashReport_ThreadCallback threadCallback,
    InProcCrashReport_FrameCallback frameCallback,
    void* ctx)
{
    EnumerateThreadsContext enumerateContext = { crashingTid, threadCallback, frameCallback, ctx };
    EnumerateThreadsInSignalSafeMap(EnumerateThreadFromSignalSafeMap, &enumerateContext);
}

void CrashReport_PublishThreadSnapshotsForFatalError(PEXCEPTION_POINTERS pExceptionInfo)
{
    WRAPPER_NO_CONTRACT;

    Thread* pCurrentThread = GetThreadNULLOk();
    if (pCurrentThread != NULL)
    {
        pCurrentThread->RefreshPublishedCrashReportStackFrames(pExceptionInfo);
    }

    PublishThreadSnapshotsContext publishContext = { pCurrentThread };
    EnumerateThreadsInSignalSafeMap(PublishThreadSnapshotFromSignalSafeMap, &publishContext);
}

static void CrashReport_PublishThreadSnapshots()
{
    WRAPPER_NO_CONTRACT;

    CrashReport_PublishThreadSnapshotsForFatalError(NULL);
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
    InProcCrashReport_SetThreadSnapshotPublisher(CrashReport_PublishThreadSnapshots);
    InProcCrashReport_SetCurrentThreadManagedResolver(CrashReport_IsCurrentThreadManaged);
}

#endif // HOST_ANDROID

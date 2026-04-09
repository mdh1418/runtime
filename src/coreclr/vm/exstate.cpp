// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//

//


#include "common.h"
#include "exstate.h"
#include "exinfo.h"

#ifdef _DEBUG
#include "comutilnative.h"      // for assertions only
#endif

namespace
{
void CopyUtf8String(const char* source, char* destination, int destinationLength)
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

void AppendUtf8String(char* destination, int destinationLength, int* position, const char* source)
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

void CopyStringRefToAscii(STRINGREF source, char* destination, int destinationLength)
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
}

OBJECTHANDLE ThreadExceptionState::GetThrowableAsHandle()
{
    WRAPPER_NO_CONTRACT;

    if (m_pCurrentTracker)
    {
        return m_pCurrentTracker->m_hThrowable;
    }

    return (OBJECTHANDLE)NULL;
}


ThreadExceptionState::ThreadExceptionState()
{
    m_pCurrentTracker = NULL;
    m_flag = TEF_None;
    m_crashReportExceptionVersion = 0;
    m_crashReportExceptionObject = 0;
    m_crashReportExceptionHResult = 0;
    m_crashReportExceptionType[0] = '\0';
    m_crashReportExceptionMessage[0] = '\0';

#ifndef TARGET_UNIX
    // Init the UE Watson BucketTracker
    m_UEWatsonBucketTracker.Init();
#endif // !TARGET_UNIX
}

ThreadExceptionState::~ThreadExceptionState()
{
#ifndef TARGET_UNIX
    // Init the UE Watson BucketTracker
    m_UEWatsonBucketTracker.ClearWatsonBucketDetails();
#endif // !TARGET_UNIX
}

#ifndef DACCESS_COMPILE

Thread* ThreadExceptionState::GetMyThread()
{
    return (Thread*)(((BYTE*)this) - offsetof(Thread, m_ExceptionState));
}


OBJECTREF ThreadExceptionState::GetThrowable()
{
    CONTRACTL
    {
        MODE_COOPERATIVE;
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

    if (m_pCurrentTracker && m_pCurrentTracker->m_hThrowable)
    {
        return ObjectFromHandle(m_pCurrentTracker->m_hThrowable);
    }

    return NULL;
}

void ThreadExceptionState::UpdatePublishedCrashReportException(OBJECTREF throwable)
{
    WRAPPER_NO_CONTRACT;

    __atomic_add_fetch(&m_crashReportExceptionVersion, 1, __ATOMIC_ACQ_REL);
    m_crashReportExceptionObject = 0;
    m_crashReportExceptionHResult = 0;
    m_crashReportExceptionType[0] = '\0';
    m_crashReportExceptionMessage[0] = '\0';

    if (throwable != NULL)
    {
        m_crashReportExceptionObject = (TADDR)OBJECTREFToObject(throwable);
        m_crashReportExceptionHResult = (DWORD)((EXCEPTIONREF)throwable)->GetHResult();

        MethodTable* pMethodTable = throwable->GetMethodTable();
        if (pMethodTable != NULL)
        {
            mdTypeDef classToken = pMethodTable->GetCl();
            Module* pModule = pMethodTable->GetModule();
            if (pModule != NULL)
            {
                IMDInternalImport* pImport = pModule->GetMDImport();
                if (pImport != NULL && classToken != mdTypeDefNil)
                {
                    LPCUTF8 className = NULL;
                    LPCUTF8 namespaceName = NULL;
                    pImport->GetNameOfTypeDef(classToken, &className, &namespaceName);

                    int position = 0;
                    AppendUtf8String(m_crashReportExceptionType, CrashReportExceptionTypeLength, &position, namespaceName);
                    if (namespaceName != NULL && namespaceName[0] != '\0' && className != NULL && className[0] != '\0')
                    {
                        AppendUtf8String(m_crashReportExceptionType, CrashReportExceptionTypeLength, &position, ".");
                    }
                    AppendUtf8String(m_crashReportExceptionType, CrashReportExceptionTypeLength, &position, className);
                    m_crashReportExceptionType[position] = '\0';
                }
            }
        }

        CopyStringRefToAscii(((EXCEPTIONREF)throwable)->GetMessage(), m_crashReportExceptionMessage, CrashReportExceptionMessageLength);
    }

    __atomic_add_fetch(&m_crashReportExceptionVersion, 1, __ATOMIC_RELEASE);
}

BOOL ThreadExceptionState::TryGetPublishedCrashReportException(
    uint64_t* objectAddress,
    char* exceptionTypeBuf, int exceptionTypeBufSize,
    char* exceptionMessageBuf, int exceptionMessageBufSize,
    uint32_t* hresult)
{
    LIMITED_METHOD_CONTRACT;

    if (objectAddress != NULL)
    {
        *objectAddress = 0;
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

    ULONG startVersion = __atomic_load_n(&m_crashReportExceptionVersion, __ATOMIC_ACQUIRE);
    if ((startVersion & 1) != 0)
    {
        return FALSE;
    }

    TADDR localObject = m_crashReportExceptionObject;
    DWORD localHResult = m_crashReportExceptionHResult;

    if (objectAddress != NULL)
    {
        *objectAddress = (uint64_t)localObject;
    }

    if (hresult != NULL)
    {
        *hresult = localHResult;
    }

    CopyUtf8String(m_crashReportExceptionType, exceptionTypeBuf, exceptionTypeBufSize);
    CopyUtf8String(m_crashReportExceptionMessage, exceptionMessageBuf, exceptionMessageBufSize);

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    ULONG endVersion = __atomic_load_n(&m_crashReportExceptionVersion, __ATOMIC_ACQUIRE);
    if (startVersion != endVersion || (endVersion & 1) != 0)
    {
        return FALSE;
    }

    return localObject != 0 || (exceptionTypeBuf != NULL && exceptionTypeBuf[0] != '\0');
}

void ThreadExceptionState::SetThrowable(OBJECTREF throwable DEBUG_ARG(SetThrowableErrorChecking stecFlags))
{
    CONTRACTL
    {
        if ((throwable == NULL) || CLRException::IsPreallocatedExceptionObject(throwable)) NOTHROW; else THROWS; // From CreateHandle
        GC_NOTRIGGER;
        if (throwable == NULL) MODE_ANY; else MODE_COOPERATIVE;
    }
    CONTRACTL_END;

    if (m_pCurrentTracker)
    {
        m_pCurrentTracker->DestroyExceptionHandle();
    }

    if (throwable != NULL)
    {
        // Non-compliant exceptions are always wrapped.
        // The use of the ExceptionNative:: helper here (rather than the global ::IsException helper)
        // is hokey, but we need a GC_NOTRIGGER version and it's only for an ASSERT.
        _ASSERTE(IsException(throwable->GetMethodTable()));

        OBJECTHANDLE hNewThrowable;

        // If we're tracking one of the preallocated exception objects, then just use the global handle that
        // matches it rather than creating a new one.
        if (CLRException::IsPreallocatedExceptionObject(throwable))
        {
            hNewThrowable = CLRException::GetPreallocatedHandleForObject(throwable);
        }
        else
        {
            AppDomain* pDomain = AppDomain::GetCurrentDomain();
            _ASSERTE(pDomain != NULL);
            hNewThrowable = pDomain->CreateHandle(throwable);
        }

#ifdef _DEBUG
        //
        // Fatal stack overflow policy ends up short-circuiting the normal exception handling
        // flow such that there could be no Tracker for this SO that is in flight.  In this
        // situation there is no place to store the throwable in the exception state, and instead
        // it is presumed that the handle to the SO exception is elsewhere.  (Current knowledge
        // as of 7/15/05 is that it is stored in Thread::m_LastThrownObjectHandle;
        //
        if (stecFlags != STEC_CurrentTrackerEqualNullOkHackForFatalStackOverflow)
        {
            CONSISTENCY_CHECK(CheckPointer(m_pCurrentTracker));
        }
#endif

        if (m_pCurrentTracker != NULL)
        {
            m_pCurrentTracker->m_hThrowable = hNewThrowable;
        }
    }

    UpdatePublishedCrashReportException(throwable);
}

DWORD ThreadExceptionState::GetExceptionCode()
{
    LIMITED_METHOD_CONTRACT;

    _ASSERTE(m_pCurrentTracker);
    return m_pCurrentTracker->m_ExceptionCode;
}

BOOL ThreadExceptionState::IsComPlusException()
{
    STATIC_CONTRACT_NOTHROW;
    STATIC_CONTRACT_GC_NOTRIGGER;
    STATIC_CONTRACT_FORBID_FAULT;

    if (GetExceptionCode() != EXCEPTION_COMPLUS)
    {
        return FALSE;
    }

    _ASSERTE(IsInstanceTaggedSEHCode(GetExceptionCode()));



    return GetFlags()->WasThrownByUs();
}


#endif // !DACCESS_COMPILE

BOOL ThreadExceptionState::IsExceptionInProgress()
{
    LIMITED_METHOD_DAC_CONTRACT;

    return (m_pCurrentTracker != NULL);
}

#if !defined(DACCESS_COMPILE)

EXCEPTION_POINTERS* ThreadExceptionState::GetExceptionPointers()
{
    LIMITED_METHOD_CONTRACT;

    if (m_pCurrentTracker)
    {
        return (EXCEPTION_POINTERS*)&(m_pCurrentTracker->m_ptrs);
    }
    else
    {
        return NULL;
    }
}

#endif // !DACCESS_COMPILE

PTR_EXCEPTION_RECORD ThreadExceptionState::GetExceptionRecord()
{
    LIMITED_METHOD_DAC_CONTRACT;

    if (m_pCurrentTracker)
    {
        return m_pCurrentTracker->m_ptrs.ExceptionRecord;
    }
    else
    {
        return NULL;
    }
}

PTR_CONTEXT ThreadExceptionState::GetContextRecord()
{
    LIMITED_METHOD_DAC_CONTRACT;

    if (m_pCurrentTracker)
    {
        return m_pCurrentTracker->m_ptrs.ContextRecord;
    }
    else
    {
        return NULL;
    }
}

ExceptionFlags* ThreadExceptionState::GetFlags()
{

    if (m_pCurrentTracker)
    {
        return &(m_pCurrentTracker->m_ExceptionFlags);
    }
    else
    {
        _ASSERTE(!"GetFlags() called when there is no current exception");
        return NULL;
    }

}

#if !defined(DACCESS_COMPILE)

#ifdef DEBUGGING_SUPPORTED
static DebuggerExState   s_emptyDebuggerExState;

DebuggerExState*    ThreadExceptionState::GetDebuggerState()
{
    if (m_pCurrentTracker)
    {
        return &(m_pCurrentTracker->m_DebuggerExState);
    }
    else
    {
        _ASSERTE(!"unexpected use of GetDebuggerState() when no exception in flight");
        return &s_emptyDebuggerExState;
    }
}

void ThreadExceptionState::SetDebuggerIndicatedFramePointer(LPVOID indicatedFramePointer)
{
    WRAPPER_NO_CONTRACT;
    if (m_pCurrentTracker)
    {
        m_pCurrentTracker->m_DebuggerExState.SetDebuggerIndicatedFramePointer(indicatedFramePointer);
    }
    else
    {
        _ASSERTE(!"unexpected use of SetDebuggerIndicatedFramePointer() when no exception in flight");
    }
}

BOOL ThreadExceptionState::IsDebuggerInterceptable()
{
    LIMITED_METHOD_CONTRACT;
    DWORD ExceptionCode = GetExceptionCode();
    return (BOOL)((ExceptionCode != STATUS_STACK_OVERFLOW) &&
                  (ExceptionCode != EXCEPTION_BREAKPOINT) &&
                  (ExceptionCode != EXCEPTION_SINGLE_STEP) &&
                  !GetFlags()->UnwindHasStarted() &&
                  !GetFlags()->DebuggerInterceptNotPossible());
}

#ifdef TARGET_X86
PEXCEPTION_REGISTRATION_RECORD GetClrSEHRecordServicingStackPointer(Thread *pThread, void *pStackPointer);
#endif // TARGET_X86

//---------------------------------------------------------------------------------------
//
// This function is called by the debugger to store information necessary to intercept the current exception.
// This information is consumed by the EH subsystem to start the unwind and resume execution without
// finding and executing a catch clause.
//
// Arguments:
//    pJitManager   - the JIT manager for the method where we are going to intercept the exception
//    pThread       - the thread on which the interception is taking place
//    methodToken   - the MethodDef token of the interception method
//    pFunc         - the MethodDesc of the interception method
//    natOffset     - the native offset at which we are going to resume execution
//    sfDebuggerInterceptFramePointer
//                  - the frame pointer of the interception method frame
//    pFlags        - flags on the current exception (ExInfo);
//                    to be set by this function to indicate that an interception is going on
//
// Return Value:
//    whether the operation is successful
//

BOOL DebuggerExState::SetDebuggerInterceptInfo(IJitManager *pJitManager,
                                      Thread *pThread,
                                      const METHODTOKEN& methodToken,
                                      MethodDesc *pFunc,
                                      ULONG_PTR natOffset,
                                      StackFrame sfDebuggerInterceptFramePointer,
                                      ExceptionFlags* pFlags)
{
    WRAPPER_NO_CONTRACT;

    //
    // Verify parameters are non-NULL
    //
    if ((pJitManager == NULL) ||
        (pThread == NULL) ||
        (methodToken.IsNull()) ||
        (pFunc == NULL) ||
        (natOffset == (TADDR)0) ||
        (sfDebuggerInterceptFramePointer.IsNull()))
    {
        return FALSE;
    }

    //
    // You can only call this function on the currently active exception.
    //
    if (this != pThread->GetExceptionState()->GetDebuggerState())
    {
        return FALSE;
    }

    //
    // Check that the stack pointer is less than as far as we have searched so far.
    //
    if (sfDebuggerInterceptFramePointer > m_sfDebuggerIndicatedFramePointer)
    {
        return FALSE;
    }

    int nestingLevel = 0;

    //
    // These values will override the normal information used by the EH subsystem to handle the exception.
    // They are retrieved by GetDebuggerInterceptInfo().
    //
    m_pDebuggerInterceptFunc = pFunc;
    m_dDebuggerInterceptHandlerDepth  = nestingLevel;
    m_sfDebuggerInterceptFramePointer = sfDebuggerInterceptFramePointer;
    m_pDebuggerInterceptNativeOffset  = natOffset;

    // set a flag on the exception tracking struct to indicate that an interception is in progress
    pFlags->SetDebuggerInterceptInfo();
    return TRUE;
}
#endif // DEBUGGING_SUPPORTED

#endif // DACCESS_COMPILE

EHClauseInfo* ThreadExceptionState::GetCurrentEHClauseInfo()
{
    if (m_pCurrentTracker)
    {
        return &(m_pCurrentTracker->m_EHClauseInfo);
    }
    else
    {
        _ASSERTE(!"unexpected use of GetCurrentEHClauseInfo() when no exception in flight");
#if defined(_MSC_VER)
        #pragma warning(disable : 4640)
#endif // defined(_MSC_VER)

        static EHClauseInfo m_emptyEHClauseInfo;

#if defined(_MSC_VER)
        #pragma warning(default : 4640)
#endif // defined(_MSC_VER)

        return &m_emptyEHClauseInfo;
    }
}

void ThreadExceptionState::SetThreadExceptionFlag(ThreadExceptionFlag flag)
{
    LIMITED_METHOD_CONTRACT;

    m_flag = (ThreadExceptionFlag)((DWORD)m_flag | flag);
}

void ThreadExceptionState::ResetThreadExceptionFlag(ThreadExceptionFlag flag)
{
    LIMITED_METHOD_CONTRACT;

    m_flag = (ThreadExceptionFlag)((DWORD)m_flag & ~flag);
}

BOOL ThreadExceptionState::HasThreadExceptionFlag(ThreadExceptionFlag flag)
{
    LIMITED_METHOD_CONTRACT;

    return ((DWORD)m_flag & flag);
}

ThreadExceptionFlagHolder::ThreadExceptionFlagHolder(ThreadExceptionState::ThreadExceptionFlag flag)
{
    WRAPPER_NO_CONTRACT;

    Thread* pThread = GetThread();
    m_pExState = pThread->GetExceptionState();

    m_flag = flag;
    m_pExState->SetThreadExceptionFlag(m_flag);
}

ThreadExceptionFlagHolder::~ThreadExceptionFlagHolder()
{
    WRAPPER_NO_CONTRACT;

    _ASSERTE(m_pExState);
    m_pExState->ResetThreadExceptionFlag(m_flag);
}

#ifdef DACCESS_COMPILE

void
ThreadExceptionState::EnumChainMemoryRegions(CLRDataEnumMemoryFlags flags)
{
    ExInfo*           head = m_pCurrentTracker;

    if (head == NULL)
    {
        return;
    }

    for (;;)
    {
        head->EnumMemoryRegions(flags);

        if (!head->m_pPrevNestedInfo.IsValid())
        {
            break;
        }

        head->m_pPrevNestedInfo.EnumMem();
        head = head->m_pPrevNestedInfo;
    }
}


#endif // DACCESS_COMPILE




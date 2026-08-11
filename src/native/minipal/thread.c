// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "thread.h"

#if !defined(__wasm) || defined(_REENTRANT)
PLATFORM_THREAD_LOCAL size_t minipal_cached_thread_id;
#endif

size_t minipal_get_current_thread_id_no_cache(void)
{
    size_t tid;
#if defined(__wasm) && !defined(_REENTRANT)
    tid = 1; // In non-reentrant WASM builds, define a single thread with ID 1.
#else // !__wasm || _REENTRANT

#if defined(__linux__)
    tid = (size_t)syscall(SYS_gettid);
#elif defined(__APPLE__)
    uint64_t thread_id;
    pthread_threadid_np(pthread_self(), &thread_id);
    tid = (size_t)thread_id;
#elif defined(__FreeBSD__)
    tid = (size_t)pthread_getthreadid_np();
#elif defined(__NetBSD__)
    tid = (size_t)_lwp_self();
#elif defined(__OpenBSD__)
    tid = (size_t)getthrid();
#elif defined(__HAIKU__)
    tid = (size_t)find_thread(NULL);
#elif defined(__sun)
    tid = (size_t)pthread_self();
#elif defined(__wasm)
    tid = (size_t)(void*)pthread_self();
#else
#error "Unsupported platform"
#endif

#endif // __wasm && !_REENTRANT
    return tid;
}

size_t minipal_get_current_thread_id(void)
{
#if defined(__wasm) && !defined(_REENTRANT)
    return minipal_get_current_thread_id_no_cache();

#else // !__wasm || _REENTRANT
    if (!minipal_cached_thread_id)
    {
        minipal_cached_thread_id = minipal_get_current_thread_id_no_cache();
    }

    return minipal_cached_thread_id;
#endif // __wasm && !_REENTRANT
}

int minipal_set_thread_name(pthread_t thread, const char* name)
{
#ifdef __wasm
    // WASM does not support pthread_setname_np yet
    return 0;
#else
    const char* threadName = name;
    char truncatedName[MINIPAL_MAX_THREAD_NAME_LENGTH + 1];

    if (strlen(name) > MINIPAL_MAX_THREAD_NAME_LENGTH)
    {
        strncpy(truncatedName, name, MINIPAL_MAX_THREAD_NAME_LENGTH);
        truncatedName[MINIPAL_MAX_THREAD_NAME_LENGTH] = '\0';
        threadName = truncatedName;
    }

#if defined(__APPLE__)
    // On Apple OSes, pthread_setname_np only works for the calling thread.
    if (thread != pthread_self()) return 0;

    return pthread_setname_np(threadName);
#elif defined(__OpenBSD__)
    pthread_set_name_np(thread, threadName);
    return 0;
#elif defined(__HAIKU__)
    return rename_thread(get_pthread_thread_id(thread), threadName);
#else
    return pthread_setname_np(thread, threadName);
#endif
#endif
}

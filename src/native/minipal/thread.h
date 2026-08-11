// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef HAVE_MINIPAL_THREAD_H
#define HAVE_MINIPAL_THREAD_H

#ifndef HOST_WINDOWS

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <minipal/utils.h>

#if defined(__linux__)
#include <unistd.h>
#include <sys/syscall.h>
#elif defined(__FreeBSD__)
#include <pthread_np.h>
#elif defined(__OpenBSD__)
#include <unistd.h>
#include <pthread_np.h>
#elif defined(__NetBSD__)
#include <lwp.h>
#elif defined(__HAIKU__)
#include <kernel/OS.h>
#endif

#ifdef PTHREAD_MAX_NAMELEN_NP
#define MINIPAL_MAX_THREAD_NAME_LENGTH (PTHREAD_MAX_NAMELEN_NP - 1)
#elif defined(__APPLE__)
#define MINIPAL_MAX_THREAD_NAME_LENGTH 63
#elif defined(__FreeBSD__)
#define MINIPAL_MAX_THREAD_NAME_LENGTH MAXCOMLEN
#elif defined(__HAIKU__)
#define MINIPAL_MAX_THREAD_NAME_LENGTH (B_OS_NAME_LENGTH - 1)
#else
#define MINIPAL_MAX_THREAD_NAME_LENGTH 15
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Function implementations moved to thread.c to preserve a single external definition and call-stack visibility. */

/* Prototypes: implementations live in thread.c */
inline size_t minipal_get_current_thread_id_no_cache(void);

#if !defined(__wasm) || defined(_REENTRANT)
extern PLATFORM_THREAD_LOCAL size_t minipal_cached_thread_id;
#endif

inline size_t minipal_get_current_thread_id(void);
inline int minipal_set_thread_name(pthread_t thread, const char* name);


#ifdef __cplusplus
}
#endif // extern "C"

#endif // !HOST_WINDOWS

#endif // HAVE_MINIPAL_THREAD_H

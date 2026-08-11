// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef HAVE_MINIPAL_GETEXEPATH_H
#define HAVE_MINIPAL_GETEXEPATH_H

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__FreeBSD__)
#include <string.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#elif defined(__OpenBSD__)
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#elif defined(_WIN32)
#include <windows.h>
#elif defined(__HAIKU__)
#include <FindDirectory.h>
#include <StorageDefs.h>
#elif defined(TARGET_WASI)
#include <string.h>
#elif HAVE_GETAUXVAL
#include <sys/auxv.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Implementation moved to getexepath.c to preserve a single external definition and call-stack visibility. */

/* Prototype: implementation in getexepath.c */
char* minipal_getexepath(void);


#ifdef __cplusplus
}
#endif // extern "C"

#endif // HAVE_MINIPAL_GETEXEPATH_H

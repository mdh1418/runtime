// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef __CRASHREPORTSTACKWALKER_H__
#define __CRASHREPORTSTACKWALKER_H__

#include "common.h"

#ifdef HOST_ANDROID

void CrashReport_PublishThreadSnapshotsForFatalError(PEXCEPTION_POINTERS pExceptionInfo);
void CrashReport_RegisterStackWalker();

#endif // HOST_ANDROID

#endif // __CRASHREPORTSTACKWALKER_H__

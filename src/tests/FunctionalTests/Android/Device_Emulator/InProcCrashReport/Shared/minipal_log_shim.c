// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Test-only definition of minipal_log_write for the recompiled in-proc crash
// reporter. The real implementation lives in minipal/log.c behind a generated
// minipalconfig.h that is not available to the functional-test app build, so a
// tiny async-safe stderr/logcat writer is provided instead. Only minipal_log_write
// (used for initialization-failure logging and the non-Android console sink) needs
// a real definition; the flush/sync entry points are inert.

#include <minipal/log.h>

#include <string.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

int minipal_log_write(minipal_log_flags flags, const char* msg)
{
    (void)flags;
    if (msg == NULL)
    {
        return 0;
    }
#if defined(__ANDROID__)
    __android_log_write(ANDROID_LOG_ERROR, "DOTNET", msg);
#endif
    size_t len = strlen(msg);
    return (int)write(STDERR_FILENO, msg, len);
}

void minipal_log_flush(minipal_log_flags flags) { (void)flags; }
void minipal_log_flush_all(void) {}
void minipal_log_sync(minipal_log_flags flags) { (void)flags; }
void minipal_log_sync_all(void) {}

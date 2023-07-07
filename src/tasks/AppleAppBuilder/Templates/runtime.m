// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#import <Foundation/Foundation.h>
#include <mono/utils/mono-publib.h>
#include <mono/utils/mono-logger.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/class.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/mono-gc.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/object.h>
#include <mono/jit/jit.h>
#include <mono/jit/mono-private-unstable.h>
#include <TargetConditionals.h>
#import <os/log.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <stdio.h>

#import "util.h"

static char *bundle_path;

#define APPLE_RUNTIME_IDENTIFIER "//%APPLE_RUNTIME_IDENTIFIER%"

const char *
get_bundle_path (void)
{
    if (bundle_path)
        return bundle_path;
    NSBundle* main_bundle = [NSBundle mainBundle];
    NSString* path = [main_bundle bundlePath];

#if TARGET_OS_MACCATALYST
    path = [path stringByAppendingString:@"/Contents/Resources"];
#endif

    bundle_path = strdup ([path UTF8String]);

    return bundle_path;
}

static void
register_dllmap (void)
{
//%DllMap%
}

void
mono_ios_runtime_init (void)
{
    setenv ("MONO_LOG_LEVEL", "debug", TRUE);
    setenv ("MONO_LOG_MASK", "all", TRUE);

    const char* bundle = get_bundle_path ();
    chdir (bundle);

    char icu_dat_path [1024];
    int res;
#if defined(HYBRID_GLOBALIZATION)
    res = snprintf (icu_dat_path, sizeof (icu_dat_path) - 1, "%s/%s", bundle, "icudt_hybrid.dat");
#else
    res = snprintf (icu_dat_path, sizeof (icu_dat_path) - 1, "%s/%s", bundle, "icudt.dat");
#endif
    assert (res > 0);

    // TODO: set TRUSTED_PLATFORM_ASSEMBLIES, APP_PATHS and NATIVE_DLL_SEARCH_DIRECTORIES
    const char *appctx_keys [] = {
        "RUNTIME_IDENTIFIER",
        "APP_CONTEXT_BASE_DIRECTORY",
#if !defined(INVARIANT_GLOBALIZATION)
        "ICU_DAT_FILE_PATH"
#endif
    };
    const char *appctx_values [] = {
        APPLE_RUNTIME_IDENTIFIER,
        bundle,
#if !defined(INVARIANT_GLOBALIZATION)
        icu_dat_path
#endif
    };

    monovm_initialize (sizeof (appctx_keys) / sizeof (appctx_keys [0]), appctx_keys, appctx_values);

    // Print this so apps parsing logs can detect when we exited
    os_log_info (OS_LOG_DEFAULT, EXIT_CODE_TAG ": %d", 42);

    exit (42);
}

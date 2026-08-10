// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef HAVE_MINIPAL_ENTRYPOINTS_H
#define HAVE_MINIPAL_ENTRYPOINTS_H

#include <stdint.h>
#include <string.h>
#include <minipal/utils.h>

typedef struct
{
    const char* name;
    const void* method;
} Entry;

// expands to:      {"impl", (void*)&impl},
#define DllImportEntry(impl) \
    {#impl, (void*)&impl},

#if defined(MINIPAL_ENTRYPOINTS_SOURCE)
#define MINIPAL_ENTRYPOINTS_INLINE
#else
#define MINIPAL_ENTRYPOINTS_INLINE inline
#endif

MINIPAL_ENTRYPOINTS_INLINE const void* minipal_resolve_dllimport(const Entry* resolutionTable, size_t tableLength, const char* name)
{
    for (size_t i = 0; i < tableLength; i++)
    {
        if (strcmp(name, resolutionTable[i].name) == 0)
        {
            return resolutionTable[i].method;
        }
    }

    return NULL;
}

#undef MINIPAL_ENTRYPOINTS_INLINE

#endif // HAVE_MINIPAL_ENTRYPOINTS_H

// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#define MINIPAL_ENTRYPOINTS_SOURCE
#include "entrypoints.h"

const void* minipal_resolve_dllimport(const Entry* resolutionTable, size_t tableLength, const char* name)
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

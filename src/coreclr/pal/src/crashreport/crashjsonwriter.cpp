// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Async-signal-safe JSON writer implementation.
// Every function here uses only stack variables and the pre-allocated buffer.
// No malloc, no stdio, no locks — safe to call from a signal handler.

#include "crashjsonwriter.h"
#include <string.h>
#include <stdio.h>  // snprintf only — snprintf with stack buffer IS signal-safe

// Append raw bytes to buffer. Returns 0 if out of space.
static int CrashJson_Append(CrashJsonWriter* w, const char* str, int len)
{
    if (w->pos + len >= CRASH_JSON_BUFFER_SIZE - 16)  // Reserve minimal space for closing
        return 0;
    memcpy(w->buffer + w->pos, str, len);
    w->pos += len;
    return 1;
}

static int CrashJson_AppendStr(CrashJsonWriter* w, const char* str)
{
    int len = 0;
    while (str[len]) len++;
    return CrashJson_Append(w, str, len);
}

static void CrashJson_WriteSeparator(CrashJsonWriter* w)
{
    if (w->commaNeeded)
        CrashJson_AppendStr(w, ",");
    w->commaNeeded = 1;
}

// Escape a string value for JSON. Handles \, ", and control characters.
static void CrashJson_WriteEscapedString(CrashJsonWriter* w, const char* str)
{
    CrashJson_AppendStr(w, "\"");
    if (str != NULL)
    {
        for (int i = 0; str[i]; i++)
        {
            char c = str[i];
            if (c == '"')
                CrashJson_AppendStr(w, "\\\"");
            else if (c == '\\')
                CrashJson_AppendStr(w, "\\\\");
            else if (c == '\n')
                CrashJson_AppendStr(w, "\\n");
            else if (c == '\r')
                CrashJson_AppendStr(w, "\\r");
            else if (c == '\t')
                CrashJson_AppendStr(w, "\\t");
            else if ((unsigned char)c < 0x20)
            {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                CrashJson_AppendStr(w, esc);
            }
            else
            {
                CrashJson_Append(w, &c, 1);
            }
        }
    }
    CrashJson_AppendStr(w, "\"");
}

void CrashJson_Init(CrashJsonWriter* w)
{
    w->pos = 0;
    w->depth = 0;
    w->commaNeeded = 0;
    w->buffer[0] = '\0';
}

void CrashJson_OpenObject(CrashJsonWriter* w, const char* key)
{
    CrashJson_WriteSeparator(w);
    if (key != NULL)
    {
        CrashJson_WriteEscapedString(w, key);
        CrashJson_AppendStr(w, ": ");
    }
    CrashJson_AppendStr(w, "{");
    w->depth++;
    w->commaNeeded = 0;
}

void CrashJson_CloseObject(CrashJsonWriter* w)
{
    w->depth--;
    CrashJson_AppendStr(w, "}");
    w->commaNeeded = 1;
}

void CrashJson_OpenArray(CrashJsonWriter* w, const char* key)
{
    CrashJson_WriteSeparator(w);
    if (key != NULL)
    {
        CrashJson_WriteEscapedString(w, key);
        CrashJson_AppendStr(w, ": ");
    }
    CrashJson_AppendStr(w, "[");
    w->depth++;
    w->commaNeeded = 0;
}

void CrashJson_CloseArray(CrashJsonWriter* w)
{
    w->depth--;
    CrashJson_AppendStr(w, "]");
    w->commaNeeded = 1;
}

void CrashJson_WriteString(CrashJsonWriter* w, const char* key, const char* value)
{
    CrashJson_WriteSeparator(w);
    CrashJson_WriteEscapedString(w, key);
    CrashJson_AppendStr(w, ": ");
    CrashJson_WriteEscapedString(w, value);
}

void CrashJson_WriteInt(CrashJsonWriter* w, const char* key, int64_t value)
{
    CrashJson_WriteSeparator(w);
    CrashJson_WriteEscapedString(w, key);
    char buf[32];
    snprintf(buf, sizeof(buf), ": %lld", (long long)value);
    CrashJson_AppendStr(w, buf);
}

void CrashJson_WriteHex(CrashJsonWriter* w, const char* key, uint64_t value)
{
    CrashJson_WriteSeparator(w);
    CrashJson_WriteEscapedString(w, key);
    char buf[32];
    snprintf(buf, sizeof(buf), ": \"0x%llx\"", (unsigned long long)value);
    CrashJson_AppendStr(w, buf);
}

void CrashJson_WriteBool(CrashJsonWriter* w, const char* key, int value)
{
    CrashJson_WriteSeparator(w);
    CrashJson_WriteEscapedString(w, key);
    CrashJson_AppendStr(w, value ? ": true" : ": false");
}

int CrashJson_GetLength(CrashJsonWriter* w)
{
    return w->pos;
}

const char* CrashJson_GetBuffer(CrashJsonWriter* w)
{
    w->buffer[w->pos] = '\0';
    return w->buffer;
}

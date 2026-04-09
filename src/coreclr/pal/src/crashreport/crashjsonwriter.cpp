// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Async-signal-safe JSON writer implementation.
// Every function here uses only stack variables and the pre-allocated buffer.
// No malloc, no stdio, no locks — safe to call from a signal handler.

#include "crashjsonwriter.h"
#include <string.h>

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

static char ToHexChar(unsigned value)
{
    return (value < 10) ? (char)('0' + value) : (char)('a' + (value - 10));
}

static void CrashJson_AppendUnsignedDecimal(CrashJsonWriter* w, uint64_t value)
{
    char buf[32];
    int pos = 0;

    if (value == 0)
    {
        buf[pos++] = '0';
    }
    else
    {
        char reverse[32];
        int reversePos = 0;
        while (value != 0 && reversePos < (int)sizeof(reverse))
        {
            reverse[reversePos++] = (char)('0' + (value % 10));
            value /= 10;
        }
        while (reversePos > 0)
        {
            buf[pos++] = reverse[--reversePos];
        }
    }

    CrashJson_Append(w, buf, pos);
}

static void CrashJson_AppendSignedDecimal(CrashJsonWriter* w, int64_t value)
{
    if (value < 0)
    {
        CrashJson_Append(w, "-", 1);
        uint64_t magnitude = (uint64_t)(-(value + 1)) + 1;
        CrashJson_AppendUnsignedDecimal(w, magnitude);
    }
    else
    {
        CrashJson_AppendUnsignedDecimal(w, (uint64_t)value);
    }
}

static void CrashJson_AppendHexValue(CrashJsonWriter* w, uint64_t value)
{
    char buf[32];
    int pos = 0;
    int started = 0;

    buf[pos++] = '0';
    buf[pos++] = 'x';

    for (int shift = (int)(sizeof(uint64_t) * 8) - 4; shift >= 0; shift -= 4)
    {
        unsigned digit = (unsigned)((value >> shift) & 0xF);
        if (digit != 0 || started || shift == 0)
        {
            buf[pos++] = ToHexChar(digit);
            started = 1;
        }
    }

    CrashJson_Append(w, buf, pos);
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
                char esc[7];
                esc[0] = '\\';
                esc[1] = 'u';
                esc[2] = '0';
                esc[3] = '0';
                esc[4] = ToHexChar(((unsigned char)c >> 4) & 0xF);
                esc[5] = ToHexChar((unsigned char)c & 0xF);
                esc[6] = '\0';
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
    CrashJson_AppendStr(w, ": ");
    CrashJson_AppendSignedDecimal(w, value);
}

void CrashJson_WriteHex(CrashJsonWriter* w, const char* key, uint64_t value)
{
    CrashJson_WriteSeparator(w);
    CrashJson_WriteEscapedString(w, key);
    CrashJson_AppendStr(w, ": \"");
    CrashJson_AppendHexValue(w, value);
    CrashJson_AppendStr(w, "\"");
}

void CrashJson_WriteBool(CrashJsonWriter* w, const char* key, int value)
{
    CrashJson_WriteSeparator(w);
    CrashJson_WriteEscapedString(w, key);
    CrashJson_AppendStr(w, value ? ": \"true\"" : ": \"false\"");
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

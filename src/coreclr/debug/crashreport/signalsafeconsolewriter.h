// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Bounded, signal-safe line-oriented console writer. Paired with
// SignalSafeJsonWriter as the second crash-report output sink:
// SignalSafeJsonWriter writes JSON to a file callback;
// SignalSafeConsoleWriter writes one line at a time to the platform console
// (Android logcat under tag "DOTNET_CRASH"; stderr elsewhere). All public
// members are async-signal-safe: no heap allocation, no stdio, no locale
// or variadic formatting.
//
// Each EndLine() / WriteLine() call produces exactly one platform log entry
// on Android (so per-line filtering works) and one '\n'-terminated chunk on
// Apple/Linux stderr. Best-effort: a per-line buffer overflow or short
// platform write is silently dropped and the next line begins fresh, so a
// console hiccup never fails any other crash-report output.

#pragma once

#include <stddef.h>
#include <stdint.h>

static constexpr size_t SIGNAL_SAFE_CONSOLE_BUFFER_SIZE = 512;

class SignalSafeConsoleWriter
{
public:
    SignalSafeConsoleWriter()
        : m_pos(0)
    {
        m_buffer[0] = '\0';
    }

    SignalSafeConsoleWriter(const SignalSafeConsoleWriter&) = delete;
    SignalSafeConsoleWriter& operator=(const SignalSafeConsoleWriter&) = delete;

    void AppendStr(const char* s);
    void AppendChar(char c);
    void AppendHex(uint64_t v);
    void AppendDecimal(uint64_t v);
    void AppendSignedDecimal(int64_t v);
    void EndLine();

    // Convenience for the many fixed strings emitted during the report.
    void WriteLine(const char* s);
    // "key: value" line shortcut (no string-escaping; values are trusted CLR strings).
    void WriteKeyValueStr(const char* key, const char* value);
    void WriteKeyValueDecimal(const char* key, uint64_t value);

    void WriteSeparator();
    void WriteBlank() { WriteLine(""); }

private:
    void Flush();

    char m_buffer[SIGNAL_SAFE_CONSOLE_BUFFER_SIZE];
    size_t m_pos;
};

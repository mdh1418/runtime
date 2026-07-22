// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "inproccrashreporter.h"

#include <android/log.h>
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    struct OutputBuffer
    {
        char* Data;
        size_t Length;
        size_t Capacity;
    };

    bool WriteOutput(const char* buffer, size_t length, void* context)
    {
        OutputBuffer* output = static_cast<OutputBuffer*>(context);
        size_t required = output->Length + length + 1;
        if (required > output->Capacity)
        {
            size_t capacity = output->Capacity == 0 ? 4096 : output->Capacity;
            while (capacity < required)
            {
                capacity *= 2;
            }

            char* resized = static_cast<char*>(realloc(output->Data, capacity));
            if (resized == nullptr)
            {
                return false;
            }

            output->Data = resized;
            output->Capacity = capacity;
        }

        memcpy(output->Data + output->Length, buffer, length);
        output->Length += length;
        output->Data[output->Length] = '\0';
        return true;
    }

    bool WriteReport(InProcCrashReportOutputFormat format, OutputBuffer* output)
    {
        output->Length = 0;
        if (output->Data != nullptr)
        {
            output->Data[0] = '\0';
        }

        return InProcCrashReportCreateReport(
            format,
            SIGABRT,
            /*context*/ nullptr,
            &WriteOutput,
            output);
    }

    bool Contains(const OutputBuffer& value, const char* expected)
    {
        return value.Data != nullptr && strstr(value.Data, expected) != nullptr;
    }

    size_t CountOccurrences(const OutputBuffer& value, const char* expected)
    {
        size_t count = 0;
        size_t expectedLength = strlen(expected);
        const char* current = value.Data;
        while (current != nullptr && (current = strstr(current, expected)) != nullptr)
        {
            count++;
            current += expectedLength;
        }

        return count;
    }

    int CountLifecycleReports()
    {
        const char* tmpdir = getenv("TMPDIR");
        if (tmpdir == nullptr)
        {
            return -1;
        }

        char reportDir[PATH_MAX];
        if (snprintf(reportDir, sizeof(reportDir), "%s/.dotnet/crash-reports", tmpdir) >= static_cast<int>(sizeof(reportDir)))
        {
            return -1;
        }

        DIR* directory = opendir(reportDir);
        if (directory == nullptr)
        {
            return 0;
        }

        constexpr const char* Suffix = ".crashreport.json";
        constexpr size_t SuffixLength = sizeof(".crashreport.json") - 1;
        int count = 0;
        while (dirent* entry = readdir(directory))
        {
            const char* name = entry->d_name;
            size_t length = strlen(name);
            if (strncmp(name, "report-", sizeof("report-") - 1) == 0 &&
                length >= SuffixLength &&
                strcmp(name + length - SuffixLength, Suffix) == 0)
            {
                count++;
            }
        }

        closedir(directory);
        return count;
    }

    char* Base64Encode(const OutputBuffer& input, size_t* encodedLength)
    {
        static constexpr char Alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        *encodedLength = ((input.Length + 2) / 3) * 4;
        char* output = static_cast<char*>(malloc(*encodedLength + 1));
        if (output == nullptr)
        {
            return nullptr;
        }

        size_t outputOffset = 0;
        for (size_t offset = 0; offset < input.Length; offset += 3)
        {
            unsigned int value = static_cast<unsigned char>(input.Data[offset]) << 16;
            bool hasSecond = offset + 1 < input.Length;
            bool hasThird = offset + 2 < input.Length;
            if (hasSecond)
            {
                value |= static_cast<unsigned char>(input.Data[offset + 1]) << 8;
            }
            if (hasThird)
            {
                value |= static_cast<unsigned char>(input.Data[offset + 2]);
            }

            output[outputOffset++] = Alphabet[(value >> 18) & 0x3f];
            output[outputOffset++] = Alphabet[(value >> 12) & 0x3f];
            output[outputOffset++] = hasSecond ? Alphabet[(value >> 6) & 0x3f] : '=';
            output[outputOffset++] = hasThird ? Alphabet[value & 0x3f] : '=';
        }

        output[outputOffset] = '\0';
        return output;
    }

    bool EmitArtifact(const char* name, const OutputBuffer& content)
    {
        // Stay below Android's per-log-entry payload limit after logcat adds its prefix.
        constexpr size_t ChunkSize = 700;
        size_t encodedLength;
        char* encoded = Base64Encode(content, &encodedLength);
        if (encoded == nullptr)
        {
            return false;
        }

        size_t count = (encodedLength + ChunkSize - 1) / ChunkSize;
        for (size_t index = 0; index < count; index++)
        {
            size_t offset = index * ChunkSize;
            size_t length = encodedLength - offset < ChunkSize ? encodedLength - offset : ChunkSize;
            __android_log_print(
                ANDROID_LOG_INFO,
                "DOTNET",
                "INPROC_ARTIFACT:%s:%zu:%zu:%.*s",
                name,
                index,
                count,
                static_cast<int>(length),
                encoded + offset);
        }

        free(encoded);
        return true;
    }
}

extern "C" int InProcCrashReportTest_CreateRealOnDemandReports()
{
    int reportsBefore = CountLifecycleReports();
    if (reportsBefore < 0)
    {
        return -1;
    }

    OutputBuffer firstJson = {};
    OutputBuffer secondJson = {};
    OutputBuffer log = {};
    int result = 0;

    if (!WriteReport(InProcCrashReportOutputFormat::Json, &firstJson))
    {
        result = -2;
    }
    else if (!WriteReport(InProcCrashReportOutputFormat::Json, &secondJson))
    {
        result = -3;
    }
    else if (!WriteReport(InProcCrashReportOutputFormat::Log, &log))
    {
        result = -4;
    }
    else if (!Contains(firstJson, R"("protocol_version": "1.0.0")") ||
        !Contains(firstJson, R"("signal": "6")") ||
        CountOccurrences(firstJson, R"("crashed": "true")") != 1 ||
        !Contains(firstJson, "Program.CaptureReports") ||
        !Contains(firstJson, "Program.ParkedWorker"))
    {
        result = -5;
    }
    else if (!Contains(secondJson, R"("protocol_version": "1.0.0")") ||
        !Contains(secondJson, R"("signal": "6")") ||
        CountOccurrences(secondJson, R"("crashed": "true")") != 1 ||
        !Contains(secondJson, "Program.CaptureReports") ||
        !Contains(secondJson, "Program.ParkedWorker"))
    {
        result = -6;
    }
    else if (!Contains(log, ".NET Crash Report v1.0.0") ||
        !Contains(log, "signal 6 (SIGABRT)") ||
        !Contains(log, "(crashed) ---") ||
        !Contains(log, "Program.CaptureReports") ||
        !Contains(log, "Program.ParkedWorker") ||
        !Contains(log, "modules:"))
    {
        result = -7;
    }
    else if (CountLifecycleReports() != reportsBefore)
    {
        result = -8;
    }
    else if (!EmitArtifact("first-report.json", firstJson) ||
        !EmitArtifact("second-report.json", secondJson) ||
        !EmitArtifact("report.log", log))
    {
        result = -9;
    }

    free(firstJson.Data);
    free(secondJson.Data);
    free(log.Data);
    return result;
}

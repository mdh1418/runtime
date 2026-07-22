// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json;

public static class OnDemandProgram
{
    private const int Success = 100;
    private const int Failure = 1;
    private const string NativeLib = "libmonodroid";

    [DllImport(NativeLib)]
    private static extern int InProcCrashReportTest_DriveOnDemand(
        string firstJsonPath,
        string secondJsonPath,
        string logPath);

    public static int Main()
    {
        string outputDir = GetWritableDirectory();
        string firstJsonPath = Path.Combine(outputDir, "inproc_ondemand_first.json");
        string secondJsonPath = Path.Combine(outputDir, "inproc_ondemand_second.json");
        string logPath = Path.Combine(outputDir, "inproc_ondemand.log");
        string reportDir = Path.Combine(outputDir, ".dotnet", "crash-reports");
        string[] reportsBefore = ListReports(reportDir);

        try
        {
            DeleteIfExists(firstJsonPath);
            DeleteIfExists(secondJsonPath);
            DeleteIfExists(logPath);

            int result = InProcCrashReportTest_DriveOnDemand(firstJsonPath, secondJsonPath, logPath);
            if (result != 0)
            {
                return FailMain($"native on-demand driver returned {result}");
            }

            if (!ValidateJson(File.ReadAllText(firstJsonPath)) ||
                !ValidateJson(File.ReadAllText(secondJsonPath)) ||
                !ValidateLog(File.ReadAllText(logPath)))
            {
                return Failure;
            }

            string[] addedReports = ListReports(reportDir)
                .Except(reportsBefore, StringComparer.Ordinal)
                .ToArray();
            if (addedReports.Length != 0)
            {
                return FailMain("on-demand generation unexpectedly created lifecycle-managed files");
            }
        }
        catch (Exception ex)
        {
            return FailMain(ex.ToString());
        }

        Console.WriteLine("PASS: on-demand JSON and log reports are repeatable and isolated");
        return Success;
    }

    private static bool ValidateJson(string json)
    {
        using JsonDocument document = JsonDocument.Parse(json);
        JsonElement root = document.RootElement;
        if (!root.TryGetProperty("payload", out JsonElement payload) ||
            !payload.TryGetProperty("protocol_version", out JsonElement protocol) ||
            protocol.GetString() != "1.0.0" ||
            !payload.TryGetProperty("threads", out JsonElement threads) ||
            threads.ValueKind != JsonValueKind.Array ||
            threads.GetArrayLength() != 3)
        {
            return Fail("on-demand JSON payload shape mismatch");
        }

        JsonElement crashThread = threads[0];
        if (!StringEquals(crashThread, "crashed", "true") ||
            !StringEquals(crashThread, "managed_exception_type", "System.NullReferenceException") ||
            !crashThread.TryGetProperty("stack_frames", out JsonElement frames) ||
            frames.ValueKind != JsonValueKind.Array ||
            frames.GetArrayLength() != 5)
        {
            return Fail("on-demand JSON crash thread mismatch");
        }

        if (!root.TryGetProperty("parameters", out JsonElement parameters) ||
            !StringEquals(parameters, "signal", "11"))
        {
            return Fail("on-demand JSON signal mismatch");
        }

        return true;
    }

    private static bool ValidateLog(string log)
    {
        string[] required =
        [
            ".NET Crash Report v1.0.0",
            "signal 11 (SIGSEGV)",
            "managed exception: System.NullReferenceException (0x80004003)",
            "Synthetic.App.Worker`1[System.Int32].DoWork",
            "Synthetic.App.Dictionary`2[System.String,System.Int32].Insert",
        ];

        foreach (string value in required)
        {
            if (!log.Contains(value, StringComparison.Ordinal))
            {
                return Fail($"on-demand log missing '{value}'");
            }
        }

        return true;
    }

    private static bool StringEquals(JsonElement element, string property, string expected) =>
        element.TryGetProperty(property, out JsonElement value) &&
        value.ValueKind == JsonValueKind.String &&
        value.GetString() == expected;

    private static void DeleteIfExists(string path)
    {
        if (File.Exists(path))
        {
            File.Delete(path);
        }
    }

    private static string[] ListReports(string reportDir) =>
        Directory.Exists(reportDir)
            ? Directory.GetFiles(reportDir, "report-*.crashreport.json")
            : [];

    private static string GetWritableDirectory()
    {
        string? tmpdir = Environment.GetEnvironmentVariable("TMPDIR");
        if (!string.IsNullOrEmpty(tmpdir) && Directory.Exists(tmpdir))
        {
            return tmpdir;
        }

        return AppContext.BaseDirectory;
    }

    private static int FailMain(string message)
    {
        Console.WriteLine($"FAIL: {message}");
        return Failure;
    }

    private static bool Fail(string message)
    {
        Console.WriteLine($"FAIL: {message}");
        return false;
    }
}

// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Reflection;
using System.Text.Json;

namespace InProcCrashReport.RealCrash.Host;

/// <summary>
/// Shared per-test-class fixture: resolves the device and APK, installs the CrashApp
/// once, and drives a single crash scenario (launch -> wait -> pull JSON + logcat).
/// </summary>
public sealed class CrashAppDevice : IDisposable
{
    // Matches the AssemblyName in the CrashApp .csproj; net.dot.* is the AndroidAppBuilder package prefix.
    private const string AssemblyName = "Android.Device_Emulator.InProcCrashReport.RealCrash.CrashApp";
    public const string PackageId = "net.dot." + AssemblyName;
    private const string Instrumentation = PackageId + "/net.dot.MonoRunner";
    private const string ReportRoot = "/data/data/" + PackageId + "/files";
    private const string ReportDirectory = "files/.dotnet/crash-reports";

    private static readonly TimeSpan s_shortTimeout = TimeSpan.FromSeconds(60);
    private static readonly TimeSpan s_instrumentTimeout = TimeSpan.FromSeconds(180);
    private static readonly TimeSpan s_reportPollTimeout = TimeSpan.FromSeconds(30);

    private readonly Adb _adb;

    public CrashAppDevice()
    {
        _adb = Adb.Create();

        string apk = ResolveApkPath();
        if (!File.Exists(apk))
        {
            throw new FileNotFoundException(
                $"CrashApp APK not found at '{apk}'. Build it first (see Host/README.md) or set INPROC_CRASH_APK.", apk);
        }

        // Best-effort clean install so each run starts from a known state.
        _adb.Run(["uninstall", PackageId], s_shortTimeout);
        AdbResult install = _adb.Run(["install", "-r", "-g", apk], s_instrumentTimeout);
        if (!install.Succeeded || !install.CombinedOutput.Contains("Success", StringComparison.Ordinal))
        {
            throw new InvalidOperationException($"Failed to install '{apk}':\n{install.CombinedOutput}");
        }
    }

    /// <summary>
    /// Launches the app for <paramref name="scenario"/>, waits for the crash, and returns
    /// the pulled crash-report JSON and the captured DOTNET_CRASH console output. When
    /// <paramref name="collectJson"/> is <see langword="false"/> the reporter is enabled
    /// without a report root, so it must emit the console report but write no JSON file; the
    /// returned <see cref="CrashOutputs.Json"/> is empty and the absence of a file is verified.
    /// </summary>
    public CrashOutputs RunScenario(string scenario, bool collectJson = true)
    {
        string[] reportsBefore = ListReportFiles();

        // Scope logcat to this run.
        _adb.Run(["logcat", "-c"], s_shortTimeout);

        // The app crashes through the real reporter; `am instrument` returns with
        // shortMsg=Process crashed, which we intentionally ignore.
        var instrument = new List<string>
        {
            "am", "instrument", "-w",
            "-e", "env:DOTNET_EnableCrashReport", "1",
            "-e", "env:CRASH_SCENARIO", scenario,
        };
        if (collectJson)
        {
            instrument.AddRange(["-e", "env:DOTNET_CrashReportRootPath", ReportRoot]);
        }

        instrument.Add(Instrumentation);
        _adb.Shell(s_instrumentTimeout, [.. instrument]);

        string json = collectJson ? PollForReport(reportsBefore) : VerifyNoNewReport(reportsBefore);
        string console = CaptureConsoleReport();
        return new CrashOutputs(json, console);
    }

    private string PollForReport(string[] reportsBefore)
    {
        DateTime deadline = DateTime.UtcNow + s_reportPollTimeout;
        string lastError = "report file never appeared";

        while (DateTime.UtcNow < deadline)
        {
            string[] added = ListReportFiles().Except(reportsBefore, StringComparer.Ordinal).ToArray();
            if (added.Length > 1)
            {
                throw new InvalidOperationException(
                    "Crash produced multiple lifecycle-managed reports: " + string.Join(", ", added));
            }

            if (added.Length == 1)
            {
                AdbResult cat = _adb.ExecOut(s_shortTimeout, "run-as", PackageId, "cat", ReportDirectory + "/" + added[0]);
                if (cat.Succeeded)
                {
                    string content = cat.StandardOutput.Trim();
                    if (content.StartsWith("{", StringComparison.Ordinal) && TryParse(content, out lastError))
                    {
                        return content;
                    }
                }
                else
                {
                    lastError = cat.CombinedOutput.Trim();
                }
            }

            Thread.Sleep(500);
        }

        throw new InvalidOperationException(
            $"A lifecycle-managed crash report was not produced/complete within {s_reportPollTimeout.TotalSeconds:0}s. Last: {lastError}");
    }

    private static bool TryParse(string content, out string error)
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(content);
            // A complete report has the top-level payload object with threads.
            if (doc.RootElement.TryGetProperty("payload", out JsonElement payload)
                && payload.TryGetProperty("threads", out _))
            {
                error = string.Empty;
                return true;
            }

            error = "JSON parsed but missing payload.threads (likely truncated)";
            return false;
        }
        catch (JsonException ex)
        {
            error = "incomplete JSON: " + ex.Message;
            return false;
        }
    }

    private string CaptureConsoleReport()
    {
        AdbResult logcat = _adb.Run(["logcat", "-d", "-s", "DOTNET_CRASH:V"], s_shortTimeout);
        return logcat.StandardOutput;
    }

    /// <summary>
    /// Lists the app's lifecycle-managed <c>*.crashreport.json</c> files.
    /// </summary>
    private string[] ListReportFiles()
    {
        AdbResult ls = _adb.ExecOut(s_shortTimeout, "run-as", PackageId, "ls", ReportDirectory);
        if (!ls.Succeeded)
        {
            return [];
        }

        return ls.StandardOutput
            .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Where(name => name.StartsWith("report-", StringComparison.Ordinal) &&
                           name.EndsWith(".crashreport.json", StringComparison.Ordinal))
            .ToArray();
    }

    /// <summary>
    /// Asserts the console-only crash produced no new JSON report (the reporter ran without a
    /// report root), returning an empty JSON string.
    /// </summary>
    private string VerifyNoNewReport(string[] reportsBefore)
    {
        string[] added = ListReportFiles().Except(reportsBefore).ToArray();
        if (added.Length > 0)
        {
            throw new InvalidOperationException(
                "Reporter was enabled without a report root but still wrote a JSON report: " + string.Join(", ", added));
        }

        return string.Empty;
    }

    private static string ResolveApkPath()
    {
        if (Environment.GetEnvironmentVariable("INPROC_CRASH_APK") is { Length: > 0 } fromEnv)
        {
            return fromEnv;
        }

        foreach (AssemblyMetadataAttribute meta in
                 typeof(CrashAppDevice).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>())
        {
            if (meta.Key == "DefaultApkPath" && !string.IsNullOrEmpty(meta.Value))
            {
                return meta.Value;
            }
        }

        throw new InvalidOperationException("No APK path: set INPROC_CRASH_APK.");
    }

    public void Dispose() => _adb.Run(["uninstall", PackageId], s_shortTimeout);
}

/// <summary>The two artifacts a crash produces: the JSON report and the console report.</summary>
public readonly record struct CrashOutputs(string Json, string Console);

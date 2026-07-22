// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Reflection;
using System.Text;
using System.Text.Json;

namespace InProcCrashReport.RealCrash.Host;

internal static class CrashArtifactWriter
{
    private static readonly object s_lock = new();
    private static readonly string s_runDirectory = ResolveRunDirectory();

    public static void Write(string artifactName, string scenario, bool expectsJson, CrashOutputs outputs)
    {
        string scenarioDirectory = Path.Combine(s_runDirectory, SanitizeName(artifactName));

        lock (s_lock)
        {
            Directory.CreateDirectory(scenarioDirectory);

            string jsonPath = Path.Combine(scenarioDirectory, "report.json");
            if (expectsJson)
            {
                File.WriteAllText(jsonPath, outputs.Json, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
            }
            else if (File.Exists(jsonPath))
            {
                File.Delete(jsonPath);
            }

            File.WriteAllText(
                Path.Combine(scenarioDirectory, "console.txt"),
                outputs.Console,
                new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));

            string capture = JsonSerializer.Serialize(
                new
                {
                    scenario,
                    expectsJson,
                    capturedAtUtc = DateTime.UtcNow,
                },
                new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(
                Path.Combine(scenarioDirectory, "capture.json"),
                capture,
                new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
        }
    }

    private static string ResolveRunDirectory()
    {
        if (Environment.GetEnvironmentVariable("INPROC_CRASH_RESULTS_DIR") is { Length: > 0 } configured)
        {
            return Path.GetFullPath(configured);
        }

        foreach (AssemblyMetadataAttribute metadata in
                 typeof(CrashArtifactWriter).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>())
        {
            if (metadata.Key == "DefaultResultsRoot" && metadata.Value is { Length: > 0 } root)
            {
                string runName = $"{DateTime.UtcNow:yyyyMMdd-HHmmss}-pid{Environment.ProcessId}";
                return Path.Combine(root, runName, "real-crash");
            }
        }

        throw new InvalidOperationException(
            "No results directory is configured. Set INPROC_CRASH_RESULTS_DIR.");
    }

    private static string SanitizeName(string name)
    {
        foreach (char invalid in Path.GetInvalidFileNameChars())
        {
            name = name.Replace(invalid, '_');
        }

        return name;
    }
}

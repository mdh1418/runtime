// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Diagnostics;
using System.Text;

namespace InProcCrashReport.RealCrash.Host;

/// <summary>
/// Thin wrapper around the <c>adb</c> command line for a single target device.
/// </summary>
internal sealed class Adb
{
    private readonly string _exe;
    private readonly string _deviceId;

    private Adb(string exe, string deviceId)
    {
        _exe = exe;
        _deviceId = deviceId;
    }

    public string DeviceId => _deviceId;

    /// <summary>
    /// Creates an <see cref="Adb"/> for the configured device, auto-selecting the
    /// single attached device when <c>INPROC_CRASH_DEVICE_ID</c> is not set.
    /// </summary>
    public static Adb Create()
    {
        string exe = Environment.GetEnvironmentVariable("INPROC_CRASH_ADB") is { Length: > 0 } overrideExe
            ? overrideExe
            : "adb";

        string deviceId = Environment.GetEnvironmentVariable("INPROC_CRASH_DEVICE_ID") is { Length: > 0 } configured
            ? configured
            : ResolveSingleDevice(exe);

        return new Adb(exe, deviceId);
    }

    private static string ResolveSingleDevice(string exe)
    {
        AdbResult result = RunRaw(exe, deviceId: null, ["devices"], TimeSpan.FromSeconds(30));
        List<string> devices = [];
        foreach (string line in result.StandardOutput.Split('\n'))
        {
            string trimmed = line.Trim();
            if (trimmed.Length == 0 || trimmed.StartsWith("List of devices", StringComparison.Ordinal))
            {
                continue;
            }

            string[] parts = trimmed.Split('\t');
            if (parts.Length == 2 && parts[1].Trim() == "device")
            {
                devices.Add(parts[0].Trim());
            }
        }

        return devices.Count switch
        {
            1 => devices[0],
            0 => throw new InvalidOperationException("No attached adb device found. Start an emulator or set INPROC_CRASH_DEVICE_ID."),
            _ => throw new InvalidOperationException(
                $"Multiple attached adb devices ({string.Join(", ", devices)}). Set INPROC_CRASH_DEVICE_ID to choose one."),
        };
    }

    /// <summary>Runs an adb command against the target device.</summary>
    public AdbResult Run(IEnumerable<string> args, TimeSpan timeout)
        => RunRaw(_exe, _deviceId, args, timeout);

    /// <summary>Runs <c>adb shell</c> with the given shell words.</summary>
    public AdbResult Shell(TimeSpan timeout, params string[] shellArgs)
        => Run(["shell", .. shellArgs], timeout);

    /// <summary>Runs <c>adb exec-out</c> (no tty translation) with the given words.</summary>
    public AdbResult ExecOut(TimeSpan timeout, params string[] args)
        => Run(["exec-out", .. args], timeout);

    private static AdbResult RunRaw(string exe, string? deviceId, IEnumerable<string> args, TimeSpan timeout)
    {
        var psi = new ProcessStartInfo
        {
            FileName = exe,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };

        if (deviceId is not null)
        {
            psi.ArgumentList.Add("-s");
            psi.ArgumentList.Add(deviceId);
        }

        foreach (string arg in args)
        {
            psi.ArgumentList.Add(arg);
        }

        using var process = new Process { StartInfo = psi };
        var stdout = new StringBuilder();
        var stderr = new StringBuilder();

        process.OutputDataReceived += (_, e) => { if (e.Data is not null) { stdout.AppendLine(e.Data); } };
        process.ErrorDataReceived += (_, e) => { if (e.Data is not null) { stderr.AppendLine(e.Data); } };

        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();

        if (!process.WaitForExit((int)timeout.TotalMilliseconds))
        {
            try { process.Kill(entireProcessTree: true); } catch { /* best effort */ }
            throw new TimeoutException($"adb {string.Join(' ', args)} timed out after {timeout.TotalSeconds:0}s.");
        }

        // Ensure async output handlers have flushed.
        process.WaitForExit();

        return new AdbResult(process.ExitCode, stdout.ToString(), stderr.ToString());
    }
}

internal readonly record struct AdbResult(int ExitCode, string StandardOutput, string StandardError)
{
    public bool Succeeded => ExitCode == 0;

    public string CombinedOutput => StandardOutput + StandardError;
}

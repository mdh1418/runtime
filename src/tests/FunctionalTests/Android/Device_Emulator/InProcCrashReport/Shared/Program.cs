// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json;

/// <summary>
/// Synthetic fidelity gate for the CoreCLR in-proc crash reporter.
///
/// The native driver (recompiled real reporter sources) is invoked with synthetic
/// callback data to produce a *.crashreport.json file and a compact console report;
/// this harness reads both back and asserts their structure and key fields are
/// stable. Variable fields that depend on the runtime environment (pid, process
/// name/path, build version, thread ids) are intentionally not asserted by exact
/// value -- the goal is fidelity and no regression, not a byte-for-byte match.
///
/// Each scenario is its own functional-test project (its own app launch) because
/// the reporter generates exactly one report per process. The scenario is selected
/// at compile time by an INPROC_SCENARIO_* constant defined by the project file.
///
/// Returns 100 on success (the value the XHarness functional-test harness expects
/// via ExpectedExitCode); any other value indicates a failure.
/// </summary>
public static class Program
{
    private const int Success = 100;
    private const int Failure = 1;

#if INPROC_SCENARIO_ABORT
    private const int ScenarioId = 1;
    private const bool ExpectsJson = true;
#elif INPROC_SCENARIO_STACKOVERFLOW
    private const int ScenarioId = 2;
    private const bool ExpectsJson = true;
#elif INPROC_SCENARIO_CONSOLEONLY
    private const int ScenarioId = 3;
    private const bool ExpectsJson = false;
#else // INPROC_SCENARIO_RICHSIGSEGV (default)
    private const int ScenarioId = 0;
    private const bool ExpectsJson = true;
#endif

    // The native sources are compiled into the app's primary native library,
    // libmonodroid.so (the AndroidAppBuilder CMake target is always named
    // "monodroid"). Mono resolves app-embedded exports via the special
    // "__Internal" name, but CoreCLR does not, so the library is named directly.
    private const string NativeLib = "libmonodroid";

    [DllImport(NativeLib)]
    private static extern int InProcCrashReportTest_DriveScenario(
        int scenario, string reporterRootPath, string consoleCapturePath);

    public static int Main()
    {
        string outputDir = GetWritableDirectory();
        string reportDir = Path.Combine(outputDir, ".dotnet", "crash-reports");
        string consolePath = Path.Combine(outputDir, $"inproc_crashreport_scenario{ScenarioId}.console.txt");
        string[] reportsBefore = ListReports(reportDir);

        try
        {
            if (File.Exists(consolePath))
            {
                File.Delete(consolePath);
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"FAIL: could not clear stale report files: {ex}");
            return Failure;
        }

        // In console-only mode the reporter is given an empty root path so file
        // output is disabled; the console capture path is always real.
        string reporterRootPath = ExpectsJson ? outputDir : string.Empty;

        Console.WriteLine($"InProcCrashReport: driving scenario {ScenarioId} (expectsJson={ExpectsJson})");
        int driveResult = InProcCrashReportTest_DriveScenario(ScenarioId, reporterRootPath, consolePath);
        if (driveResult != 0)
        {
            Console.WriteLine($"FAIL: native driver returned {driveResult} for scenario {ScenarioId}");
            return Failure;
        }

#if INPROC_SCENARIO_CONSOLEONLY
        // Compact-log-only mode: no JSON file must be produced.
        string[] unexpectedReports = ListReports(reportDir).Except(reportsBefore, StringComparer.Ordinal).ToArray();
        if (unexpectedReports.Length != 0)
        {
            Console.WriteLine($"FAIL: console-only mode unexpectedly produced '{string.Join(", ", unexpectedReports)}'");
            return Failure;
        }
#else
        string[] newReports = ListReports(reportDir).Except(reportsBefore, StringComparer.Ordinal).ToArray();
        if (newReports.Length != 1)
        {
            Console.WriteLine($"FAIL: expected one new lifecycle-managed report, found {newReports.Length}");
            return Failure;
        }

        string json = File.ReadAllText(newReports[0]);
        Console.WriteLine($"InProcCrashReport: report is {json.Length} bytes");

        if (!ValidateJson(json))
        {
            return Failure;
        }
#endif

        if (!File.Exists(consolePath))
        {
            Console.WriteLine($"FAIL: reporter did not produce '{consolePath}'");
            return Failure;
        }

        string console = File.ReadAllText(consolePath);
        Console.WriteLine($"InProcCrashReport: console report is {console.Length} bytes");

        if (!ValidateConsole(console))
        {
            return Failure;
        }

        Console.WriteLine($"PASS: scenario {ScenarioId} produced structurally valid crash report output");
        return Success;
    }

    private static string[] ListReports(string reportDir) =>
        Directory.Exists(reportDir)
            ? Directory.GetFiles(reportDir, "report-*.crashreport.json")
            : [];

    private static bool ValidateJson(string json) => ScenarioId switch
    {
        0 => ValidateRichSigsegvJson(json),
        1 => ValidateAbortJson(json),
        2 => ValidateStackOverflowJson(json),
        _ => Fail($"no JSON validation defined for scenario {ScenarioId}"),
    };

    private static bool ValidateConsole(string console) => ScenarioId switch
    {
        // Console-only (3) drives the same synthetic data as the rich SIGSEGV
        // scenario, so it produces the same compact console report.
        0 or 3 => ValidateRichSigsegvConsole(console),
        1 => ValidateAbortConsole(console),
        2 => ValidateStackOverflowConsole(console),
        _ => Fail($"no console validation defined for scenario {ScenarioId}"),
    };

    // --- payload-level shared checks ---------------------------------------

    private static bool TryParsePayload(string json, out JsonElement root, out JsonElement payload)
    {
        root = default;
        payload = default;
        try
        {
            root = JsonDocument.Parse(json).RootElement;
        }
        catch (JsonException ex)
        {
            return Fail($"report is not valid JSON: {ex.Message}");
        }

        if (!root.TryGetProperty("payload", out payload))
        {
            return Fail("missing 'payload'");
        }

        if (!StringEquals(payload, "protocol_version", "1.0.0"))
        {
            return Fail("unexpected or missing payload.protocol_version");
        }

        if (!payload.TryGetProperty("configuration", out JsonElement configuration) ||
            configuration.ValueKind != JsonValueKind.Object ||
            !configuration.TryGetProperty("architecture", out _))
        {
            return Fail("missing payload.configuration.architecture");
        }

        // process_name and pid are runtime-dependent: assert presence, not value.
        if (!HasNonEmptyString(payload, "process_name"))
        {
            return Fail("missing payload.process_name");
        }
        if (!HasNonEmptyString(payload, "pid"))
        {
            return Fail("missing payload.pid");
        }

        return true;
    }

    private static bool CheckSignal(JsonElement root, string expected)
    {
        if (!root.TryGetProperty("parameters", out JsonElement parameters) ||
            parameters.ValueKind != JsonValueKind.Object)
        {
            return Fail("missing 'parameters' object");
        }
        if (!StringEquals(parameters, "signal", expected))
        {
            return Fail($"parameters.signal should be {expected}");
        }
        return true;
    }

    private static bool CheckRegisterContext(JsonElement thread)
    {
        if (!thread.TryGetProperty("ctx", out JsonElement ctx) || ctx.ValueKind != JsonValueKind.Object)
        {
            return Fail("crash thread missing register context 'ctx'");
        }
        foreach (string reg in new[] { "IP", "SP", "BP" })
        {
            if (!HasNonEmptyString(ctx, reg))
            {
                return Fail($"crash thread ctx missing register '{reg}'");
            }
        }
        return true;
    }

    // --- scenario 0: rich SIGSEGV ------------------------------------------

    private static bool ValidateRichSigsegvJson(string json)
    {
        if (!TryParsePayload(json, out JsonElement root, out JsonElement payload))
        {
            return false;
        }

        if (!payload.TryGetProperty("threads", out JsonElement threads) ||
            threads.ValueKind != JsonValueKind.Array)
        {
            return Fail("missing payload.threads array");
        }
        if (threads.GetArrayLength() != 3)
        {
            return Fail($"expected 3 threads, got {threads.GetArrayLength()}");
        }

        JsonElement crashThread = threads[0];
        if (!StringEquals(crashThread, "crashed", "true"))
        {
            return Fail("thread[0] is not marked crashed");
        }
        if (!StringEquals(crashThread, "managed_exception_type", "System.NullReferenceException"))
        {
            return Fail("thread[0] managed_exception_type mismatch");
        }
        if (!StringEquals(crashThread, "managed_exception_hresult", "0x80004003"))
        {
            return Fail("thread[0] managed_exception_hresult mismatch");
        }
        if (!CheckRegisterContext(crashThread))
        {
            return false;
        }

        // Five frames: a leading native frame synthesized from the register context
        // (IP/SP), then the four enumerated frames interleaving managed and native.
        if (!crashThread.TryGetProperty("stack_frames", out JsonElement frames) ||
            frames.ValueKind != JsonValueKind.Array ||
            frames.GetArrayLength() != 5)
        {
            return Fail($"thread[0] should have 5 stack frames, got {(frames.ValueKind == JsonValueKind.Array ? frames.GetArrayLength() : -1)}");
        }

        // Frame 0: context-derived native frame at the crashing instruction pointer.
        if (!StringEquals(frames[0], "is_managed", "false") ||
            !StringEquals(frames[0], "native_address", "0x40aaaa"))
        {
            return Fail("thread[0] frame[0] should be the context-derived native frame");
        }

        // Frame 1: managed, generic-instantiation method name.
        if (!StringEquals(frames[1], "is_managed", "true") ||
            !StringEquals(frames[1], "method_name", "Synthetic.App.Worker`1[System.Int32].DoWork") ||
            !HasNonEmptyString(frames[1], "token") ||
            !StringEquals(frames[1], "filename", "synthetic.managed.dll"))
        {
            return Fail("thread[0] frame[1] managed/generic frame mismatch");
        }

        // Frame 2: native (interleaved).
        if (!StringEquals(frames[2], "is_managed", "false") ||
            !StringEquals(frames[2], "native_module", "libsynthetic.so"))
        {
            return Fail("thread[0] frame[2] native frame mismatch");
        }

        // Frame 3: managed, two-parameter generic instantiation (interleaved).
        if (!StringEquals(frames[3], "is_managed", "true") ||
            !StringEquals(frames[3], "method_name", "Synthetic.App.Dictionary`2[System.String,System.Int32].Insert"))
        {
            return Fail("thread[0] frame[3] managed/generic frame mismatch");
        }

        // Frame 4: native from a second module (interleaved).
        if (!StringEquals(frames[4], "is_managed", "false") ||
            !StringEquals(frames[4], "native_module", "libnative2.so"))
        {
            return Fail("thread[0] frame[4] native frame mismatch");
        }

        JsonElement managedBg = threads[1];
        if (!StringEquals(managedBg, "crashed", "false"))
        {
            return Fail("thread[1] should not be marked crashed");
        }
        if (!SingleFrame(managedBg, out JsonElement bgFrame) ||
            !StringEquals(bgFrame, "method_name", "Synthetic.App.Server.Listen"))
        {
            return Fail("thread[1] frame[0] managed method_name mismatch");
        }

        JsonElement nativeBg = threads[2];
        if (!StringEquals(nativeBg, "crashed", "false"))
        {
            return Fail("thread[2] should not be marked crashed");
        }
        if (!SingleFrame(nativeBg, out JsonElement nativeBgFrame) ||
            !StringEquals(nativeBgFrame, "native_module", "libsynthetic.so"))
        {
            return Fail("thread[2] frame[0] native_module mismatch");
        }

        return CheckSignal(root, "11");
    }

    private static bool ValidateRichSigsegvConsole(string console)
    {
        string[] required =
        {
            ".NET Crash Report v1.0.0",
            "ABI: ",
            "signal 11 (SIGSEGV)",
            "(crashed) ---",
            "managed exception: System.NullReferenceException (0x80004003)",
            "Synthetic.App.Worker`1[System.Int32].DoWork + 0x10 (token=0x6000001)",
            "Synthetic.App.Dictionary`2[System.String,System.Int32].Insert + 0x10 (token=0x6000002)",
            "Synthetic.App.Server.Listen + 0x10 (token=0x6000003)",
            "libsynthetic.so + 0x40",
            "libnative2.so + 0x40",
            "modules:",
            "synthetic.managed.dll {11111111-2222-3333-4455-66778899aabb}",
            "libsynthetic.so {11111111-2222-3333-4455-66778899aabb}",
            "libnative2.so {11111111-2222-3333-4455-66778899aabb}",
        };
        return RequireAll(console, required) && RequireCount(console, "--- thread ", 3);
    }

    // --- scenario 1: abort (SIGABRT, no managed exception) -----------------

    private static bool ValidateAbortJson(string json)
    {
        if (!TryParsePayload(json, out JsonElement root, out JsonElement payload))
        {
            return false;
        }

        if (!payload.TryGetProperty("threads", out JsonElement threads) ||
            threads.ValueKind != JsonValueKind.Array ||
            threads.GetArrayLength() != 2)
        {
            return Fail($"expected 2 threads, got {(threads.ValueKind == JsonValueKind.Array ? threads.GetArrayLength() : -1)}");
        }

        JsonElement crashThread = threads[0];
        if (!StringEquals(crashThread, "crashed", "true"))
        {
            return Fail("thread[0] is not marked crashed");
        }
        // No managed exception: the abort path must not emit managed_exception_*.
        if (crashThread.TryGetProperty("managed_exception_type", out _))
        {
            return Fail("abort crash thread should not have a managed_exception_type");
        }
        if (!CheckRegisterContext(crashThread))
        {
            return false;
        }

        // Context frame + two native frames.
        if (!crashThread.TryGetProperty("stack_frames", out JsonElement frames) ||
            frames.ValueKind != JsonValueKind.Array ||
            frames.GetArrayLength() != 3)
        {
            return Fail($"thread[0] should have 3 stack frames, got {(frames.ValueKind == JsonValueKind.Array ? frames.GetArrayLength() : -1)}");
        }
        if (!StringEquals(frames[1], "native_module", "libsynthetic.so") ||
            !StringEquals(frames[2], "native_module", "libnative2.so"))
        {
            return Fail("thread[0] native frames mismatch");
        }

        JsonElement bg = threads[1];
        if (!StringEquals(bg, "crashed", "false") ||
            !SingleFrame(bg, out JsonElement bgFrame) ||
            !StringEquals(bgFrame, "method_name", "Synthetic.App.Server.Listen"))
        {
            return Fail("thread[1] background frame mismatch");
        }

        return CheckSignal(root, "6");
    }

    private static bool ValidateAbortConsole(string console)
    {
        string[] required =
        {
            ".NET Crash Report v1.0.0",
            "signal 6 (SIGABRT)",
            "(crashed) ---",
            "libsynthetic.so + 0x40",
            "libnative2.so + 0x40",
            "Synthetic.App.Server.Listen + 0x10 (token=0x6000001)",
            "modules:",
        };
        if (!RequireAll(console, required))
        {
            return false;
        }
        // No managed exception in this scenario, so no exception line anywhere.
        if (console.Contains("managed exception:", StringComparison.Ordinal))
        {
            return Fail("abort console report should not contain a 'managed exception:' line");
        }
        return true;
    }

    // --- scenario 2: stack overflow ----------------------------------------

    private static bool ValidateStackOverflowJson(string json)
    {
        if (!TryParsePayload(json, out JsonElement root, out JsonElement payload))
        {
            return false;
        }

        if (!payload.TryGetProperty("threads", out JsonElement threads) ||
            threads.ValueKind != JsonValueKind.Array ||
            threads.GetArrayLength() != 1)
        {
            return Fail($"expected 1 thread, got {(threads.ValueKind == JsonValueKind.Array ? threads.GetArrayLength() : -1)}");
        }

        JsonElement crashThread = threads[0];
        if (!StringEquals(crashThread, "crashed", "true") ||
            !StringEquals(crashThread, "is_managed", "true"))
        {
            return Fail("stack overflow thread should be the managed crash thread");
        }
        if (!StringEquals(crashThread, "managed_exception_type", "System.StackOverflowException"))
        {
            return Fail("stack overflow managed_exception_type mismatch");
        }
        if (!StringEquals(crashThread, "managed_exception_hresult", "0x800703e9"))
        {
            return Fail("stack overflow managed_exception_hresult mismatch");
        }
        if (!StringEquals(crashThread, "stack_overflow_total_frames", "42"))
        {
            return Fail("stack_overflow_total_frames mismatch");
        }

        if (!crashThread.TryGetProperty("stack_frames", out JsonElement frames) ||
            frames.ValueKind != JsonValueKind.Array ||
            frames.GetArrayLength() != 3)
        {
            return Fail($"stack overflow thread should have 3 trace frames, got {(frames.ValueKind == JsonValueKind.Array ? frames.GetArrayLength() : -1)}");
        }
        if (!StringEquals(frames[0], "method_name", "Synthetic.App.Program.Main") ||
            !StringEquals(frames[0], "is_managed", "true"))
        {
            return Fail("stack overflow frame[0] mismatch");
        }
        // Frame 1 is the head of a repeated recursive sequence.
        if (!StringEquals(frames[1], "method_name", "Synthetic.App.Recurse.Down") ||
            !StringEquals(frames[1], "stack_overflow_repeat_count", "40") ||
            !StringEquals(frames[1], "stack_overflow_repeat_sequence_length", "1"))
        {
            return Fail("stack overflow frame[1] repeat metadata mismatch");
        }
        if (!StringEquals(frames[2], "method_name", "Synthetic.App.Recurse.Bottom"))
        {
            return Fail("stack overflow frame[2] mismatch");
        }

        return CheckSignal(root, "11");
    }

    private static bool ValidateStackOverflowConsole(string console)
    {
        string[] required =
        {
            ".NET Crash Report v1.0.0",
            "signal 11 (SIGSEGV)",
            "(crashed) ---",
            "managed exception: System.StackOverflowException (0x800703e9)",
            "stack overflow frames: 42",
            "repeated 40 times:",
            "#00 Synthetic.App.Program.Main",
            "#01 Synthetic.App.Recurse.Down",
            "#02 Synthetic.App.Recurse.Bottom",
        };
        return RequireAll(console, required);
    }

    // --- helpers -----------------------------------------------------------

    private static bool SingleFrame(JsonElement thread, out JsonElement frame)
    {
        frame = default;
        if (!thread.TryGetProperty("stack_frames", out JsonElement frames) ||
            frames.ValueKind != JsonValueKind.Array ||
            frames.GetArrayLength() != 1)
        {
            return false;
        }
        frame = frames[0];
        return true;
    }

    private static bool RequireAll(string console, string[] required)
    {
        foreach (string line in required)
        {
            if (!console.Contains(line, StringComparison.Ordinal))
            {
                return Fail($"console report missing expected text: '{line}'");
            }
        }
        return true;
    }

    private static bool RequireCount(string console, string needle, int expected)
    {
        int count = 0;
        int index = 0;
        while ((index = console.IndexOf(needle, index, StringComparison.Ordinal)) >= 0)
        {
            count++;
            index += needle.Length;
        }
        return count == expected
            ? true
            : Fail($"console report should contain '{needle}' {expected} times, found {count}");
    }

    private static string GetWritableDirectory()
    {
        string? tmpdir = Environment.GetEnvironmentVariable("TMPDIR");
        if (!string.IsNullOrEmpty(tmpdir) && Directory.Exists(tmpdir))
        {
            return tmpdir;
        }

        try
        {
            string temp = Path.GetTempPath();
            if (!string.IsNullOrEmpty(temp) && Directory.Exists(temp))
            {
                return temp;
            }
        }
        catch
        {
            // Fall through to the app base directory.
        }

        return AppContext.BaseDirectory;
    }

    private static bool StringEquals(JsonElement obj, string property, string expected)
        => obj.ValueKind == JsonValueKind.Object &&
           obj.TryGetProperty(property, out JsonElement value) &&
           value.ValueKind == JsonValueKind.String &&
           value.GetString() == expected;

    private static bool HasNonEmptyString(JsonElement obj, string property)
        => obj.ValueKind == JsonValueKind.Object &&
           obj.TryGetProperty(property, out JsonElement value) &&
           value.ValueKind == JsonValueKind.String &&
           !string.IsNullOrEmpty(value.GetString());

    private static bool Fail(string message)
    {
        Console.WriteLine($"FAIL: {message}");
        return false;
    }
}

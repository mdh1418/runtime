// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Text.RegularExpressions;
using Xunit;

namespace InProcCrashReport.RealCrash.Host;

/// <summary>
/// Layer-2 real-crash fidelity tests. Each theory case drives the CrashApp through a
/// real fatal crash handled by the real in-proc reporter inside libcoreclr, then asserts
/// that both outputs (the *.crashreport.json file and the DOTNET_CRASH console report)
/// have a STABLE structure. Volatile values (addresses, thread ids, timestamps, pid,
/// commit hash) are asserted by shape only, never by exact value, so the tests catch
/// regressions without churning on every build.
/// </summary>
public sealed class InProcCrashReportRealCrashTests : IClassFixture<CrashAppDevice>
{
    private static readonly Regex s_hex = new(@"^0x[0-9a-fA-F]+$", RegexOptions.Compiled);
    private static readonly Regex s_guid = new(@"^\{[0-9a-fA-F]{8}-([0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12}\}$", RegexOptions.Compiled);
    private static readonly Regex s_digits = new(@"^\d+$", RegexOptions.Compiled);

    // The three parked background workers, each in a distinct managed call stack.
    private static readonly string[] s_workerMethods =
    [
        "Program.PrepareStatement", // DatabaseWorker
        "Program.ValidateAuth",     // HttpProcessor
        "Program.FlushMetrics",     // TimerCallback
    ];

    private readonly CrashAppDevice _device;

    public InProcCrashReportRealCrashTests(CrashAppDevice device) => _device = device;

    public static TheoryData<string, int, string, string?> Scenarios() => new()
    {
        // scenario,      signal, signalName, managedExceptionType
        { "abort",        6,  "SIGABRT", null },
        { "sigsegv",      11, "SIGSEGV", null },
        { "unhandled",    6,  "SIGABRT", "System.InvalidOperationException" },
        { "multithread",  6,  "SIGABRT", null },
        { "generics",     6,  "SIGABRT", null },
    };

    [Theory]
    [MemberData(nameof(Scenarios))]
    public void CrashReport_HasStableStructure(string scenario, int signal, string signalName, string? managedExceptionType)
    {
        CrashOutputs outputs = _device.RunScenario(scenario);

        AssertJsonFidelity(CrashReport.Parse(outputs.Json), scenario, signal, managedExceptionType);
        AssertConsoleFidelity(outputs.Console, scenario, signal, signalName, managedExceptionType);
    }

    private static void AssertJsonFidelity(CrashReport report, string scenario, int signal, string? managedExceptionType)
    {
        Assert.Equal("1.0.0", report.ProtocolVersion);
        Assert.False(string.IsNullOrEmpty(report.Architecture), "architecture should be reported");
        Assert.Equal(CrashAppDevice.PackageId, report.ProcessName);
        Assert.Matches(s_digits, report.Pid);
        Assert.Equal(signal.ToString(), report.Signal);

        // crashed + 3 parked workers at minimum.
        Assert.True(report.Threads.Count >= 4, $"expected >= 4 threads, got {report.Threads.Count}");

        ReportThread crashed = Assert.Single(report.Threads, t => t.Crashed);
        Assert.True(crashed.IsManaged, "crashed thread should be managed");
        Assert.True(crashed.HasContext, "crashed thread should carry a register context");
        Assert.Matches(s_hex, crashed.ContextIp!);
        Assert.Matches(s_hex, crashed.ContextSp!);

        // The nested crash chain is present and in stack order (innermost first).
        AssertCrashChainOrder(crashed);

        // Every managed frame across every thread has a well-formed shape.
        foreach (ReportThread thread in report.Threads)
        {
            foreach (ReportFrame frame in thread.Frames.Where(f => f.IsManaged))
            {
                Assert.False(string.IsNullOrEmpty(frame.MethodName), "managed frame should have a method name");
                Assert.Matches(s_hex, frame.Token!);
                Assert.Matches(s_hex, frame.IlOffset!);
                Assert.False(string.IsNullOrEmpty(frame.Filename), "managed frame should have a module filename");
                Assert.Matches(s_guid, frame.Guid!);
            }
        }

        // All three distinct parked workers are enumerated.
        foreach (string worker in s_workerMethods)
        {
            Assert.True(report.Threads.Any(t => t.HasMethod(worker)),
                $"expected a thread parked in {worker}");
        }

        AssertManagedException(crashed, managedExceptionType);

        // Generic-method instantiations render as bare Type.Method frames (no type args).
        if (scenario == "generics")
        {
            Assert.True(crashed.HasMethod("Program.GenericCrash"), "generics crash should show Program.GenericCrash");
            Assert.True(crashed.HasMethod("Program.GenericCrashInner"), "generics crash should show Program.GenericCrashInner");
        }

        // The entry-point thread distinction for the multithreaded scenario: the crash
        // happens off the main thread, so Program.Main lives on a *different* thread.
        if (scenario == "multithread")
        {
            Assert.False(crashed.HasMethod("Program.Main"), "multithread crash should not be on the entry thread");
            Assert.True(report.Threads.Any(t => !t.Crashed && t.HasMethod("Program.Main")),
                "the entry thread should still be enumerated while parked in Join");
        }
        else
        {
            Assert.True(crashed.HasMethod("Program.Main"), "crash should be on the entry thread");
        }
    }

    private static void AssertCrashChainOrder(ReportThread crashed)
    {
        int level2 = IndexOfMethod(crashed, "Program.CrashLevel2");
        int level1 = IndexOfMethod(crashed, "Program.CrashLevel1");
        int crash = IndexOfMethod(crashed, "Program.Crash");

        Assert.True(level2 >= 0, "crashed thread missing Program.CrashLevel2");
        Assert.True(level1 >= 0, "crashed thread missing Program.CrashLevel1");
        Assert.True(crash >= 0, "crashed thread missing Program.Crash");
        Assert.True(level2 < level1 && level1 < crash,
            $"crash chain out of stack order: CrashLevel2={level2}, CrashLevel1={level1}, Crash={crash}");
    }

    private static int IndexOfMethod(ReportThread thread, string method)
    {
        for (int i = 0; i < thread.Frames.Count; i++)
        {
            if (thread.Frames[i].MethodName == method)
            {
                return i;
            }
        }

        return -1;
    }

    private static void AssertManagedException(ReportThread crashed, string? managedExceptionType)
    {
        if (managedExceptionType is null)
        {
            Assert.Null(crashed.ManagedExceptionType);
            Assert.Null(crashed.ManagedExceptionHResult);
        }
        else
        {
            Assert.Equal(managedExceptionType, crashed.ManagedExceptionType);
            Assert.Matches(s_hex, crashed.ManagedExceptionHResult!);
        }
    }

    private static void AssertConsoleFidelity(string console, string scenario, int signal, string signalName, string? managedExceptionType)
    {
        Assert.Contains(".NET Crash Report", console);
        Assert.Contains($"signal {signal} ({signalName})", console);
        Assert.Contains("(crashed) ---", console);
        Assert.Contains("modules:", console);

        Assert.Contains("Program.CrashLevel2", console);
        Assert.Contains("Program.CrashLevel1", console);
        Assert.Contains("Program.Crash", console);

        foreach (string worker in s_workerMethods)
        {
            Assert.Contains(worker, console);
        }

        if (managedExceptionType is not null)
        {
            Assert.Contains(managedExceptionType, console);
        }

        if (scenario == "generics")
        {
            Assert.Contains("Program.GenericCrash", console);
            Assert.Contains("Program.GenericCrashInner", console);
        }
    }

    /// <summary>
    /// A real managed stack overflow is reported in the compact form: a single repeated
    /// frame plus a total-frame count and a System.StackOverflowException, rather than
    /// thousands of emitted frames.
    /// </summary>
    [Fact]
    public void StackOverflow_ProducesCompactReport()
    {
        CrashOutputs outputs = _device.RunScenario("stackoverflow");
        CrashReport report = CrashReport.Parse(outputs.Json);

        Assert.Equal("1.0.0", report.ProtocolVersion);
        Assert.Equal("6", report.Signal);

        ReportThread crashed = Assert.Single(report.Threads, t => t.Crashed);
        Assert.True(crashed.IsManaged, "stack-overflow thread should be managed");
        Assert.Equal("System.StackOverflowException", crashed.ManagedExceptionType);
        Assert.Matches(s_hex, crashed.ManagedExceptionHResult!);

        // The runaway recursion is summarized, not enumerated frame-by-frame.
        Assert.Matches(s_digits, crashed.StackOverflowTotalFrames!);
        Assert.True(int.Parse(crashed.StackOverflowTotalFrames!) >= 10,
            $"expected a deep overflow, got {crashed.StackOverflowTotalFrames} frames");

        ReportFrame recursive = Assert.Single(
            crashed.Frames, f => f.MethodName is not null && f.MethodName.StartsWith("Program.StackOverflow", StringComparison.Ordinal));
        Assert.Matches(s_digits, recursive.StackOverflowRepeatCount!);
        Assert.Contains(crashed.Frames, f => f.MethodName is not null && f.MethodName.StartsWith("Program.Main", StringComparison.Ordinal));

        Assert.Contains(".NET Crash Report", outputs.Console);
        Assert.Contains("signal 6 (SIGABRT)", outputs.Console);
        Assert.Contains("(crashed) ---", outputs.Console);
        Assert.Contains("System.StackOverflowException", outputs.Console);
        Assert.Contains("stack overflow frames:", outputs.Console);
        Assert.Contains("Program.StackOverflow", outputs.Console);
    }

    /// <summary>
    /// A managed callback invoked by native code (libc qsort) across a P/Invoke boundary is
    /// unwound back into the originating managed method, exercising the reporter's managed
    /// to native to managed transition handling: callback -> P/Invoke -> caller -> entry.
    /// </summary>
    [Fact]
    public void InterleavedManagedNative_UnwindsCallbackThroughPInvoke()
    {
        CrashOutputs outputs = _device.RunScenario("interleaved");
        CrashReport report = CrashReport.Parse(outputs.Json);

        Assert.Equal("6", report.Signal);

        ReportThread crashed = Assert.Single(report.Threads, t => t.Crashed);

        int comparer = IndexOfMethod(crashed, "Program.CrashingComparer");
        int pinvoke = IndexOfMethod(crashed, "Program.qsort");
        int caller = IndexOfMethod(crashed, "Program.InterleavedManaged");
        int entry = IndexOfMethod(crashed, "Program.Main");

        Assert.True(comparer >= 0, "missing the UnmanagedCallersOnly callback frame");
        Assert.True(pinvoke >= 0, "missing the P/Invoke transition frame");
        Assert.True(caller >= 0, "missing the originating managed caller frame");
        Assert.True(entry >= 0, "missing the entry-point frame");
        Assert.True(comparer < pinvoke && pinvoke < caller && caller < entry,
            $"interleaved unwind out of order: comparer={comparer}, qsort={pinvoke}, caller={caller}, main={entry}");

        // The parked workers are still enumerated alongside the crashed thread.
        foreach (string worker in s_workerMethods)
        {
            Assert.True(report.Threads.Any(t => t.HasMethod(worker)), $"expected a thread parked in {worker}");
        }

        Assert.Contains("Program.CrashingComparer", outputs.Console);
        Assert.Contains("Program.qsort", outputs.Console);
        Assert.Contains("Program.InterleavedManaged", outputs.Console);
    }

    /// <summary>
    /// With the reporter enabled but no report root configured (DOTNET_CrashReportRootPath unset),
    /// the console report is still emitted but no *.crashreport.json file is written.
    /// </summary>
    [Fact]
    public void ConsoleOnly_EmitsReportButWritesNoJson()
    {
        CrashOutputs outputs = _device.RunScenario(
            "abort",
            collectJson: false,
            artifactName: "console-only");

        // The absence of any JSON report is enforced by the fixture; Json is empty here.
        Assert.Equal(string.Empty, outputs.Json);

        Assert.Contains(".NET Crash Report", outputs.Console);
        Assert.Contains("signal 6 (SIGABRT)", outputs.Console);
        Assert.Contains("(crashed) ---", outputs.Console);
        Assert.Contains("Program.Crash", outputs.Console);
    }
}

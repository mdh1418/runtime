// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Runtime.InteropServices;
using System.Threading;

// Layer-2 real-crash integration harness for the in-proc crash reporter.
//
// Unlike the Layer-1 synthetic tests (which recompile the reporter and feed it
// fabricated callback data so the process survives and returns exit 100), this
// drives a REAL fatal crash through the REAL reporter that lives in libcoreclr.
//
// With the reporter enabled (DOTNET_EnableCrashReport=1) the runtime's signal
// path hands the fatal failure to the in-proc reporter, which writes its compact
// console report to logcat (tag DOTNET_CRASH) and a *.crashreport.json file (path
// under DOTNET_CrashReportRootPath), and then the runtime aborts. The process
// therefore always terminates via SIGABRT regardless of the original fault.
//
// This single app serves the whole scenario matrix: the host-driven test selects
// the scenario, the report root, and the reporter-enable flag at launch time via
// `am instrument -e env:KEY VALUE` extras (MonoRunner turns those into environment
// variables), so no scenario state is baked into the APK.
public static class Program
{
    private static readonly ManualResetEventSlim s_workersReady = new ManualResetEventSlim(false);
    private static int s_workersStarted;

    private const int SIGSEGV = 11;
    private const int WorkerCount = 3;

    // A unique token the host can grep for in logcat to scope a run.
    private const string ReadyMarker = "[InProcCrashReport.RealCrash] ready-to-crash";

    public static int Main()
    {
        string scenario = Environment.GetEnvironmentVariable("CRASH_SCENARIO") ?? "abort";
        Console.WriteLine($"[InProcCrashReport.RealCrash] scenario={scenario}");

        // Background workers park in distinct call stacks so the crash report has
        // multiple identifiable threads to enumerate. They never crash; exactly one
        // designated thread triggers the fatal fault.
        StartParkedWorker("DatabaseWorker", static () => QueryDatabase("SELECT * FROM Users WHERE active = 1"));
        StartParkedWorker("HttpProcessor", static () => ProcessRequest("/api/users"));
        StartParkedWorker("TimerCallback", static () => FlushMetrics());

        s_workersReady.Wait();

        // Give the workers a moment to reach their parked (Sleep) frames so the
        // report captures them mid-work rather than mid-startup.
        Thread.Sleep(200);

        // Emit a stable marker immediately before crashing so the host can confirm
        // the app reached the crash point (and scope logcat to this run).
        Console.WriteLine(ReadyMarker);

        switch (scenario)
        {
            case "abort":
                Crash(static () => abort());
                break;

            case "sigsegv":
                Crash(static () => raise(SIGSEGV));
                break;

            case "unhandled":
                Crash(static () => throw new InvalidOperationException("Layer-2 unhandled managed exception"));
                break;

            case "multithread":
                // Same fatal fault, but raised from a named non-main managed thread
                // while the other workers stay parked. Exercises a crash on a thread
                // other than the entry thread with several live threads in the report.
                var crasher = new Thread(static () => Crash(static () => abort()))
                {
                    Name = "CrashWorker",
                    IsBackground = false,
                };
                crasher.Start();
                crasher.Join();
                break;

            case "generics":
                // Crash through generic-method instantiations so the report has to
                // render generic frames.
                Crash(static () => GenericCrash<string, int>("session-key", 42));
                break;

            case "interleaved":
                // Managed -> native (libc) -> managed (callback) -> fault, so the
                // crashed thread's stack interleaves managed and native frames.
                InterleavedManaged();
                break;

            case "stackoverflow":
                // Deliberately NOT through the Crash() chain: a deep self-recursive
                // managed stack genuinely overflows on the entry thread.
                StackOverflow(0);
                break;

            default:
                Console.WriteLine($"[InProcCrashReport.RealCrash] unknown scenario '{scenario}'");
                return 1;
        }

        // Unreachable: every scenario terminates the process before returning.
        return -1;
    }

    private static void StartParkedWorker(string name, Action work)
    {
        var thread = new Thread(() =>
        {
            if (Interlocked.Increment(ref s_workersStarted) == WorkerCount)
            {
                s_workersReady.Set();
            }

            work();
        })
        {
            Name = name,
            IsBackground = true,
        };
        thread.Start();
    }

    // Nested frames so the crashing thread has a non-trivial managed call stack.
    private static void Crash(Action fault) => CrashLevel1(fault);
    private static void CrashLevel1(Action fault) => CrashLevel2(fault);
    private static void CrashLevel2(Action fault) => fault();

    // Generic instantiations in the crashing call stack: validates the report renders
    // generic-method frames.
    private static void GenericCrash<TKey, TValue>(TKey key, TValue value) => GenericCrashInner<TKey, TValue>(key, value);

    private static void GenericCrashInner<TKey, TValue>(TKey key, TValue value)
    {
        GC.KeepAlive(key);
        GC.KeepAlive(value);
        abort();
    }

    // Managed -> native (libc qsort) -> managed callback -> fault. The reporter renders the
    // P/Invoke transition as managed `qsort` frames, so this validates that a native-invoked
    // managed callback is unwound back through the P/Invoke into the originating managed code.
    private static unsafe void InterleavedManaged()
    {
        int* items = stackalloc int[4] { 4, 3, 2, 1 };
        qsort(items, 4, (nuint)sizeof(int), &CrashingComparer);
    }

    [UnmanagedCallersOnly]
    private static unsafe int CrashingComparer(void* left, void* right)
    {
        abort();
        return 0;
    }

    // Self-recursion that defeats tail-call optimization (the stackalloc and the use of
    // the recursive result keep each frame live) so the managed stack genuinely overflows.
    private static long StackOverflow(int depth)
    {
        Span<long> guard = stackalloc long[16];
        guard[depth & 0xF] = depth;
        return guard[depth & 0xF] + StackOverflow(depth + 1);
    }

    private static void QueryDatabase(string sql) => PrepareStatement(sql);
    private static void PrepareStatement(string sql) => Thread.Sleep(Timeout.Infinite);

    private static void ProcessRequest(string path) => ValidateAuth(path);
    private static void ValidateAuth(string path) => Thread.Sleep(Timeout.Infinite);

    private static void FlushMetrics() => Thread.Sleep(Timeout.Infinite);

    [DllImport("libc")]
    private static extern void abort();

    [DllImport("libc")]
    private static extern int raise(int sig);

    [DllImport("libc")]
    private static extern unsafe void qsort(void* baseAddr, nuint num, nuint size, delegate* unmanaged<void*, void*, int> compare);
}

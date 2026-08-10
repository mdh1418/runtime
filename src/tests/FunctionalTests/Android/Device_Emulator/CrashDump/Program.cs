// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Diagnostics;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Threading;

public static class Program
{
    private static readonly ManualResetEventSlim s_ready = new(false);

    public static int Main(string[] args)
    {
        string scenario = Environment.GetEnvironmentVariable("CRASH_SCENARIO") ?? "abort-thread";
        Console.WriteLine($"[CrashDumpTest] Scenario: {scenario}");

        Thread dbThread = new(DatabaseWorker) { Name = "DatabaseWorker", IsBackground = true };
        Thread httpThread = new(HttpRequestProcessor) { Name = "HttpProcessor", IsBackground = true };
        Thread timerThread = new(TimerCallback) { Name = "TimerCallback", IsBackground = true };
        dbThread.Start();
        httpThread.Start();
        timerThread.Start();

        Thread.Sleep(200);
        s_ready.Set();

        Thread.Sleep(500);
        Console.WriteLine("[CrashDumpTest] Crashing now...");

        switch (scenario)
        {
            case "abort":
                Level1(abort);
                break;
            case "sigsegv":
                Level1(() => memset(IntPtr.Zero, 0, 1));
                break;
            case "sigsegv_raise":
                Level1(() => raise(11));
                break;
            case "failfast":
                Level1(() => Environment.FailFast("Test FailFast"));
                break;
            case "unhandled":
                Level1(() => throw new InvalidOperationException("Test unhandled"));
                break;
            case "stackoverflow":
                LevelN();
                break;
            case "ping":
                Level1(TestPing);
                break;
            case "process":
                Level1(TestProcessStart);
                break;
            case "abort-thread":
                TestAbortOnThread();
                break;
            default:
                Console.WriteLine($"[CrashDumpTest] Unknown scenario: {scenario}");
                return 1;
        }

        return 42;
    }

    private static void Level1(Action action) => Level2(action);
    private static void Level2(Action action) => Level3(action);
    private static void Level3(Action action) => action();
    private static void LevelN() => LevelN();

    private static void DatabaseWorker()
    {
        s_ready.Wait();
        ExecuteQuery("SELECT * FROM Users WHERE active = 1");
    }

    private static void ExecuteQuery(string sql) => PrepareStatement(sql);
    private static void PrepareStatement(string sql) => Thread.Sleep(Timeout.Infinite);

    private static void HttpRequestProcessor()
    {
        s_ready.Wait();
        ProcessRequest("/api/users", "GET");
    }

    private static void ProcessRequest(string path, string method) => ValidateAuth(path);
    private static void ValidateAuth(string path) => Thread.Sleep(Timeout.Infinite);

    private static void TimerCallback()
    {
        s_ready.Wait();
        OnTimerElapsed();
    }

    private static void OnTimerElapsed() => FlushMetrics();
    private static void FlushMetrics() => Thread.Sleep(Timeout.Infinite);

    [DllImport("libc", EntryPoint = "abort")]
    private static extern void abort();

    [DllImport("libc", EntryPoint = "memset")]
    private static extern IntPtr memset(IntPtr destination, int character, nuint count);

    [DllImport("libc", EntryPoint = "raise")]
    private static extern int raise(int signal);

    private static void TestPing()
    {
        Console.WriteLine("[CrashDumpTest] Checking raw ICMP socket availability...");
        try
        {
            using Socket rawSocket = new(AddressFamily.InterNetwork, SocketType.Raw, ProtocolType.Icmp);
            Console.WriteLine("[CrashDumpTest] Raw ICMP socket created successfully.");
            Console.WriteLine("[CrashDumpTest] Ping.Send will use raw sockets instead of Process.Start on this device.");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[CrashDumpTest] Raw ICMP socket creation failed: {ex.GetType().Name}: {ex.Message}");
            Console.WriteLine("[CrashDumpTest] Ping.Send should fall back to /system/bin/ping via Process.Start.");
        }

        Console.WriteLine("[CrashDumpTest] Attempting Ping.Send on worker thread...");
        Thread pingThread = new(PingThreadProc) { Name = "PingWorkerThread" };
        pingThread.Start();
        pingThread.Join();
    }

    private static void PingThreadProc()
    {
        Console.WriteLine($"[CrashDumpTest] Ping running on thread: {Thread.CurrentThread.Name} (ManagedThreadId={Thread.CurrentThread.ManagedThreadId})");
        using Ping pingSender = new();
        pingSender.Send("8.8.8.8", 500);
        Console.WriteLine("[CrashDumpTest] Ping completed successfully.");
    }

    private static void TestProcessStart()
    {
        Console.WriteLine("[CrashDumpTest] Testing Process.Start directly...");
        Thread processThread = new(ProcessStartThreadProc) { Name = "ProcessStartWorker" };
        processThread.Start();
        processThread.Join();
        Console.WriteLine("[CrashDumpTest] Process.Start test completed.");
    }

    private static void ProcessStartThreadProc()
    {
        Console.WriteLine($"[CrashDumpTest] Process.Start running on thread: {Thread.CurrentThread.Name}");

        ProcessStartInfo psi = new()
        {
            FileName = "/system/bin/ping",
            Arguments = "-c 1 127.0.0.1",
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };

        using Process? process = Process.Start(psi);
        if (process is null)
        {
            Console.WriteLine("[CrashDumpTest] Process.Start returned null.");
            return;
        }

        process.WaitForExit();
        Console.WriteLine($"[CrashDumpTest] Process.Start completed, exit code: {process.ExitCode}");
    }

    private static void TestAbortOnThread()
    {
        Console.WriteLine("[CrashDumpTest] Testing abort from named worker thread...");
        Thread abortThread = new(AbortThreadProc) { Name = "AbortWorkerThread" };
        abortThread.Start();
        abortThread.Join();
    }

    private static void AbortThreadProc()
    {
        Console.WriteLine($"[CrashDumpTest] Calling abort() on thread: {Thread.CurrentThread.Name} (ManagedThreadId={Thread.CurrentThread.ManagedThreadId})");
        Level1(abort);
    }
}

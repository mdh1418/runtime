// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

public static class Program
{
    private const int Success = 100;
    private const int Failure = 1;
    private const string NativeLib = "libmonodroid";

    private static bool s_workerReady;

    [DllImport(NativeLib)]
    private static extern int InProcCrashReportTest_CreateRealOnDemandReports();

    public static int Main()
    {
        var worker = new Thread(ParkedWorker)
        {
            Name = "OnDemandBackgroundWorker",
            IsBackground = true,
        };
        worker.Start();
        while (!Volatile.Read(ref s_workerReady))
        {
            Thread.Yield();
        }

        int result = CaptureReports();
        if (result != 0)
        {
            Console.WriteLine($"FAIL: native real-runtime on-demand driver returned {result}");
            return Failure;
        }

        Console.WriteLine("PASS: actual CoreCLR generated repeatable on-demand reports with real VM stacks");
        return Success;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int CaptureReports() =>
        InProcCrashReportTest_CreateRealOnDemandReports();

    private static void ParkedWorker()
    {
        Volatile.Write(ref s_workerReady, true);
        Thread.Sleep(Timeout.Infinite);
    }
}

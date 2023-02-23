// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Runtime.InteropServices;

public static class Program
{
    [UnmanagedCallersOnly(EntryPoint="SayHello")]
    public static void SayHello()
    {
        Console.WriteLine("Called from native!  Hello!");
    }

    [UnmanagedCallersOnly(EntryPoint="hdm")]
    public static int hdm()
    {
        return 8141;
    }

    [DllImport("mdh")]
    public static extern int mdh ();

    public static int Main()
    {
        Console.WriteLine("Hello, Android!"); // logcat
        Environment.GetEnvironmentVariable("MONO_LOG_LEVEL");
        Console.WriteLine($"mdh{mdh ()}");
        return 42;
    }
}

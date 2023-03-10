// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Runtime.InteropServices;

public static class Program
{
    [UnmanagedCallersOnly(EntryPoint = nameof(SayHello))]
    public static void SayHello()
    {
        Console.WriteLine("Called from native!  Hello!");
    }

    [UnmanagedCallersOnly(EntryPoint="mdh")]
    public static int mdh()
    {
        return 1418;
    }

    public static int Main()
    {
        return 42;
    }
}

// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using Microsoft.Extensions.Hosting;

namespace NoSpecialEntryPointPatternBuildsThenThrows
{
    public class Program
    {
        public static void Main(string[] args)
        {
            var host = new HostBuilder().Build();

            throw new Exception("Main just throws");
        }
    }
}

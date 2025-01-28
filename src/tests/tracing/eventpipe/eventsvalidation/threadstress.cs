// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Diagnostics;
using System.Diagnostics.Tracing;
using System.Threading;
using Tracing.Tests.Common;
using Xunit;

namespace Tracing.Tests.ThreadStress
{
    /// <summary>
    /// This test validates the behavior of the EventPipe buffer manager under high load.
    /// - An EventListener is created to listen to the in-process native runtime event source.
    /// - The listener enables the ExceptionKeyword (0x8000) to capture "ExceptionThrown_V1" events.
    /// - The test spawns 10,000 short-lived threads to populate the EventPipe Session's ThreadSessionStateList.
    /// - Afterward, it emits 10,000 exceptions to stress the EventPipe infrastructure.
    ///
    /// The goal of the test is to measure how many "ExceptionThrown_V1" events are received by the listener.
    /// Events are often dropped because the `ThreadSessionState` grows whenever there are many short-lived threads.
    /// This growth causes contention and inefficiencies in the EventPipe buffer manager, leading to dropped events.
    /// The test demonstrates the impact of these inefficiencies and validates improvements to the cleanup logic.
    /// </summary>
    internal sealed class ThreadStressEventListener : EventListener
    {
        public int ExceptionEventCount { get; private set; } = 0;

        protected override void OnEventSourceCreated(EventSource source)
        {
            if (source.Name.Equals("Microsoft-Windows-DotNETRuntime"))
            {
                EnableEvents(source, EventLevel.Informational, (EventKeywords)0x8000);
            }
        }

        protected override void OnEventWritten(EventWrittenEventArgs eventData)
        {
            if (eventData.EventName == "ExceptionThrown_V1")
            {
                ExceptionEventCount++;
            }
        }
    }

    public class ThreadStressValidation
    {
        // 10,000 threads reliably cause events to be dropped when each thread emits events.
        // The goal is not to capture all events emitted by these threads but to ensure that
        // some events are dropped, which results in a large number of ThreadSessionStates.
        private const int ThreadCount = 10000;
        private const int ExceptionCount = 10000;

        /// <summary>
        /// Main entry point for the ThreadStressValidation test.
        /// This test demonstrates how event drops occur due to the growth of the ThreadSessionState
        /// when many short-lived threads are created. It measures the number of exceptions received
        /// by the EventListener and validates the impact of cleanup logic in the EventPipe buffer manager.
        /// </summary>
        [Fact]
        public static int TestEntryPoint()
        {
            Console.WriteLine("Starting ThreadStressValidation test...");

            using (var listener = new ThreadStressEventListener())
            {
                // Step 1: Spawn 10,000 short-lived threads to populate and stress the ThreadSessionStateList.
                Console.WriteLine("Spawning short-lived threads to populate the EP Session's Buffer Manager's ThreadSessionStateList...");
                for (int i = 0; i < ThreadCount; i++)
                {
                    var thread = new Thread(() => { try { throw new Exception(); } catch {} });
                    thread.Start();
                    thread.Join(); // Wait for the thread to complete
                }

                Console.WriteLine("Begin throwing exceptions to trigger GetNextEvent...");
                Exception preAllocatedException = new Exception();

                var exceptionCount = listener.ExceptionEventCount;
                for (int i = 0; i < ExceptionCount; i++)
                {
                    try
                    {
                        throw preAllocatedException;
                    }
                    catch { }
                }

                Thread.Sleep(100); // Allow time for the EventListener to process the exception events before capturing the count.
                var newExceptionCount = listener.ExceptionEventCount - exceptionCount;

                Console.WriteLine($"\tEventListener received {newExceptionCount} exception event(s)\n");
                bool pass = newExceptionCount >= .99*ExceptionCount && newExceptionCount <= ExceptionCount;
                if (pass)
                {
                    Console.WriteLine($"Dropped less than 1% of {ExceptionCount} events. ThreadStressValidation test passed!");
                    return 100;
                }
                else
                {
                    Console.WriteLine($"Dropped more than 1% of {ExceptionCount} events. ThreadStressValidation test failed!");
                    return -1;
                }
            }
        }
    }
}

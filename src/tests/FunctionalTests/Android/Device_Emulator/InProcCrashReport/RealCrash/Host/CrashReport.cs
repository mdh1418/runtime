// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Text.Json;

namespace InProcCrashReport.RealCrash.Host;

/// <summary>A single stack frame from a crash-report thread.</summary>
internal sealed record ReportFrame(
    bool IsManaged,
    string? MethodName,
    string? Token,
    string? IlOffset,
    string? Filename,
    string? Guid,
    string? NativeModule,
    string? StackOverflowRepeatCount);

/// <summary>A single thread from a crash report.</summary>
internal sealed record ReportThread(
    bool IsManaged,
    bool Crashed,
    string? NativeThreadId,
    bool HasContext,
    string? ContextIp,
    string? ContextSp,
    string? ManagedExceptionType,
    string? ManagedExceptionHResult,
    string? StackOverflowTotalFrames,
    IReadOnlyList<ReportFrame> Frames)
{
    public bool HasMethod(string methodName) => Frames.Any(f => f.MethodName == methodName);
}

/// <summary>
/// A parsed view of a <c>*.crashreport.json</c> payload, exposing only the fields the
/// fidelity assertions care about. Volatile values (addresses, ids, timestamps) are
/// preserved as raw strings so tests can assert their <em>shape</em> without pinning values.
/// </summary>
internal sealed class CrashReport
{
    public required string ProtocolVersion { get; init; }
    public required string Architecture { get; init; }
    public required string ProcessName { get; init; }
    public required string Pid { get; init; }
    public required string Signal { get; init; }
    public required IReadOnlyList<ReportThread> Threads { get; init; }

    public static CrashReport Parse(string json)
    {
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement root = doc.RootElement;
        JsonElement payload = root.GetProperty("payload");
        JsonElement configuration = payload.GetProperty("configuration");

        var threads = new List<ReportThread>();
        foreach (JsonElement t in payload.GetProperty("threads").EnumerateArray())
        {
            bool hasCtx = t.TryGetProperty("ctx", out JsonElement ctx);
            var frames = new List<ReportFrame>();
            if (t.TryGetProperty("stack_frames", out JsonElement frameArray))
            {
                foreach (JsonElement f in frameArray.EnumerateArray())
                {
                    frames.Add(new ReportFrame(
                        IsManaged: GetString(f, "is_managed") == "true",
                        MethodName: GetString(f, "method_name"),
                        Token: GetString(f, "token"),
                        IlOffset: GetString(f, "il_offset"),
                        Filename: GetString(f, "filename"),
                        Guid: GetString(f, "guid"),
                        NativeModule: GetString(f, "native_module"),
                        StackOverflowRepeatCount: GetString(f, "stack_overflow_repeat_count")));
                }
            }

            threads.Add(new ReportThread(
                IsManaged: GetString(t, "is_managed") == "true",
                Crashed: GetString(t, "crashed") == "true",
                NativeThreadId: GetString(t, "native_thread_id"),
                HasContext: hasCtx,
                ContextIp: hasCtx ? GetString(ctx, "IP") : null,
                ContextSp: hasCtx ? GetString(ctx, "SP") : null,
                ManagedExceptionType: GetString(t, "managed_exception_type"),
                ManagedExceptionHResult: GetString(t, "managed_exception_hresult"),
                StackOverflowTotalFrames: GetString(t, "stack_overflow_total_frames"),
                Frames: frames));
        }

        return new CrashReport
        {
            ProtocolVersion = GetString(payload, "protocol_version") ?? string.Empty,
            Architecture = GetString(configuration, "architecture") ?? string.Empty,
            ProcessName = GetString(payload, "process_name") ?? string.Empty,
            Pid = GetString(payload, "pid") ?? string.Empty,
            Signal = root.TryGetProperty("parameters", out JsonElement p) ? GetString(p, "signal") ?? string.Empty : string.Empty,
            Threads = threads,
        };
    }

    private static string? GetString(JsonElement element, string name)
        => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String
            ? value.GetString()
            : null;
}

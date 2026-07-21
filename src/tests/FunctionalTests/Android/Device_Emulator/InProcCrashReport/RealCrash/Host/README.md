# In-proc crash reporter — Layer 2 (real-crash) host-driven tests

This directory contains **Layer 2** of the in-proc crash reporter tests: an
integration check that a **real** fatal crash, handled by the **real** reporter
inside `libcoreclr`, produces a high-fidelity crash report. It complements
**Layer 1** (`../../` synthetic `/t:Test` apps that recompile the reporter and
feed it fabricated data so the process survives and returns exit 100).

## Why this is host-driven (and not a `/t:Test` functional test)

A real fatal signal happens inside the Android *instrumentation* process. Our
runtime re-raises `SIGABRT`, so the process always dies by signal and XHarness
classifies the run as **APP_CRASH** regardless of `--expected-exit-code`. There
is therefore **no way to express a real in-proc-reporter crash as a green
`/t:Test`**. The verdict must come from a *host* process that:

1. launches the crash app (and expects it to crash),
2. pulls the `*.crashreport.json` and the `DOTNET_CRASH` logcat output, and
3. asserts they have a stable structure.

So the test logic lives here, as an ordinary desktop xUnit project that shells
out to `adb`. The host process's pass/fail is the verdict.

## Components

| Path | Role |
| --- | --- |
| `../CrashApp/` | One CoreCLR Android app that triggers a real crash. The scenario, report path, and reporter-enable flag are supplied **at launch** via `am instrument -e env:KEY VALUE` (MonoRunner turns these into environment variables), so a single APK serves the whole matrix. Marked `IgnoreForCI` so it is never auto-run as a standalone (always-APP_CRASH) test. |
| `Host/` | This xUnit harness. Installs the APK once, drives each scenario, validates both outputs. |

### Scenario matrix

| Scenario | Fault | `parameters.signal` | Distinctive assertion |
| --- | --- | --- | --- |
| `abort` | `libc abort()` | 6 (SIGABRT) | no `managed_exception_*` |
| `sigsegv` | `libc raise(SIGSEGV)` | 11 (SIGSEGV) | crashed on entry thread |
| `unhandled` | unhandled `InvalidOperationException` | 6 (SIGABRT) | `managed_exception_type = System.InvalidOperationException` |
| `multithread` | `abort()` on a named non-entry thread | 6 (SIGABRT) | crash off the entry thread; entry thread parked in `Join` |
| `generics` | `abort()` through generic methods | 6 (SIGABRT) | `Program.GenericCrash` / `GenericCrashInner` frames present (rendered without type args) |
| `stackoverflow` | deep self-recursion | 6 (SIGABRT) | **compact form**: `System.StackOverflowException`, `stack_overflow_total_frames`, a single repeated `Program.StackOverflow` frame with `stack_overflow_repeat_count` (only the crashed thread is emitted) |
| `interleaved` | `abort()` in an `UnmanagedCallersOnly` callback invoked by `libc qsort` | 6 (SIGABRT) | crashed stack unwinds the callback back through the P/Invoke: `CrashingComparer` → `qsort` → `InterleavedManaged` → `Main` |
| console-only | `abort()` with `DOTNET_DbgMiniDumpName` **unset** | 6 (SIGABRT) | reporter emits the console report but writes **no** `*.crashreport.json` |

The first five rows (`abort`, `sigsegv`, `unhandled`, `multithread`, `generics`) share one
`[Theory]` and also assert: protocol version, architecture, process name, pid shape,
the nested `Crash -> CrashLevel1 -> CrashLevel2` chain in stack order, well-formed
managed-frame shape (token / il_offset / filename / guid), and that all three
parked background workers (`PrepareStatement`, `ValidateAuth`, `FlushMetrics`) are
enumerated. The three odd-shaped cases (`stackoverflow`, `interleaved`, console-only)
have their own test methods because their report shapes diverge (compact SO form,
P/Invoke-interleaved stack, no-JSON gating). Volatile values (addresses, ids,
timestamps, pid, commit) are asserted by **shape only**, never pinned.

## On-device file path (SELinux)

The report is written to the app's **internal** data dir
(`/data/data/<pkg>/files/...`). An installed app (domain `untrusted_app`) is
blocked by **SELinux** from writing under `/data/local/tmp` (`shell_data_file`)
even when DAC permissions look open, so `DOTNET_DbgMiniDumpName` must point at an
app-owned location. The host pulls the file with
`adb exec-out run-as <pkg> cat files/<name>` (the app is debuggable).

## Running locally

1. For a fresh Windows worktree, initialize the repository SDK and produce one
   coherent Android CoreCLR baseline:

   ```pwsh
   $env:ANDROID_SDK_ROOT = "$env:LOCALAPPDATA\Android\Sdk"
   $env:ANDROID_NDK_ROOT = "$env:ANDROID_SDK_ROOT\ndk\27.2.12479018"
   $env:PATH = "$env:ANDROID_SDK_ROOT\platform-tools;$(Resolve-Path .\.dotnet);$env:PATH"

   .\eng\common\dotnet.cmd --info
   .\build.cmd -s clr+libs -os android -arch x64 -c Release
   ```

2. Build the crash APK from the repo root:

   ```pwsh
   .\.dotnet\dotnet.exe build -c Release `
     src\tests\FunctionalTests\Android\Device_Emulator\InProcCrashReport\RealCrash\CrashApp\Android.Device_Emulator.InProcCrashReport.RealCrash.CrashApp.csproj `
     /p:TargetOS=android /p:TargetArchitecture=x64 /p:RuntimeFlavor=coreclr /p:RuntimeConfiguration=Release
   ```

3. With an emulator running and `adb` on `PATH`, run the harness with a standalone
   .NET SDK:

   ```pwsh
   cd src\tests\FunctionalTests\Android\Device_Emulator\InProcCrashReport\RealCrash\Host
   $env:INPROC_CRASH_DEVICE_ID = "emulator-5554"   # optional if exactly one device
   $env:INPROC_CRASH_ADB = "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe"
   dotnet test
   ```

The complete host matrix was rerun from a fresh worktree on 2026-07-21 and
passed 8 of 8 tests on an API 36 x64 emulator.

### Configuration (environment variables, all optional)

| Variable | Default |
| --- | --- |
| `INPROC_CRASH_DEVICE_ID` | the only attached device (else required) |
| `INPROC_CRASH_APK` | the repo artifacts path baked in at build time |
| `INPROC_CRASH_ADB` | `adb` on `PATH` |

This project is deliberately isolated from the dotnet/runtime build (empty
`Directory.Build.*`, local `NuGet.config`, local `global.json`) so it restores
the xUnit packages from nuget.org and builds with a standalone SDK.

## CI / Helix packaging — DESIGNED, NOT YET VALIDATED

> [!IMPORTANT]
> The harness is green **locally**. The CI path below is a design only; it has
> **not** been validated on real Helix infrastructure and is the remaining gap.

In dotnet/runtime CI, Android device access lives on **Helix agents** (the build
machine has no device). A host process that shells to `adb` must therefore run
**on the Helix Android agent**, not the build machine. The proposed packaging:

1. Build the CrashApp APK (already produced; `IgnoreForCI` keeps it from being
   archived as a runnable XHarness test).
2. Publish this host harness as a self-contained binary for the agent OS.
3. Ship both, plus a run command, as a **single Helix work item on the Android
   queue**, where the command runs the harness against the agent's emulator via
   `adb`. The work item's exit code is the verdict.

The recommended first step before scaling is to prove **one** scenario
end-to-end as a Helix work item (verify `adb devices`, install, drive, validate,
return green despite APP_CRASH), then wire in the rest.

# In-proc crash reporter tests — overview & status

> **Status doc for a long-running effort.** This was written to hand the work off
> to a future self/PR after a multi-month pause. It captures the goal, what is
> done, what remains, and — most importantly — the discoveries and design
> decisions that shaped *why* the tests look the way they do, so the rationale is
> not lost.

## Goal

The CoreCLR **in-proc crash reporter** (`src/coreclr/debug/crashreport/`) runs
only on **Android and iOS CoreCLR**. On a real fatal crash it emits two artifacts:

1. a **compact console report** (Android: logcat, tag `DOTNET_CRASH`), and
2. a lifecycle-managed **`*.crashreport.json`** file under
   `DOTNET_CrashReportRootPath/.dotnet/crash-reports/`.

We want **regression tests that validate the fidelity** of both outputs across a
matrix of crash scenarios — *not* a byte-for-byte match (much of the report is
inherently run-time-dependent: addresses, thread ids, timestamps, pid, commit
hash), but a check that the **structure is stable and does not regress**.

The reporter also supports programmatic, on-demand reports through a
caller-provided output callback. The synthetic fidelity suite validates that API independently from
the fatal-signal services and lifecycle-managed file output.

This effort currently targets **Android CoreCLR on an x64 emulator** (Windows
host). iOS and other Android architectures are out of scope for now.

## Two complementary test suites (the central design decision)

A real fatal crash cannot be expressed as a normal green test, so the suite is
split into two complementary layers:

| | **Synthetic fidelity tests** | **Real-crash integration tests** |
| --- | --- | --- |
| Location | `./` (`Shared/` + per-scenario projects) | `./RealCrash/` |
| What crashes | nothing — the process **survives** | a **real** fatal signal |
| How | recompiles the **real reporter sources** into a test app and feeds them **fabricated** callback data via a synthetic native driver | drives a **real** crash through the **real** reporter inside `libcoreclr` |
| Verdict | app returns exit **100** → green XHarness `/t:Test` | a **host** process (xUnit over `adb`) inspects the pulled artifacts |
| Runs in CI as | a standard functional `/t:Test` | (designed) a Helix work item; **not yet wired** |
| Strength | fast, deterministic, hermetic; can probe code paths hard to trigger for real | true end-to-end fidelity, real unwinder/signal path |

Why both: the synthetic suite gives cheap, deterministic, CI-ready coverage of the
report-*formatting* code, while the real-crash suite proves the whole real crash→report
pipeline actually works on a device. They validate the same reporter from
opposite ends.

## What has been done

### Synthetic fidelity suite (GREEN locally, CI-ready)

- A shared-source multi-project suite under `Shared/` imported by five
  `.csproj`s. Each fatal-signal scenario is its **own** functional-test project,
  so one scenario == one app launch. The on-demand project generates multiple
  reports in one process to validate repeatability.
- The scenario is selected by an `INPROC_SCENARIO_*` **compile constant**
  (`InProcCrashReport.Common.props`); everything else (managed harness, native
  driver, PAL/minipal/watchdog shims, the real reporter `.cpp`s) is shared.
- The real reporter sources (formatters, lifecycle manager, and reporter) are
  compiled into the app via `ExtraAppNativeSources`. The watchdog is **stubbed inert**
  (`inproccrashreportwatchdog_teststub.cpp`) — it reuses the real *header* so the
  contract stays in sync, but avoids pulling in `pal/signal.hpp` and a live pthread.
- Fatal-signal scenario projects: `RichSigsegv/`, `Abort/`, `StackOverflow/`,
  and `ConsoleOnly/`. They validate both JSON and the compact console report.
- The `OnDemand/` project generates JSON twice and compact log output once
  through caller-provided sinks. It validates both formats, repeatability,
  null-callback rejection, nested in-flight rejection,
  and isolation from lifecycle-managed files without initializing signal-path
  services.
- Build-task enabler: `CMakeLists-android.txt` (AndroidAppBuilder template) was
  changed to compile C++ `ExtraAppNativeSources` (previously C-only), which is
  what lets the app embed the reporter `.cpp`s.

### Real-crash integration suite (GREEN locally: 8/8 in ~41s)

- **One** CoreCLR Android app (`RealCrash/CrashApp/`) triggers the crash. The
  scenario, report root, and reporter-enable flag are supplied **at launch** as
  environment variables, so a single APK serves the entire matrix. Marked
  `IgnoreForCI` (it is always-APP_CRASH; it must never be auto-run as a `/t:Test`).
- A host **xUnit** project (`RealCrash/Host/`) installs the APK once, drives each
  scenario over raw `adb`, pulls the JSON + the `DOTNET_CRASH` logcat, and asserts
  structural fidelity (shape only). See `RealCrash/Host/README.md` for run steps.

#### Real-crash scenario matrix (all signal 6 / SIGABRT unless noted)

| Scenario | Fault | Notable shape |
| --- | --- | --- |
| `abort` | `libc abort()` | baseline; no `managed_exception_*` |
| `sigsegv` | `libc raise(SIGSEGV)` | `parameters.signal` = 11 |
| `unhandled` | unhandled `InvalidOperationException` | `managed_exception_type = System.InvalidOperationException` (hresult `0x80131509`) |
| `multithread` | `abort()` on a named non-entry thread | crash off the entry thread; entry thread parked in `Thread.Join` |
| `generics` | `abort()` through generic methods | generic frames render **without** type args (`Program.GenericCrash`) |
| `stackoverflow` | deep self-recursion | **compact form** — see discoveries |
| `interleaved` | `abort()` in an `UnmanagedCallersOnly` callback invoked by libc `qsort` | managed→native→managed unwind through the P/Invoke |
| console-only | `abort()` with `DOTNET_CrashReportRootPath` **unset** | console report emitted, **no** JSON file |

The first five share one `[Theory]`; the last three have dedicated test methods
because their report shapes diverge.

## What is still left to do

- **Watchdog thread killing a stuck/hung reporter.** The synthetic suite stubs the watchdog
  inert; the real-crash suite has no way to force the reporter to hang. Needs investigation of
  a test seam (a hook/env knob to induce a hang) before it can be exercised.
- **Lifecycle retention policy.** Both layers now exercise lifecycle-managed file
  creation and unique-name discovery, but do not validate startup pruning,
  `DOTNET_CrashReportMaxFileCount`, stale temp cleanup, or oldest-file replacement.
- **Deferred-symbolication compact console form** — future reporter feature.
- **CI / Helix wiring for the real-crash suite.** The host harness runs on the **build**
  machine, which has no device. In CI, device access is on **Helix agents**. The
  packaging is **designed but unvalidated** (see `RealCrash/Host/README.md`): ship
  the CrashApp APK + a self-contained host harness as a **single Helix work item**
  on the Android queue, where the item's exit code is the verdict. `IgnoreForCI`
  on the CrashApp keeps it out of the normal test archive, so CI needs a separate
  copy step for the APK.
- **Broader architecture coverage.** Only x64 emulator is validated. `arm64`,
  `arm`, `x86` may differ in signal reporting, unwinding, and callback-frame
  rendering — keep signal/shape assertions loose if these are added.
- **iOS CoreCLR.** Out of scope so far; the same reporter runs there.

## Discoveries & design decisions (the "why")

These are the non-obvious findings that shaped the implementation. They are the
most valuable thing to re-read before resuming.

1. **A real crash can never be a green `/t:Test`.** A fatal signal in the Android
   *instrumentation* process makes `am instrument` report `shortMsg=Process
   crashed`, which XHarness classifies as **APP_CRASH** regardless of
   `--expected-exit-code`. ⇒ The real-crash suite must be **host-driven**: the host launches the
   app (expecting it to crash) and the *host's* pass/fail is the verdict. This is
   the root reason the real-crash suite exists separately from the synthetic suite.

2. **One APK serves the whole matrix.** `MonoRunner` turns
   `am instrument -e env:KEY VALUE` extras into **environment variables set before
   runtime init**. So the host fully controls `CRASH_SCENARIO`,
   `DOTNET_EnableCrashReport`, and `DOTNET_CrashReportRootPath` at launch — no need to
   bake scenarios into per-scenario APKs. (The real-crash suite originally planned 4 APKs; this
   discovery collapsed it to one.)

3. **SELinux blocks the obvious JSON path.** An installed app (`untrusted_app`
   domain) **cannot write under `/data/local/tmp`** (`shell_data_file`) even at
   `0777` — DAC perms are irrelevant. ⇒ `DOTNET_CrashReportRootPath` must point at
   the app-internal dir `/data/data/<pkg>/files`, and the host discovers and pulls
   the file under `files/.dotnet/crash-reports/` via `adb exec-out run-as` (the app
   is debuggable). An earlier
   spike "worked" only because that file was written by the *shell* user.

4. **Reporter enablement knobs (this tree).** `DOTNET_EnableCrashReport=1` plus
   `DOTNET_CrashReportRootPath=<existing-directory>`, both read at startup by
   `CrashReportConfigure` during coreclr init (before `Main`). The reporter creates
   `.dotnet/crash-reports/report-<timestamp>-<pid>.crashreport.json` under the root.

5. **Fidelity = shape, not bytes.** All volatile values (addresses, thread ids,
   timestamps, pid, commit hash) are asserted by **shape** (regex/structure) only,
   never pinned, so the tests catch regressions without churning every build.

6. **Stack overflow uses a special compact report.** A real managed SO is
   reported as **signal 6**, with **only the crashed thread**, a
   `System.StackOverflowException` (hresult `0x800703e9`), a
   `stack_overflow_total_frames` count, and a **single repeated frame** carrying
   `stack_overflow_repeat_count` (no `ctx`, no token/guid). ⇒ It needs its own test
   with its own assertions; the normal per-frame shape checks do not apply. (Layer
   1 exercises the same path synthetically via
   `BeginStackOverflowTrace`/`AddStackOverflowTraceFrame`/`EndStackOverflowTrace`.)

7. **P/Invoke and `UnmanagedCallersOnly` frames render as *managed*.** In the
   `interleaved` scenario, the native libc `qsort` frames are **collapsed**: the
   report shows `is_managed=true` frames named `Program.qsort` (the P/Invoke) and
   `Program.CrashingComparer` (the callback), with **no** `is_managed=false` frame
   between them. ⇒ The assertion can't be "a native frame between managed frames";
   it is instead the **call order** `CrashingComparer → qsort → InterleavedManaged
   → Main`, which still proves the unwinder walked the managed↔native transition.

8. **Generic frames drop their type args.** `Program.GenericCrash<TKey,TValue>`
   renders as bare `Program.GenericCrash`. Assertions match the bare name.

9. **Console-only is the JSON-gating test.** Enabling the reporter **without**
   `DOTNET_CrashReportRootPath` leaves lifecycle-managed file output disabled:
   the console report is still emitted but **no** JSON file is
   written. The test verifies absence via a **before/after snapshot** of the app's
   report files. Pitfall found the hard way: `run-as <pkg> sh -c "rm
   files/*.crashreport.json"` is a **no-op** — `adb shell` splits the args so the
   glob never reaches `sh -c`, and the app-uid shell can't expand it under SELinux
   anyway. Snapshot-diff avoids needing any glob/deletion.

10. **The host project is deliberately isolated from the runtime build.** Empty
    `Directory.Build.props`/`.targets` cut the repo MSBuild import chain; a local
    `NuGet.config` (nuget.org) and local `global.json` (standalone SDK) let it
    restore xUnit and build with the **system** dotnet, independent of the
    in-repo preview SDK and locked feeds. This is why it can run as a plain desktop
    xUnit project.

11. **Local-run gotcha:** if more than one emulator/device is attached, the adb
    auto-detect throws "Multiple attached adb devices"; set
    `INPROC_CRASH_DEVICE_ID` (e.g. `emulator-5554`).

## Layout

```
InProcCrashReport/
├─ OVERVIEW.md                     ← this file
├─ Shared/                         ← synthetic: shared managed harness, native driver,
│                                     reporter-source imports, inert watchdog stub
├─ Abort/  ConsoleOnly/  OnDemand/  RichSigsegv/  StackOverflow/
│                                  ← synthetic fidelity projects
└─ RealCrash/                      ← real-crash integration tests
   ├─ CrashApp/                    ← single on-device crash app (one APK, env-driven)
   └─ Host/                        ← desktop xUnit harness over adb (the verdict)
      └─ README.md                 ← run steps + CI/Helix design (unvalidated)
```

## How to run (quick reference)

- **Complete suite with durable artifacts:** use
  [`Run-InProcCrashReportTests.ps1`](Run-InProcCrashReportTests.ps1). See
  [`FIDELITY.md`](FIDELITY.md) for the result layout, scenario coverage, and
  post-crash fidelity model.
- **Synthetic fidelity tests:** standard functional test, `dotnet build /t:Test <scenario>.csproj`,
  with the Android build environment (expects exit 100 / XHarness green).
- **Real-crash integration tests:** build the CrashApp APK, then run the host harness with the system
  SDK. Full steps are in `RealCrash/Host/README.md`.

### Fresh Windows worktree validation

The complete suite was validated on 2026-07-21 from a fresh Windows worktree
using an API 36 x64 emulator.

From the repository root:

```pwsh
$env:ANDROID_SDK_ROOT = "$env:LOCALAPPDATA\Android\Sdk"
$env:ANDROID_NDK_ROOT = "$env:ANDROID_SDK_ROOT\ndk\27.2.12479018"
$env:ADB_EXE_PATH = "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe"
$env:PATH = "C:\Program Files\Git\usr\bin;$env:ANDROID_SDK_ROOT\platform-tools;$(Resolve-Path .\.dotnet);$env:PATH"

.\eng\common\dotnet.cmd --info
.\build.cmd -s clr+libs -os android -arch x64 -c Release
```

The single coherent baseline build is important. Split runtime and libraries
builds can leave an incompatible `System.Private.CoreLib.dll` in the Android
runtime pack. Current `main` also requires Git for Windows' `usr\bin` on `PATH`
because the NativeAOT libunwind build invokes `sh` and GNU `sort`.

With exactly one emulator attached, run every synthetic fidelity project:

```pwsh
$projects = @(
    "RichSigsegv\Android.Device_Emulator.InProcCrashReport.RichSigsegv.Test.csproj",
    "Abort\Android.Device_Emulator.InProcCrashReport.Abort.Test.csproj",
    "StackOverflow\Android.Device_Emulator.InProcCrashReport.StackOverflow.Test.csproj",
    "ConsoleOnly\Android.Device_Emulator.InProcCrashReport.ConsoleOnly.Test.csproj",
    "OnDemand\Android.Device_Emulator.InProcCrashReport.OnDemand.Test.csproj"
)

foreach ($project in $projects) {
    .\.dotnet\dotnet.exe build -c Release `
        "src\tests\FunctionalTests\Android\Device_Emulator\InProcCrashReport\$project" `
        /t:Test /p:TargetOS=android /p:TargetArchitecture=x64 `
        /p:RuntimeFlavor=coreclr /p:RuntimeConfiguration=Release
}
```

Do not set `AdditionalXHarnessArguments` as an MSBuild command-line property for
these projects. A command-line global property cannot be appended to by
`tests.mobile.targets`, which prevents it from adding
`--expected-exit-code 100`. The app then passes and returns 100, but XHarness
incorrectly reports a failure because it expects 0.

Validated results:

- Synthetic fidelity suite: 5 of 5 passed.
- Real-crash integration suite: 8 of 8 passed.
- Total: 13 of 13 passed.
- Emulator: Android 16, API 36, x86_64 (`emulator-5554`).

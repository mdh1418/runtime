# In-proc crash report fidelity testing

This suite checks that a crash report continues to preserve the evidence needed
to identify why a process crashed. It does not compare complete reports byte for
byte because addresses, process ids, thread ids, timestamps, and build identifiers
change on every run.

## Durable result directory

Run the complete local suite from the repository root:

```pwsh
$env:ANDROID_SDK_ROOT = "$env:LOCALAPPDATA\Android\Sdk"
$env:ANDROID_NDK_ROOT = "$env:ANDROID_SDK_ROOT\ndk\27.2.12479018"
$env:INPROC_CRASH_DEVICE_ID = "emulator-5554"
$env:INPROC_CRASH_ADB = "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe"
$env:PATH = "C:\Program Files\Git\usr\bin;$env:ANDROID_SDK_ROOT\platform-tools;$(Resolve-Path .\.dotnet);$env:PATH"

.\src\tests\FunctionalTests\Android\Device_Emulator\InProcCrashReport\Run-InProcCrashReportTests.ps1
```

The runner requires PowerShell 7 or later. Each output directory must be new or
empty so artifacts from separate runs cannot be mixed.

The script writes a timestamped run under:

```text
artifacts/test-results/InProcCrashReport/<timestamp>-pid<pid>/
├─ README.md
├─ results.json
├─ synthetic/
│  ├─ rich-sigsegv/test.log
│  ├─ abort/test.log
│  ├─ stack-overflow/test.log
│  ├─ console-only/test.log
│  └─ on-demand/test.log
├─ real-on-demand/
│  ├─ test.log
│  ├─ first-report.json
│  ├─ second-report.json
│  ├─ report.log
│  └─ validation.json
└─ real-crash/
   ├─ tests.trx
   ├─ build.log
   ├─ test.log
   ├─ abort/{report.json,console.txt,capture.json,validation.json}
   ├─ sigsegv/{report.json,console.txt,capture.json,validation.json}
   ├─ unhandled/{report.json,console.txt,capture.json,validation.json}
   ├─ multithread/{report.json,console.txt,capture.json,validation.json}
   ├─ generics/{report.json,console.txt,capture.json,validation.json}
   ├─ stackoverflow/{report.json,console.txt,capture.json,validation.json}
   ├─ interleaved/{report.json,console.txt,capture.json,validation.json}
   └─ console-only/{console.txt,capture.json,validation.json}
```

Use `-ResultsDirectory <path>` to select an explicit output directory. When the
real-crash host is run directly, `INPROC_CRASH_RESULTS_DIR` overrides its output
directory. Without the override, it creates a timestamped directory under the
same `artifacts/test-results/InProcCrashReport` root.

The host harness uses the standalone SDK installed under
`%ProgramFiles%\dotnet`. Set `INPROC_CRASH_HOST_DOTNET` if the required SDK is
installed elsewhere.

## Three complementary test suites

### Synthetic fidelity tests

Five Android functional-test apps compile the real reporter sources into the
test app and feed deterministic callback data into them. The process survives,
so each app can parse its generated JSON and compact report and return exit code
100 only after all assertions pass.

| Scenario | Evidence evaluated |
| --- | --- |
| Rich SIGSEGV | Signal 11, register context, managed exception type and HRESULT, three threads, and interleaved managed and native frames |
| Abort | Signal 6, no managed exception, native crash frames, and a managed background thread |
| Stack overflow | Compact stack-overflow form, total frame count, repeated-frame count, exception details, and console markers |
| Console only | Full compact report with lifecycle-managed JSON intentionally disabled |
| On demand | JSON and log formats, two consecutive reports, null callback rejection, nested in-flight rejection, and no lifecycle file creation |

The synthetic logs are retained because XHarness owns and removes each
application after execution. The generated report contents are evaluated inside
the application before the PASS result is emitted.

### Real-runtime on-demand integration test

A dedicated Android app statically links the actual CoreCLR runtime and compiles
a small test wrapper into the same `libmonodroid` image. The wrapper can call the
internal C++ `InProcCrashReportCreateReport` symbol without changing the
production export surface. `DOTNET_EnableCrashReport=1` is embedded in the app
host, so CoreCLR initializes the reporter singleton with the real VM thread and
stack-walk callbacks before managed `Main` runs.

The app creates a parked managed worker, invokes the real API twice for JSON and
once for compact log output, and validates:

- both JSON reports parse as protocol `1.0.0` with signal 6;
- exactly one thread is marked crashed;
- the crashed stack includes the managed requesting method;
- another enumerated thread includes the parked worker method;
- the compact log contains the same signal, crashed-thread marker, requesting
  method, worker method, and module section;
- repeated calls succeed;
- the lifecycle report count does not change.

Because XHarness uninstalls the app, the native wrapper emits base64 chunks of
the exact report bytes into the captured test log. The durable runner reconstructs
`first-report.json`, `second-report.json`, and `report.log`, parses and validates
them again on the host, and writes `validation.json`.

This test exercises the actual reporter singleton and real VM callbacks. It does
not exercise a production caller because PR #131220 adds an internal API for a
future native fatal-error-handler integration; no production caller exists yet.

### Real-crash integration tests

The host xUnit harness launches a real CoreCLR Android app and expects the app to
die. It then pulls the lifecycle-managed JSON through `adb run-as`, captures the
`DOTNET_CRASH` logcat stream, writes both to the durable directory, and evaluates
the same strings in memory. Tests are serialized because installation, logcat,
and the emulator are shared process-wide resources.

| Scenario | Crash reason evidence |
| --- | --- |
| Abort | SIGABRT, no managed exception, nested crash call chain, and parked worker threads |
| SIGSEGV | Signal 11 and a crash on the entry thread |
| Unhandled exception | `System.InvalidOperationException` and its HRESULT |
| Multithread | The crashed thread is not the entry thread; the entry thread remains visible in `Join` |
| Generics | Generic crash methods remain identifiable in the stack |
| Stack overflow | `System.StackOverflowException`, total depth, and compressed recursive frame count |
| Interleaved | Managed callback, P/Invoke transition, managed caller, and entry point remain in unwind order |
| Console only | The compact crash reason is present while no JSON report is created |

## How fidelity is evaluated

The automated assertions answer four questions:

1. **What terminated the process?** The signal number and name must agree with
   the triggered fault.
2. **Was there a managed failure?** Exception type and HRESULT must be present
   for managed failures and absent for native aborts.
3. **Where did it fail?** Exactly one crashed thread must contain the expected
   stack chain, register context where applicable, and scenario-specific frames.
4. **What else was the process doing?** Expected background threads must remain
   enumerable so deadlocks and cross-thread failures can be investigated.

The real-runtime on-demand test asks an additional question: can a live CoreCLR
process request more than one report through the new API and receive useful
real-process evidence without creating a lifecycle-managed crash file?

Stable semantic values are compared exactly. Volatile values are validated by
shape: hexadecimal addresses and tokens, decimal identifiers, GUIDs, non-empty
module names, and relative stack ordering. This catches missing or malformed
diagnostic evidence without making the suite depend on one build or emulator.

## How this helps uncover the crash reason

The suite is a fidelity gate, not a general crash classifier. It ensures that
the report retains the inputs an investigator uses after a crash:

- signal and managed exception identify the failure category;
- the crashed-thread marker isolates the relevant thread;
- register context and the innermost frames locate the failing code;
- ordered managed, native, and transition frames reconstruct the call path;
- stack-overflow compression preserves recursion depth without producing an
  unusably large report;
- the remaining threads provide process-wide context.

When capture succeeds, the retained real-crash artifacts allow a reviewer to
inspect the exact report evaluated by the automated assertions and compare it
with later runs. If capture itself fails, the TRX, host log, and per-scenario
`validation.json` remain available even when no report was produced.

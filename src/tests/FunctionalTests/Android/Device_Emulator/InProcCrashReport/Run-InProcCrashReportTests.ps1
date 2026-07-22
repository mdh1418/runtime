#requires -Version 7.0

[CmdletBinding()]
param(
    [string] $ResultsDirectory
)

$ErrorActionPreference = "Stop"

$repoRoot = (& git -C $PSScriptRoot rev-parse --show-toplevel).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrEmpty($repoRoot))
{
    throw "Could not locate the repository root."
}

if ([string]::IsNullOrEmpty($ResultsDirectory))
{
    $runName = "{0}-pid{1}" -f [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmss"), $PID
    $ResultsDirectory = Join-Path $repoRoot "artifacts\test-results\InProcCrashReport\$runName"
}
else
{
    if (![IO.Path]::IsPathRooted($ResultsDirectory))
    {
        $ResultsDirectory = Join-Path $repoRoot $ResultsDirectory
    }

    $ResultsDirectory = [IO.Path]::GetFullPath($ResultsDirectory)
}

$syntheticDirectory = Join-Path $ResultsDirectory "synthetic"
$realOnDemandDirectory = Join-Path $ResultsDirectory "real-on-demand"
$realCrashDirectory = Join-Path $ResultsDirectory "real-crash"
if (Test-Path $ResultsDirectory -PathType Container)
{
    if (Get-ChildItem $ResultsDirectory -Force | Select-Object -First 1)
    {
        throw "Results directory '$ResultsDirectory' is not empty. Choose a new directory."
    }
}

New-Item -ItemType Directory -Force $syntheticDirectory, $realOnDemandDirectory, $realCrashDirectory | Out-Null

$repoDotnet = Join-Path $repoRoot ".dotnet\dotnet.exe"
$installedDotnet = Join-Path $env:ProgramFiles "dotnet\dotnet.exe"
$hostDotnet = if ($env:INPROC_CRASH_HOST_DOTNET)
{
    $env:INPROC_CRASH_HOST_DOTNET
}
elseif (Test-Path $installedDotnet)
{
    $installedDotnet
}
else
{
    (Get-Command dotnet.exe -ErrorAction Stop).Source
}
$testRoot = Join-Path $repoRoot "src\tests\FunctionalTests\Android\Device_Emulator\InProcCrashReport"
$results = [Collections.Generic.List[object]]::new()

function Invoke-TestCommand
{
    param(
        [string] $Name,
        [string] $Kind,
        [string] $LogPath,
        [scriptblock] $Command
    )

    Write-Host "=== $Name ==="
    & $Command *> $LogPath
    $exitCode = $LASTEXITCODE
    $status = if ($exitCode -eq 0) { "PASS" } else { "FAIL" }
    $results.Add([pscustomobject]@{
        Name = $Name
        Kind = $Kind
        Status = $status
        ExitCode = $exitCode
        Log = [IO.Path]::GetRelativePath($ResultsDirectory, $LogPath)
    })
    Write-Host "$status ($exitCode): $Name"
    return $exitCode
}

function Get-RealCrashScenarioName
{
    param([string] $TestName)

    if ($TestName.Contains("ConsoleOnly_", [StringComparison]::Ordinal))
    {
        return "console-only"
    }
    if ($TestName.Contains("InterleavedManagedNative_", [StringComparison]::Ordinal))
    {
        return "interleaved"
    }
    if ($TestName.Contains("StackOverflow_", [StringComparison]::Ordinal))
    {
        return "stackoverflow"
    }
    if ($TestName -match 'scenario: "([^"]+)"')
    {
        return $Matches[1]
    }

    return $null
}

function Export-EmbeddedArtifacts
{
    param(
        [string] $LogPath,
        [string] $OutputDirectory
    )

    $artifacts = @{}
    foreach ($line in Get-Content $LogPath)
    {
        if ($line -match 'INPROC_ARTIFACT:([^:]+):(\d+):(\d+):([A-Za-z0-9+/=]+)')
        {
            $name = $Matches[1]
            if (!$artifacts.ContainsKey($name))
            {
                $artifacts[$name] = [Collections.Generic.List[object]]::new()
            }

            $artifacts[$name].Add([pscustomobject]@{
                Index = [int]$Matches[2]
                Count = [int]$Matches[3]
                Data = $Matches[4]
            })
        }
    }

    foreach ($name in @("first-report.json", "second-report.json", "report.log"))
    {
        if (!$artifacts.ContainsKey($name))
        {
            throw "Test output did not contain embedded artifact '$name'."
        }

        $chunks = @($artifacts[$name] | Sort-Object Index)
        $expectedCount = $chunks[0].Count
        if ($chunks.Count -ne $expectedCount -or
            $chunks.Where({ $_.Count -ne $expectedCount }).Count -ne 0)
        {
            throw "Embedded artifact '$name' has an incomplete chunk set."
        }
        for ($index = 0; $index -lt $chunks.Count; $index++)
        {
            if ($chunks[$index].Index -ne $index)
            {
                throw "Embedded artifact '$name' is missing chunk $index."
            }
        }

        $bytes = [Convert]::FromBase64String(($chunks.Data -join ""))
        [IO.File]::WriteAllBytes((Join-Path $OutputDirectory $name), $bytes)
    }
}

$syntheticProjects = [ordered]@{
    "rich-sigsegv" = "RichSigsegv\Android.Device_Emulator.InProcCrashReport.RichSigsegv.Test.csproj"
    "abort" = "Abort\Android.Device_Emulator.InProcCrashReport.Abort.Test.csproj"
    "stack-overflow" = "StackOverflow\Android.Device_Emulator.InProcCrashReport.StackOverflow.Test.csproj"
    "console-only" = "ConsoleOnly\Android.Device_Emulator.InProcCrashReport.ConsoleOnly.Test.csproj"
    "on-demand" = "OnDemand\Android.Device_Emulator.InProcCrashReport.OnDemand.Test.csproj"
}

foreach ($entry in $syntheticProjects.GetEnumerator())
{
    $scenarioDirectory = Join-Path $syntheticDirectory $entry.Key
    New-Item -ItemType Directory -Force $scenarioDirectory | Out-Null
    $project = Join-Path $testRoot $entry.Value
    $log = Join-Path $scenarioDirectory "test.log"

    Invoke-TestCommand $entry.Key "synthetic" $log {
        & $repoDotnet build -c Release $project /t:Test `
            /p:TargetOS=android /p:TargetArchitecture=x64 `
            /p:RuntimeFlavor=coreclr /p:RuntimeConfiguration=Release --nologo
    } | Out-Null
}

$realOnDemandProject = Join-Path $testRoot "RealOnDemand\Android.Device_Emulator.InProcCrashReport.RealOnDemand.Test.csproj"
$realOnDemandLog = Join-Path $realOnDemandDirectory "test.log"
$realOnDemandExitCode = Invoke-TestCommand "real-runtime-on-demand" "real-on-demand" $realOnDemandLog {
    & $repoDotnet build -c Release $realOnDemandProject /t:Test `
        /p:TargetOS=android /p:TargetArchitecture=x64 `
        /p:RuntimeFlavor=coreclr /p:RuntimeConfiguration=Release `
        /p:ArchiveTests=false --nologo
}

if ($realOnDemandExitCode -eq 0)
{
    try
    {
        Export-EmbeddedArtifacts $realOnDemandLog $realOnDemandDirectory
        $firstReport = Get-Content -Raw (Join-Path $realOnDemandDirectory "first-report.json") | ConvertFrom-Json
        $secondReport = Get-Content -Raw (Join-Path $realOnDemandDirectory "second-report.json") | ConvertFrom-Json
        $compactLog = Get-Content -Raw (Join-Path $realOnDemandDirectory "report.log")
        $firstThreads = @($firstReport.payload.threads)
        $secondThreads = @($secondReport.payload.threads)
        $firstMethods = @($firstThreads.stack_frames.method_name)
        $secondMethods = @($secondThreads.stack_frames.method_name)
        $validation = [ordered]@{
            firstProtocol = $firstReport.payload.protocol_version
            secondProtocol = $secondReport.payload.protocol_version
            firstSignal = $firstReport.parameters.signal
            secondSignal = $secondReport.parameters.signal
            firstThreadCount = $firstThreads.Count
            secondThreadCount = $secondThreads.Count
            firstCrashedThreadCount = @($firstThreads.Where({ $_.crashed -eq "true" })).Count
            secondCrashedThreadCount = @($secondThreads.Where({ $_.crashed -eq "true" })).Count
            firstHasRequestingFrame = $firstMethods.Where({ $_ -like "*CaptureReports*" }).Count -ne 0
            secondHasRequestingFrame = $secondMethods.Where({ $_ -like "*CaptureReports*" }).Count -ne 0
            firstHasParkedWorker = $firstMethods.Where({ $_ -like "*ParkedWorker*" }).Count -ne 0
            secondHasParkedWorker = $secondMethods.Where({ $_ -like "*ParkedWorker*" }).Count -ne 0
            logHasSignal = $compactLog.Contains("signal 6 (SIGABRT)", [StringComparison]::Ordinal)
            logHasCrashedThread = $compactLog.Contains("(crashed) ---", [StringComparison]::Ordinal)
            logHasRequestingFrame = $compactLog.Contains("Program.CaptureReports", [StringComparison]::Ordinal)
            logHasParkedWorker = $compactLog.Contains("Program.ParkedWorker", [StringComparison]::Ordinal)
            deviceValidatedLifecycleReportCountUnchanged = $true
        }

        $validation |
            ConvertTo-Json |
            Set-Content -Encoding utf8NoBOM (Join-Path $realOnDemandDirectory "validation.json")

        $invalid = $validation.GetEnumerator().Where({
            ($_.Value -is [bool] -and !$_.Value) -or
            ($_.Key -in @("firstProtocol", "secondProtocol") -and $_.Value -ne "1.0.0") -or
            ($_.Key -in @("firstSignal", "secondSignal") -and $_.Value -ne "6") -or
            ($_.Key -in @("firstThreadCount", "secondThreadCount") -and [int]$_.Value -lt 1) -or
            ($_.Key -in @("firstCrashedThreadCount", "secondCrashedThreadCount") -and [int]$_.Value -ne 1)
        })
        if ($invalid.Count -ne 0)
        {
            throw "Retained real on-demand artifacts failed host-side validation."
        }

        $results.Add([pscustomobject]@{
            Name = "real-runtime-on-demand-artifacts"
            Kind = "artifact-validation"
            Status = "PASS"
            ExitCode = $null
            Log = [IO.Path]::GetRelativePath($ResultsDirectory, (Join-Path $realOnDemandDirectory "validation.json"))
        })
    }
    catch
    {
        $_ | Out-String | Add-Content $realOnDemandLog
        $results.Add([pscustomobject]@{
            Name = "real-runtime-on-demand-artifacts"
            Kind = "artifact-validation"
            Status = "FAIL"
            ExitCode = $null
            Log = [IO.Path]::GetRelativePath($ResultsDirectory, $realOnDemandLog)
        })
    }
}

$crashAppProject = Join-Path $testRoot "RealCrash\CrashApp\Android.Device_Emulator.InProcCrashReport.RealCrash.CrashApp.csproj"
$crashAppLog = Join-Path $realCrashDirectory "build.log"
$buildExitCode = Invoke-TestCommand "real-crash-app-build" "build" $crashAppLog {
    & $repoDotnet build -c Release $crashAppProject `
        /p:TargetOS=android /p:TargetArchitecture=x64 `
        /p:RuntimeFlavor=coreclr /p:RuntimeConfiguration=Release `
        /p:IgnoreForCI=false --nologo
}

if ($buildExitCode -eq 0)
{
    $hostDirectory = Join-Path $testRoot "RealCrash\Host"
    $hostLog = Join-Path $realCrashDirectory "test.log"
    $env:INPROC_CRASH_RESULTS_DIR = $realCrashDirectory

    Push-Location $hostDirectory
    try
    {
        Invoke-TestCommand "real-crash-host" "real-crash" $hostLog {
            & $hostDotnet test --nologo `
                --results-directory $realCrashDirectory `
                --logger "trx;LogFileName=tests.trx"
        } | Out-Null

        $trxPath = Join-Path $realCrashDirectory "tests.trx"
        if (Test-Path $trxPath)
        {
            [xml] $trx = Get-Content $trxPath
            foreach ($testResult in $trx.SelectNodes("//*[local-name()='UnitTestResult']"))
            {
                $scenario = Get-RealCrashScenarioName $testResult.testName
                $status = if ($testResult.outcome -eq "Passed") { "PASS" } else { "FAIL" }
                if ($null -eq $scenario)
                {
                    $results.Add([pscustomobject]@{
                        Name = $testResult.testName
                        Kind = "real-crash-infrastructure"
                        Status = $status
                        ExitCode = $null
                        Log = [IO.Path]::GetRelativePath($ResultsDirectory, $trxPath)
                    })
                    continue
                }

                $scenarioDirectory = Join-Path $realCrashDirectory $scenario
                New-Item -ItemType Directory -Force $scenarioDirectory | Out-Null
                $validationPath = Join-Path $scenarioDirectory "validation.json"
                @{
                    scenario = $scenario
                    testName = $testResult.testName
                    outcome = $testResult.outcome
                    duration = $testResult.duration
                } |
                    ConvertTo-Json |
                    Set-Content -Encoding utf8NoBOM $validationPath

                $results.Add([pscustomobject]@{
                    Name = $scenario
                    Kind = "real-crash-scenario"
                    Status = $status
                    ExitCode = $null
                    Log = [IO.Path]::GetRelativePath($ResultsDirectory, $validationPath)
                })
            }
        }
    }
    finally
    {
        Pop-Location
        Remove-Item Env:\INPROC_CRASH_RESULTS_DIR -ErrorAction SilentlyContinue
    }
}
else
{
    $results.Add([pscustomobject]@{
        Name = "real-crash-host"
        Kind = "real-crash"
        Status = "SKIPPED"
        ExitCode = $null
        Log = $null
    })
}

$results |
    ConvertTo-Json |
    Set-Content -Encoding utf8NoBOM (Join-Path $ResultsDirectory "results.json")

$summary = [Collections.Generic.List[string]]::new()
$summary.Add("# In-proc crash reporter test results")
$summary.Add("")
$summary.Add("Generated at $([DateTime]::UtcNow.ToString("O")).")
$summary.Add("")
$summary.Add("| Test | Suite | Status | Exit code | Log |")
$summary.Add("| --- | --- | --- | ---: | --- |")
foreach ($result in $results)
{
    $exitCode = if ($null -eq $result.ExitCode) { "" } else { $result.ExitCode }
    $logLink = if ($null -eq $result.Log) { "" } else { "[$($result.Log)]($($result.Log.Replace('\', '/')))" }
    $summary.Add("| $($result.Name) | $($result.Kind) | $($result.Status) | $exitCode | $logLink |")
}
$summary.Add("")
$summary.Add('When capture succeeds, the real on-demand directory contains both exact JSON reports and the compact log, and real-crash scenario directories contain the exact `report.json` and `console.txt` inputs evaluated by the assertions. Failed capture attempts still retain their validation records.')
$summary |
    Set-Content -Encoding utf8NoBOM (Join-Path $ResultsDirectory "README.md")

Write-Host ""
Write-Host "Durable results: $ResultsDirectory"

if ($results.Where({ $_.Status -eq "FAIL" }).Count -ne 0)
{
    exit 1
}

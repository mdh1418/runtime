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
$realCrashDirectory = Join-Path $ResultsDirectory "real-crash"
if (Test-Path $ResultsDirectory -PathType Container)
{
    if (Get-ChildItem $ResultsDirectory -Force | Select-Object -First 1)
    {
        throw "Results directory '$ResultsDirectory' is not empty. Choose a new directory."
    }
}

New-Item -ItemType Directory -Force $syntheticDirectory, $realCrashDirectory | Out-Null

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
$summary.Add("| Test | Layer | Status | Exit code | Log |")
$summary.Add("| --- | --- | --- | ---: | --- |")
foreach ($result in $results)
{
    $exitCode = if ($null -eq $result.ExitCode) { "" } else { $result.ExitCode }
    $logLink = if ($null -eq $result.Log) { "" } else { "[$($result.Log)]($($result.Log.Replace('\', '/')))" }
    $summary.Add("| $($result.Name) | $($result.Kind) | $($result.Status) | $exitCode | $logLink |")
}
$summary.Add("")
$summary.Add('When capture succeeds, real-crash scenario directories contain the exact `report.json` and `console.txt` inputs evaluated by the host assertions. Failed capture attempts still retain their `validation.json` result.')
$summary |
    Set-Content -Encoding utf8NoBOM (Join-Path $ResultsDirectory "README.md")

Write-Host ""
Write-Host "Durable results: $ResultsDirectory"

if ($results.Where({ $_.Status -eq "FAIL" }).Count -ne 0)
{
    exit 1
}

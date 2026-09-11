<#
.SYNOPSIS
    Builds NetPlugHost end to end and runs the ABI_SPEC.md §8 headless test.

.DESCRIPTION
    In dependency order (ABI_SPEC.md §1):
      1. Vst3HostNative.dll  (native, CMake + MSVC x64)
      2. NetPlugHost.dll     (managed, net9.0-windows x64)
      3. the §8 headless test, plus the VST3 test fixture it runs against

.PARAMETER Configuration
    Release (default) or Debug.

.PARAMETER SkipFixture
    Skip building the SDK's test plug-in. The §8 test then needs an explicit .vst3 path.

.PARAMETER SkipTest
    Build everything but don't run the §8 test.

.EXAMPLE
    .\build.ps1
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [switch]$SkipFixture,
    [switch]$SkipTest
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

function Assert-Tool([string]$name, [string]$hint) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        throw "'$name' not found on PATH. $hint"
    }
}

Assert-Tool 'cmake' "Install Visual Studio 2022's 'Desktop development with C++' workload, or run this from a Developer PowerShell."
Assert-Tool 'dotnet' 'Install the .NET 9 SDK.'

if (-not (Test-Path "$root\external\vst3sdk\pluginterfaces\base\funknown.cpp")) {
    throw "VST3 SDK missing. Run: git submodule update --init --recursive"
}

Write-Host "==> [1/3] Native: Vst3HostNative.dll ($Configuration)" -ForegroundColor Cyan
cmake -S "$root\native" -B "$root\native\build" -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
cmake --build "$root\native\build" --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "Native build failed." }

$nativeDll = "$root\native\build\bin\Vst3HostNative.dll"
if (-not (Test-Path $nativeDll)) { throw "Expected $nativeDll to exist after the native build." }
Write-Host "    -> $nativeDll" -ForegroundColor DarkGray

if (-not $SkipFixture) {
    Write-Host "==> [2/3] Test fixture: again-sample-accurate.vst3" -ForegroundColor Cyan
    cmake -S "$root\tests\fixture" -B "$root\tests\fixture\build" -A x64
    if ($LASTEXITCODE -ne 0) { throw "Fixture configure failed." }
    cmake --build "$root\tests\fixture\build" --config $Configuration --target again-sample-accurate
    if ($LASTEXITCODE -ne 0) { throw "Fixture build failed." }
} else {
    Write-Host "==> [2/3] Test fixture: skipped" -ForegroundColor DarkGray
}

Write-Host "==> [3/3] Managed: NetPlugHost + headless test" -ForegroundColor Cyan
dotnet build "$root\NetPlugHost.sln" -c $Configuration
if ($LASTEXITCODE -ne 0) { throw "Managed build failed." }

if ($SkipTest) {
    Write-Host "Build complete (test skipped)." -ForegroundColor Green
    return
}

Write-Host ""
Write-Host "==> Native smoke test (C ABI, no CLR)" -ForegroundColor Cyan
$fixtureVst3 = Get-ChildItem "$root\tests\fixture\build" -Recurse -Filter 'again-sample-accurate.vst3' -Directory -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if ($fixtureVst3) {
    & "$root\native\build\bin\Vst3HostSmoke.exe" $fixtureVst3
    if ($LASTEXITCODE -ne 0) { throw "Native smoke test FAILED." }
} else {
    Write-Host "    (no fixture built; skipped)" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "==> ABI_SPEC.md §8 headless test" -ForegroundColor Cyan
dotnet run --project "$root\tests\NetPlugHost.HeadlessTest\NetPlugHost.HeadlessTest.csproj" -c $Configuration
if ($LASTEXITCODE -ne 0) { throw "§8 headless test FAILED." }

Write-Host ""
Write-Host "All green." -ForegroundColor Green

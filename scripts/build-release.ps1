[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [switch]$WithoutDebugger,
    [ValidateRange(0, 1024)]
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $Configuration = if ($WithoutDebugger) { 'release-nodebug' } else { 'release' }
    $BuildDirectory = Join-Path $PSScriptRoot "..\build\$Configuration"
}
$ResolvedBuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$CoreData = Join-Path $ResolvedBuildDirectory 'meson-private\coredata.dat'
$DebuggerOption = if ($WithoutDebugger) { 'false' } else { 'true' }
$SetupArguments = @(
    'setup', $ResolvedBuildDirectory,
    '--buildtype=release',
    '-Doptimization=3',
    '-Db_lto=true',
    '-Db_ndebug=true',
    "-Ddebugger=$DebuggerOption"
)

if (Test-Path -LiteralPath $CoreData) {
    $SetupArguments += '--reconfigure'
}

& meson @SetupArguments
if ($LASTEXITCODE -ne 0) { throw "Meson setup failed with exit code $LASTEXITCODE." }

$CompileArguments = @('compile', '-C', $ResolvedBuildDirectory)
if ($Jobs -gt 0) { $CompileArguments += @('--jobs', $Jobs.ToString()) }
& meson @CompileArguments
if ($LASTEXITCODE -ne 0) { throw "Release build failed with exit code $LASTEXITCODE." }

# Build above with the selected limit; tests must not start an unlimited implicit rebuild.
$TestArguments = @('test', '-C', $ResolvedBuildDirectory, '--no-rebuild', '--print-errorlogs')
if ($Jobs -gt 0) { $TestArguments += @('--num-processes', $Jobs.ToString()) }
& meson @TestArguments
if ($LASTEXITCODE -ne 0) { throw "Release tests failed with exit code $LASTEXITCODE." }

Write-Host "ProjectFeather release build is ready at $ResolvedBuildDirectory"

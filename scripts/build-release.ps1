[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [switch]$WithoutDebugger
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

& meson compile -C $ResolvedBuildDirectory
if ($LASTEXITCODE -ne 0) { throw "Release build failed with exit code $LASTEXITCODE." }

& meson test -C $ResolvedBuildDirectory --print-errorlogs
if ($LASTEXITCODE -ne 0) { throw "Release tests failed with exit code $LASTEXITCODE." }

Write-Host "ProjectFeather release build is ready at $ResolvedBuildDirectory"

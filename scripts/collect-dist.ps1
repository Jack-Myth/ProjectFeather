[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [string]$OutputDirectory
)

# Collect existing Meson products only. Never build, install, or delete a tree.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $PSBoundParameters.ContainsKey('BuildDirectory')) {
    $BuildDirectory = Join-Path $PSScriptRoot '..\build\release'
}
if (-not $PSBoundParameters.ContainsKey('OutputDirectory')) {
    $OutputDirectory = Join-Path $PSScriptRoot '..\dist'
}

function FullPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw 'Directory arguments must not be empty.'
    }
    return [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

function IsWithin([string]$Path, [string]$Parent) {
    return $Path.Equals($Parent, [StringComparison]::OrdinalIgnoreCase) -or
        $Path.StartsWith($Parent + '\', [StringComparison]::OrdinalIgnoreCase)
}

function RejectLinks([string]$Path) {
    $Current = $Path
    while ($Current) {
        if (Test-Path -LiteralPath $Current) {
            $Item = Get-Item -LiteralPath $Current -Force
            if ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Symbolic links and junctions are not supported: $Current"
            }
        }
        $Parent = [IO.Directory]::GetParent($Current)
        if ($null -eq $Parent) { break }
        $Current = $Parent.FullName
    }
}

function Sha256([string]$Path) {
    $Stream = [IO.File]::OpenRead($Path)
    $Hasher = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($Hasher.ComputeHash($Stream)).Replace('-', '').ToLowerInvariant()
    } finally {
        $Hasher.Dispose()
        $Stream.Dispose()
    }
}

function ReadJson([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing Meson metadata: $Path. Configure and build the directory first."
    }
    $Value = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    return $Value
}

if ($env:OS -ne 'Windows_NT') {
    throw 'This collector currently supports Windows builds only.'
}

$Project = FullPath (Join-Path $PSScriptRoot '..')
$Build = FullPath $BuildDirectory
$Output = FullPath $OutputDirectory

RejectLinks $Build
RejectLinks $Output
if ((IsWithin $Project $Output) -or
    (IsWithin $Build $Output) -or (IsWithin $Output $Build) -or
    (IsWithin $Output (Join-Path $Project 'build'))) {
    throw 'Output must not be the project root, a parent of the project/build, or inside a build directory.'
}

$InfoDirectory = Join-Path $Build 'meson-info'
$Info = ReadJson (Join-Path $InfoDirectory 'meson-info.json')
if ($Info.error -or (FullPath $Info.directories.source) -ne $Project -or
    (FullPath $Info.directories.build) -ne $Build) {
    throw 'Meson metadata does not describe this project and build directory.'
}
$Machines = ReadJson (Join-Path $InfoDirectory 'intro-machines.json')
if ($Machines.host.system -ne 'windows') {
    throw 'The selected Meson build is not a Windows build.'
}

$Targets = @(ReadJson (Join-Path $InfoDirectory 'intro-targets.json'))
$Options = @{}
foreach ($Option in (ReadJson (Join-Path $InfoDirectory 'intro-buildoptions.json'))) {
    $Options[$Option.name] = $Option.value
}
foreach ($Name in @('debugger', 'network', 'render2d')) {
    if (-not $Options.ContainsKey($Name)) { throw "Missing Meson option: $Name" }
}

$Executables = @('feather', 'featherc', 'feathervm')
if ($Options['debugger']) { $Executables += 'feather-debugger' }
$Modules = @('stdio.felib', 'snapshot.felib', 'checkpoint.felib', 'time.felib')
if ($Options['network']) { $Modules += 'network.felib' }
if ($Options['render2d'] -eq 'enabled' -or
    ($Options['render2d'] -eq 'auto' -and
        @($Targets | Where-Object { $_.name -eq 'render2d.felib' }).Count -gt 0)) {
    $Modules += 'render2d.felib'
}

$Files = @()
foreach ($Name in ($Executables + $Modules)) {
    $Candidates = @($Targets | Where-Object {
        $_.name -eq $Name -and $_.installed -and -not $_.subproject
    })
    if ($Candidates.Count -ne 1) { throw "Missing or ambiguous installed product target: $Name" }

    $Target = $Candidates[0]
    $IsModule = $Modules -contains $Name
    $ExpectedType = if ($IsModule) { 'shared module' } else { 'executable' }
    $Extension = if ($IsModule) { '.dll' } else { '.exe' }
    if ($Target.type -ne $ExpectedType -or @($Target.filename).Count -ne 1) {
        throw "Unexpected product target layout: $Name"
    }

    $Source = FullPath $Target.filename[0]
    RejectLinks $Source
    if (-not (IsWithin $Source $Build) -or
        [IO.Path]::GetExtension($Source) -ne $Extension -or
        -not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Missing or invalid built product: $Name ($Source). Build it before collecting."
    }

    $Relative = [IO.Path]::GetFileName($Source)
    if ($IsModule) { $Relative = 'modules/' + $Relative }
    $Files += [pscustomobject]@{
        Source = $Source
        Path = $Relative
        Sha256 = (Sha256 $Source)
    }
}

$MarkerName = '.feather-dist.json'
if (Test-Path -LiteralPath $Output) {
    if (-not (Test-Path -LiteralPath $Output -PathType Container)) {
        throw "Output is not a directory: $Output"
    }
    $Marker = Join-Path $Output $MarkerName
    if (-not (Test-Path -LiteralPath $Marker -PathType Leaf)) {
        throw "Refusing to replace an unowned output directory: $Output"
    }
    RejectLinks $Marker
    $Previous = ReadJson $Marker
    if ($Previous.Collector -ne 'ProjectFeather/collect-dist' -or $Previous.Format -ne 1) {
        throw "Output has no recognized collector manifest: $Output"
    }
}

$Parent = [IO.Directory]::GetParent($Output).FullName
$Leaf = [IO.Path]::GetFileName($Output)
$Stage = Join-Path $Parent ('.' + $Leaf + '-stage-' + [guid]::NewGuid().ToString('N'))
$Backup = $null
try {
    New-Item -ItemType Directory -Path (Join-Path $Stage 'modules') -Force | Out-Null
    foreach ($File in $Files) {
        $Destination = Join-Path $Stage $File.Path
        Copy-Item -LiteralPath $File.Source -Destination $Destination
        if ((Sha256 $Destination) -ne $File.Sha256) {
            throw "Product changed during collection: $($File.Source). Retry after the build finishes."
        }
    }

    $Manifest = [ordered]@{
        Collector = 'ProjectFeather/collect-dist'
        Format = 1
        Files = @($Files | Select-Object Path, Sha256)
    }
    $Json = $Manifest | ConvertTo-Json -Depth 4
    [IO.File]::WriteAllText((Join-Path $Stage $MarkerName), $Json, [Text.UTF8Encoding]::new($false))

    if (Test-Path -LiteralPath $Output) {
        $Backup = Join-Path $Parent ('.' + $Leaf + '-backup-' + [guid]::NewGuid().ToString('N'))
        Move-Item -LiteralPath $Output -Destination $Backup
    }
    try {
        Move-Item -LiteralPath $Stage -Destination $Output
    } catch {
        if ($Backup -and -not (Test-Path -LiteralPath $Output)) {
            Move-Item -LiteralPath $Backup -Destination $Output
            $Backup = $null
        }
        throw
    }
} catch {
    throw "Collection failed: $($_.Exception.Message) Staging, if present: $Stage. Backup: $Backup"
}

Write-Host "Collected $($Files.Count) products to $Output (no build performed)."
if ($Backup) { Write-Host "Previous output preserved at $Backup" }

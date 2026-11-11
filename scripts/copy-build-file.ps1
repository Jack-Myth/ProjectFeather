param([string]$Source, [string]$Destination)

$ErrorActionPreference = 'Stop'
Copy-Item -LiteralPath $Source -Destination $Destination -Force

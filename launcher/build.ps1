# Rebuilds ..\build\win\LRLauncher.exe (Odin; the iv_* UI libraries are in .\shared).
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$odin = if ($env:ODIN) { $env:ODIN } else { 'odin' }
$out = Join-Path $here '..\build\win\LRLauncher.exe'
Push-Location $here
try {
    & $odin build . -collection:shared=shared -subsystem:windows -o:speed "-out:$out"
    if ($LASTEXITCODE -ne 0) { throw "odin build failed ($LASTEXITCODE)" }
    Get-Item $out | Select-Object FullName, Length, LastWriteTime
} finally { Pop-Location }

# Copyright 2026 The Umbra Authors.
# Installs the release build of the agent next to Umbra and restarts it
# (Umbra itself keeps running; the new agent reattaches to it).
#   cargo build --release --manifest-path remote\agent\Cargo.toml
#   powershell -NoProfile -ExecutionPolicy Bypass -File remote\agent\deploy.ps1
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $here 'target\release\umbra-remote.exe'
$dest = 'C:\UmbraBrowser\umbra-remote.exe'
$dist = Join-Path $here '..\..\dist\umbra-remote.exe'

Get-Process umbra-remote -ErrorAction SilentlyContinue | Where-Object Path -eq $dest | Stop-Process -Force -PassThru |
  Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
# The old exe can stay locked for a moment after its process ends (the
# antivirus looks at it too): retry for a few seconds.
for ($i = 0; ; $i++) {
  try { Copy-Item $exe $dest -Force -ErrorAction Stop; break }
  catch { if ($i -ge 20) { throw }; Start-Sleep -Milliseconds 500 }
}
New-Item -ItemType Directory -Force (Split-Path $dist) | Out-Null
Copy-Item $exe $dist -Force
# The agent serves the phone app at http://<PC>:7777/UmbraRemote.apk.
$apk = Join-Path $here '..\..\dist\UmbraRemote.apk'
if (Test-Path $apk) { Copy-Item $apk 'C:\UmbraBrowser\UmbraRemote.apk' -Force }
Start-Process $dest
Start-Sleep -Seconds 5
Get-Content (Join-Path $env:LOCALAPPDATA 'UmbraRemote\agent.log') -Tail 4

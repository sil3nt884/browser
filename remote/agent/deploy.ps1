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

Get-Process umbra-remote -ErrorAction SilentlyContinue | Where-Object Path -eq $dest | Stop-Process -Force
Start-Sleep -Milliseconds 800
Copy-Item $exe $dest -Force
Copy-Item $exe $dist -Force
# The agent serves the phone app at http://<PC>:7777/UmbraRemote.apk.
$apk = Join-Path $here '..\..\dist\UmbraRemote.apk'
if (Test-Path $apk) { Copy-Item $apk 'C:\UmbraBrowser\UmbraRemote.apk' -Force }
Start-Process $dest
Start-Sleep -Seconds 5
Get-Content (Join-Path $env:LOCALAPPDATA 'UmbraRemote\agent.log') -Tail 4

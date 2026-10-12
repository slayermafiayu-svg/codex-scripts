# Installs PuppetPin.ofx.bundle into the standard OFX plug-in folder, which VEGAS Pro scans at startup.
# Run from an administrator PowerShell:
#   powershell -ExecutionPolicy Bypass -File install.ps1
#   powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall
#   powershell -ExecutionPolicy Bypass -File install.ps1 -Destination "C:\Program Files\VEGAS\VEGAS Pro 22.0\OFX Video Plug-Ins"
[CmdletBinding()]
param(
    [string]$Destination,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$bundleName = 'PuppetPin.ofx.bundle'

if (-not $Destination) {
    $common = $env:CommonProgramW6432
    if (-not $common) { $common = $env:CommonProgramFiles }
    $Destination = Join-Path $common 'OFX\Plugins'
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Please run this script from an administrator PowerShell (the OFX folder is under Program Files).'
}

$target = Join-Path $Destination $bundleName

if ($Uninstall) {
    if (Test-Path $target) {
        Remove-Item $target -Recurse -Force
        Write-Host "Removed $target"
    } else {
        Write-Host "Nothing to remove at $target"
    }
    return
}

# The bundle sits next to this script (downloaded package) or in build\ (built from source).
$source = @((Join-Path $PSScriptRoot $bundleName), (Join-Path $PSScriptRoot "build\$bundleName")) |
    Where-Object { Test-Path (Join-Path $_ 'Contents\Win64\PuppetPin.ofx') } |
    Select-Object -First 1
if (-not $source) {
    throw "PuppetPin.ofx.bundle not found next to this script or in build\ - run build.bat first."
}

New-Item -ItemType Directory -Force -Path $Destination | Out-Null
if (Test-Path $target) { Remove-Item $target -Recurse -Force }
Copy-Item $source $target -Recurse -Force

Write-Host "Installed to $target"
Get-ChildItem $target -Recurse -File | ForEach-Object { '  ' + $_.FullName.Substring($target.Length + 1) }
Write-Host ''
Write-Host "Restart VEGAS Pro, then look for 'Puppet Pin' in Video FX (group: Distort)."

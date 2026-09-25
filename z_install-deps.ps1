#!/usr/bin/env pwsh
# Installs swapp's build dependencies. Run once per machine, elevated; the
# build itself (z_run.ps1) needs no elevation.

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Test-Elevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Elevated)) {
    throw "This script installs system packages. Re-run it from an elevated PowerShell."
}

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw "winget not found. Install CMake, Ninja and the VC++ build tools manually."
}

# The MSVC toolchain, CMake and Ninja are everything CMakeLists.txt needs;
# the Windows SDK headers the tray and DDC/CI code use come with the build
# tools workload.
$packages = @(
    @{ Id = 'Kitware.CMake';  Check = 'cmake' },
    @{ Id = 'Ninja-build.Ninja'; Check = 'ninja' }
)

foreach ($package in $packages) {
    if (Get-Command $package.Check -ErrorAction SilentlyContinue) {
        Write-Host "$($package.Check) already installed"
        continue
    }
    Write-Host "Installing $($package.Id)..."
    winget install --id $package.Id --exact --silent --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) { throw "Failed to install $($package.Id)." }
}

# cl.exe is not on PATH outside a Developer PowerShell, so probe for the
# installation itself rather than the command.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$hasMsvc = $false
if (Test-Path $vswhere) {
    $hasMsvc = [bool](& $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
}

if (-not $hasMsvc) {
    Write-Host "Installing the VC++ build tools (this one is large)..."
    winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --silent `
        --accept-package-agreements --accept-source-agreements `
        --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    if ($LASTEXITCODE -ne 0) { throw "Failed to install the VC++ build tools." }
} else {
    Write-Host "VC++ build tools already installed"
}

Write-Host "Build dependencies installed. Build and start the app with .\z_run.ps1"

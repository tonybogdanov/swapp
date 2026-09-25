#!/usr/bin/env pwsh
# Kills any running swapp, rebuilds it, and starts the new binary.
# No elevation: system packages are z_install-deps.ps1's job, the build needs none.

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [string]$BuildDir = 'build-windows'
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path)

# The old binary has to go before linking: the linker writes the executable
# in place, and Windows locks an image that a running process has mapped.
$running = Get-Process swapp -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "Stopping the running swapp"
    $running | Stop-Process -Force
    $running | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsPath) {
            $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path $vcvars) {
                Write-Host "Importing MSVC environment from $vcvars"
                cmd /c "`"$vcvars`" && set" | ForEach-Object {
                    if ($_ -match '^([^=]+)=(.*)$') {
                        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
                    }
                }
            }
        }
    }
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw "cl.exe not found. Run this from a Developer PowerShell for VS, or install the VC++ build tools."
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake not found on PATH."
}

cmake -S . -B $BuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

cmake --build $BuildDir --config $Config
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Write-Host "Build succeeded: $BuildDir\swapp.exe"

Start-Process -FilePath (Join-Path $PWD "$BuildDir\swapp.exe")
Write-Host "Started $BuildDir\swapp.exe"

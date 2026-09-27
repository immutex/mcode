<#
.SYNOPSIS
    Bootstrap a Windows development environment.

.DESCRIPTION
    Activates the MSVC developer environment, checks Conan, builds the private
    Luau package, installs dependencies, and configures CMake.

.PARAMETER Configuration
    Release (default) or Debug.

.PARAMETER NoConfigure
    Stop after installing dependencies.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [switch]$NoConfigure
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot

function Write-Step($message) { Write-Host "`n==> $message" -ForegroundColor Cyan }
function Write-Ok($message) { Write-Host "    $message" -ForegroundColor Green }
function Write-Warn($message) { Write-Host "    $message" -ForegroundColor Yellow }

try {
    Write-Step 'Locating Visual Studio'

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found. Install Visual Studio 2022+ with the 'Desktop development with C++' workload."
    }

    $vsPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath

    if (-not $vsPath) {
        throw 'No Visual Studio installation with the MSVC C++ toolset was found.'
    }
    Write-Ok "Visual Studio: $vsPath"

    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) {
        throw "vcvars64.bat not found at $vcvars"
    }

    Write-Step 'Activating the MSVC developer environment'
    $envDump = & cmd /c "`"$vcvars`" >nul 2>&1 && set"
    foreach ($line in $envDump) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue
        }
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'cl.exe is still not on PATH after activating vcvars64.'
    }
    # cl.exe writes its version banner to stderr. Under
    # $ErrorActionPreference='Stop' PowerShell turns any native stderr output
    # into a terminating NativeCommandError, so the banner is captured through
    # cmd, which does not have that behaviour.
    $clVersion = cmd /c "cl.exe 2>&1"
    Write-Ok "compiler: $($clVersion | Select-Object -First 1)"

    # A MinGW toolchain earlier on PATH hijacks the link step and produces
    # confusing "/INCREMENTAL: No such file or directory" errors.
    $linkExe = (Get-Command link.exe -ErrorAction SilentlyContinue).Source
    if ($linkExe -and $linkExe -notmatch 'MSVC') {
        Write-Warn "link.exe resolves to $linkExe, which is not MSVC's linker."
    }

    Write-Step 'Checking Conan'
    if (-not (Get-Command conan -ErrorAction SilentlyContinue)) {
        throw 'Conan is not installed. Run: python -m pip install "conan==2.32.0"'
    }
    Write-Ok "$(& conan --version)"

    $profile = Join-Path $repoRoot 'conan/profiles/windows-msvc'

    # Luau is not on Conan Center -- upstream publishes no version tags, so the
    # pin is a commit and the recipe is ours. Conan caches the result, so this is
    # a no-op after the first run.
    Write-Step 'Building the Luau package'
    & conan create conan/recipes/luau --profile $profile --build=missing `
        --version 0.0.0-mcode.c0e346ed

    if ($LASTEXITCODE -ne 0) { throw 'conan create failed for the Luau package' }
    Write-Ok 'Luau package ready'

    Write-Step 'Installing dependencies (builds Boost on first run)'

    $runtimeType = if ($Configuration -eq 'Debug') { 'Debug' } else { 'Release' }

    & conan install . --profile $profile --build=missing `
        -s "build_type=$Configuration" -s "compiler.runtime_type=$runtimeType"

    if ($LASTEXITCODE -ne 0) {
        throw 'conan install failed. See the output above for the failing dependency.'
    }
    Write-Ok 'dependencies installed'

    if ($NoConfigure) {
        Write-Host "`nBootstrap complete (configure skipped)." -ForegroundColor Green
        return
    }

    Write-Step 'Configuring CMake'
    $preset = if ($Configuration -eq 'Debug') { 'windows-msvc-debug' } else { 'windows-msvc' }
    & cmake --preset $preset

    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }

    Write-Host @"

Bootstrap complete.

Next:
    cmake --build build/$Configuration
    ctest --preset $preset
    ./build/$Configuration/src/mcode.exe
"@ -ForegroundColor Green
}
finally {
    Pop-Location
}

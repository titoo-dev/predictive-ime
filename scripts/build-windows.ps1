<#
.SYNOPSIS
  Construit predictord.exe sous Windows (MSVC + vcpkg).

.DESCRIPTION
  Le daemon est la seule partie portable du dépôt : l'engine est un addon
  fcitx5 et qmlpanel/preferences sont Qt+Wayland — CMake les désactive
  automatiquement sur WIN32. Le frontal Windows est un text service TSF, hors
  de cette build (cf docs/specs/2026-09-11-windows-tsf-port-design.md).

  Bootstrappe vcpkg au premier passage, puis configure et compile.

.PARAMETER Arch
  x64 (défaut) ou x86. Le x86 servira au text service TSF chargé dans les
  applications 32 bits ; le daemon lui-même n'en a pas besoin.

.EXAMPLE
  .\scripts\build-windows.ps1
  .\scripts\build-windows.ps1 -Arch x86 -Clean
#>
[CmdletBinding()]
param(
  [ValidateSet('x64', 'x86')] [string] $Arch = 'x64',
  [ValidateSet('Release', 'Debug')] [string] $Config = 'Release',
  [string] $VcpkgRoot = $env:VCPKG_ROOT,
  [switch] $Clean
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Require-Tool([string] $name, [string] $hint) {
  if (-not (Get-Command $name -EA SilentlyContinue)) {
    throw "$name introuvable sur le PATH. $hint"
  }
}

# Les Build Tools embarquent leur propre CMake et Ninja : inutile d'exiger une
# installation séparée quand « Desktop development with C++ » est déjà là.
if (-not (Get-Command cmake -EA SilentlyContinue)) {
  $bundled = Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio\2022' `
    -Recurse -Filter cmake.exe -EA SilentlyContinue | Select-Object -First 1
  if ($bundled) {
    $env:PATH = "$($bundled.Directory.FullName);$env:PATH"
    Write-Host "cmake      : $($bundled.FullName) (fourni par les Build Tools)"
  }
}

Require-Tool cmake 'winget install Kitware.CMake (puis rouvrir le terminal)'
Require-Tool git   'winget install Git.Git'

# --- vcpkg : nlohmann-json + curl -------------------------------------------
if (-not $VcpkgRoot) { $VcpkgRoot = Join-Path $env:LOCALAPPDATA 'vcpkg' }
if (-not (Test-Path (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
  Write-Host "==> bootstrap vcpkg dans $VcpkgRoot" -ForegroundColor Cyan
  if (-not (Test-Path $VcpkgRoot)) {
    git clone --depth 1 https://github.com/microsoft/vcpkg $VcpkgRoot
  }
  & (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics
  if ($LASTEXITCODE -ne 0) { throw 'bootstrap-vcpkg a échoué' }
}

$triplet = "$Arch-windows"
Write-Host "==> dépendances ($triplet)" -ForegroundColor Cyan
& (Join-Path $VcpkgRoot 'vcpkg.exe') install "nlohmann-json:$triplet" "curl:$triplet"
if ($LASTEXITCODE -ne 0) { throw 'vcpkg install a échoué' }

# --- configure + build -------------------------------------------------------
$buildDir = Join-Path $repo "build-win-$Arch"
if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }

# -A attend Win32 pour le 32 bits, pas x86.
$genArch = if ($Arch -eq 'x86') { 'Win32' } else { 'x64' }
$toolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'

Write-Host "==> configure ($genArch, $Config)" -ForegroundColor Cyan
cmake -S $repo -B $buildDir -A $genArch `
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" `
  -DVCPKG_TARGET_TRIPLET="$triplet" `
  -DCMAKE_INSTALL_PREFIX="$env:LOCALAPPDATA\ime-predictord"
if ($LASTEXITCODE -ne 0) { throw 'cmake configure a échoué' }

Write-Host "==> build" -ForegroundColor Cyan
cmake --build $buildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw 'cmake build a échoué' }

$exe = Join-Path $buildDir "daemon\$Config\predictord.exe"
if (-not (Test-Path $exe)) { $exe = Join-Path $buildDir "daemon\predictord.exe" }
Write-Host "`nOK : $exe" -ForegroundColor Green

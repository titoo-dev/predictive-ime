<#
.SYNOPSIS
  Produit l'installeur Windows (dist\predictive-ime-<version>-x64.exe).

.DESCRIPTION
  Enchaîne, de façon reproductible :
    1. build      — scripts\build-windows.ps1 (predictord.exe, predict-tsf.dll,
                    predict-admin.exe) sauf -SkipBuild ;
    2. tests      — ctest sur build-win-x64 (sauf -SkipTests) : on ne
                    packagera jamais un binaire dont les tests échouent ;
    3. modèle     — dépose le modèle n-gramme dans build-win-x64\model, d'où
                    l'installeur le LIVRE ({app}\model). Source : -ModelSource,
                    sinon le modèle déjà installé (%LOCALAPPDATA%\ime-predictord\model),
                    sinon la release GitHub (curl + zstd) ;
    4. installeur — ISCC (Inno Setup 6, cherché sur le PATH puis aux
                    emplacements d'installation habituels) ;
    5. empreinte  — <installeur>.sha256 à publier avec la release.

.EXAMPLE
  .\scripts\package-windows.ps1
  .\scripts\package-windows.ps1 -SkipBuild -SkipTests      # re-packager seulement
#>
[CmdletBinding()]
param(
  [switch] $SkipBuild,
  [switch] $SkipTests,
  [string] $ModelSource = '',
  [string] $ModelTag = 'model-v1',
  [string] $Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repo 'build-win-x64'
$stage = Join-Path $buildDir 'model'

function Step([string] $msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

# ---------------------------------------------------------------------- build
if (-not $SkipBuild) {
  Step 'build'
  & (Join-Path $PSScriptRoot 'build-windows.ps1') -Config $Config
  if ($LASTEXITCODE -ne 0) { throw 'build échoué' }
}
foreach ($rel in @("daemon\$Config\predictord.exe", "win\tsf\$Config\predict-tsf.dll",
                   "win\admin\$Config\predict-admin.exe")) {
  if (-not (Test-Path (Join-Path $buildDir $rel))) { throw "binaire manquant : $rel — lancez sans -SkipBuild" }
}

# ---------------------------------------------------------------------- tests
if (-not $SkipTests) {
  Step 'tests'
  ctest --test-dir $buildDir -C $Config --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw 'tests en échec : pas de packaging' }
}

# --------------------------------------------------------------------- modèle
Step 'modèle'
$required = @('words.tsv', 'bigrams.tsv', 'bigrams.bo.tsv', 'trigrams.tsv', 'trigrams.bo.tsv',
              'pcont.tsv', 'emoji.tsv')
function Test-Model([string] $dir) {
  if (-not $dir -or -not (Test-Path $dir)) { return $false }
  foreach ($f in $required) { if (-not (Test-Path (Join-Path $dir $f))) { return $false } }
  return $true
}

$src = $ModelSource
if (-not (Test-Model $src)) { $src = Join-Path $env:LOCALAPPDATA 'ime-predictord\model' }
if (-not (Test-Model $src)) {
  # Dernier recours : la release (même archive que setup-windows.ps1).
  if (-not (Get-Command zstd -EA SilentlyContinue)) {
    throw 'aucun modèle local et zstd introuvable — winget install Meta.Zstandard, ou -ModelSource <dossier>'
  }
  $tmp = Join-Path $buildDir 'model-download'
  New-Item -ItemType Directory -Force $tmp | Out-Null
  $zst = Join-Path $tmp "ime-model-$ModelTag.tar.zst"
  if (-not (Test-Path $zst)) {
    curl.exe -fL --progress-bar -o $zst "https://github.com/titoo-dev/predictive-ime/releases/download/$ModelTag/ime-model-$ModelTag.tar.zst"
    if ($LASTEXITCODE -ne 0) { throw 'téléchargement du modèle échoué' }
  }
  $tar = [IO.Path]::ChangeExtension($zst, $null).TrimEnd('.')
  zstd -d -f $zst -o $tar; if ($LASTEXITCODE -ne 0) { throw 'zstd -d a échoué' }
  tar -xf $tar -C $tmp;      if ($LASTEXITCODE -ne 0) { throw 'tar -xf a échoué' }
  Remove-Item $tar -Force
  $found = Get-ChildItem $tmp -Recurse -Filter words.tsv | Select-Object -First 1
  if (-not $found) { throw 'words.tsv absent de l''archive' }
  $src = $found.Directory.FullName
}
if (-not (Test-Model $src)) { throw "modèle incomplet dans $src" }

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
foreach ($f in $required + @('NOTICE')) {
  $p = Join-Path $src $f
  if (Test-Path $p) { Copy-Item $p $stage }
}
$mb = [math]::Round(((Get-ChildItem $stage | Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Host "modèle   : $src -> $stage ($mb Mo)"

# ------------------------------------------------------- scripts embarqués
# L'installeur exécute setup-windows.ps1 avec Windows PowerShell 5.1, qui lit
# un fichier SANS BOM en ANSI : un tiret cadratin devient un guillemet typo et
# le script ne se parse plus (l'installation « réussit » sans daemon). On
# refuse donc de packager un script sans BOM, et on vérifie qu'il se parse.
Step 'scripts (BOM + syntaxe PowerShell 5.1)'
foreach ($name in @('setup-windows.ps1', 'PredictLayout.ps1', 'probe-daemon.ps1', 'try-daemon.ps1')) {
  $f = Join-Path $PSScriptRoot $name
  $b = [IO.File]::ReadAllBytes($f)
  if (-not ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)) {
    throw "$name : pas de BOM UTF-8 — enregistrez-le en « UTF-8 with BOM »"
  }
  $errs = $null; $tokens = $null
  [void][System.Management.Automation.Language.Parser]::ParseFile($f, [ref]$tokens, [ref]$errs)
  if ($errs.Count) { throw "$name : erreur de syntaxe — $($errs[0].Message)" }
}

# ----------------------------------------------------------------- installeur
Step 'installeur'
$iscc = (Get-Command ISCC.exe -EA SilentlyContinue).Source
if (-not $iscc) {
  $iscc = @(
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
  ) | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $iscc) { throw 'ISCC.exe introuvable — winget install JRSoftware.InnoSetup' }

& $iscc /Q (Join-Path $repo 'packaging\windows\predictive-ime.iss')
if ($LASTEXITCODE -ne 0) { throw 'ISCC a échoué' }

$out = Get-ChildItem (Join-Path $repo 'dist') -Filter 'predictive-ime-*-x64.exe' |
  Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $out) { throw 'installeur introuvable dans dist\' }

# ------------------------------------------------------------------ empreinte
$hash = (Get-FileHash $out.FullName -Algorithm SHA256).Hash.ToLower()
"$hash  $($out.Name)" | Set-Content -LiteralPath "$($out.FullName).sha256" -Encoding ascii
Write-Host ("`nOK : {0} ({1} Mo)`nSHA256 : {2}" -f $out.FullName,
  [math]::Round($out.Length / 1MB, 1), $hash) -ForegroundColor Green
Write-Host "Installer   :  $($out.Name)            (assistant)"
Write-Host "Silencieux  :  $($out.Name) /VERYSILENT /NORESTART"

<#
.SYNOPSIS
  Non-régression des règles de disposition du setup (scripts/PredictLayout.ps1).

.DESCRIPTION
  Données tirées d'une machine réelle : français en AZERTY, anglais configuré
  en AZERTY, et la redirection 0000040c → 00000409 laissée par Windows qui
  faisait taper Predict en QWERTY. Aucun accès au système : fonctions pures.
  Sans framework (Pester 3 et 5 n'ont pas la même syntaxe) ; code de sortie
  non nul au premier échec. Lancé par ctest (win-setup-rules).
#>
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'PredictLayout.ps1')

$script:run = 0
$script:failed = 0
function Check([string] $what, $got, $want) {
  $script:run++
  if (($got -join ',') -ne ($want -join ',')) {
    $script:failed++
    Write-Host "  ECHEC  $what : obtenu [$($got -join ',')], attendu [$($want -join ',')]" -ForegroundColor Red
  }
}

$tipFr = Get-PredictTip '040C'
$tipEn = Get-PredictTip '0409'

Write-Host 'Predict propose seulement la ou il tapera comme vous'
Check 'FR en AZERTY'                (Test-PredictLayoutFits '040C' @('040C:0000040C')) $true
Check 'FR en AZERTY standard'       (Test-PredictLayoutFits '040C' @($tipFr, '040C:0001040C')) $true
Check 'EN en AZERTY (le cas reel)'  (Test-PredictLayoutFits '0409' @('0409:0000040C', '0409:0001040C')) $false
Check 'EN en US'                    (Test-PredictLayoutFits '0409' @('0409:00000409')) $true
Check 'EN en US-International'      (Test-PredictLayoutFits '0409' @('0409:00020409')) $true
Check 'FR tape en US'               (Test-PredictLayoutFits '040C' @('040C:00000409')) $true
Check 'langue sans disposition connue' (Test-PredictLayoutFits '040C' @($tipFr)) $true

Write-Host 'Dispositions de l''utilisateur (text services ignores)'
Check 'ordre conserve' (Get-PredictUserLayouts @($tipFr, '040C:0001040C', '040C:0000040C')) @('0001040C', '0000040C')

Write-Host 'Redirections de Substitutes a retirer'
$languages = @(
  [pscustomobject]@{ LanguageTag = 'fr-FR'; InputMethodTips = @($tipFr, '040C:0000040C') },
  [pscustomobject]@{ LanguageTag = 'en-US'; InputMethodTips = @('0409:0000040C') }
)
# L'état constaté quand Predict tapait en QWERTY.
$subs = @{ 'd0010409' = '0000040c'; '0000040c' = '00000409'; 'd001040c' = '0000040c'; '00000409' = '0000040c' }
Check 'retire 0000040c -> US, et SEULEMENT elle' (Get-PredictStaleSubstitutes $languages $subs) @('0000040c')
# 00000409 -> 0000040c est l'anglais AZERTY de l'utilisateur, sans Predict EN :
# ne jamais y toucher.
Check 'rien quand la base pointe vers une disposition employee' `
  (Get-PredictStaleSubstitutes $languages @{ '0000040c' = '0000040C'; '00000409' = '0000040c' }) @()
Check 'rien sans redirection' (Get-PredictStaleSubstitutes $languages @{ 'd001040c' = '0000040c' }) @()
$noPredict = @([pscustomobject]@{ LanguageTag = 'fr-FR'; InputMethodTips = @('040C:0000040C') })
Check 'rien quand Predict n''est pas dans la langue' (Get-PredictStaleSubstitutes $noPredict $subs) @()
$withEn = $languages + @([pscustomobject]@{ LanguageTag = 'en-GB'; InputMethodTips = @($tipEn, '0809:00000809') })
Check 'casse des noms de valeurs indifferente' `
  (Get-PredictStaleSubstitutes $languages @{ '0000040C' = '00000409' }) @('0000040C')
Check 'une langue EN avec Predict et sa propre base' `
  (Get-PredictStaleSubstitutes $withEn @{ '00000409' = '0000040c'; '0000040c' = '0000040c' }) @('00000409')

Write-Host "$script:run verifications, $script:failed echec(s)"
if ($script:failed) { exit 1 }
exit 0

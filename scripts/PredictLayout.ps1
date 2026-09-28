# Règles de disposition clavier de Predict — fonctions PURES, partagées par
# setup-windows.ps1 et scripts/tests/Test-PredictLayout.ps1.
#
# Contexte (cf win/tsf/KeyboardLayout.h) : Predict tape avec la disposition
# déclarée à son inscription, qui doit être de la MÊME langue que le profil ;
# sinon Windows lui donne US. Et il la charge à travers
# HKCU\Keyboard Layout\Substitutes, où une redirection laissée par Windows
# (0000040c → 00000409) la ramenait en QWERTY.

$PREDICT_CLSID   = '{5F0A1C7E-3B84-4D2E-9C31-7A6E2D4B8F10}'
$PREDICT_PROFILE = '{5F0A1C7E-3B84-4D2E-9C31-7A6E2D4B8F11}'

# LANGID du profil Predict pour une langue de la liste (fr-* → FR, en-* → EN).
function Get-PredictLangId([string] $LanguageTag) {
  switch -Wildcard ($LanguageTag) {
    'fr*' { return '040C' }
    'en*' { return '0409' }
    default { return $null }
  }
}

function Get-PredictTip([string] $LangId) { "${LangId}:$PREDICT_CLSID$PREDICT_PROFILE" }

# Dispositions (KLID) d'une langue, dans l'ordre des Paramètres, à partir de
# ses InputMethodTips (« LLLL:KKKKKKKK » ; les text services sont ignorés).
function Get-PredictUserLayouts($Tips) {
  @($Tips | Where-Object { $_ -match '^[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}$' } |
    ForEach-Object { $_.Split(':')[1] })
}

# Predict tapera-t-il avec la disposition de l'utilisateur dans cette langue ?
# Oui si sa première disposition appartient à la langue (déclarable) ou est
# US (la valeur par défaut) ; non pour l'anglais en AZERTY.
function Test-PredictLayoutFits([string] $LangId, $Tips) {
  $first = Get-PredictUserLayouts $Tips | Select-Object -First 1
  if (-not $first) { return $true } # rien de connu : on ne refuse pas
  return ($first.Substring(4) -ieq $LangId) -or ($first -ieq '00000409')
}

# Redirections de Substitutes à retirer : celle de la disposition de base
# d'une langue où Predict est proposé (0000LLLL), quand elle pointe vers une
# disposition que l'utilisateur n'a PAS dans cette langue. Jamais une qu'il
# emploie, jamais une langue sans Predict.
#   $Languages   : objets { LanguageTag ; InputMethodTips }
#   $Substitutes : hashtable nom de valeur → donnée
function Get-PredictStaleSubstitutes($Languages, [hashtable] $Substitutes) {
  $stale = @()
  foreach ($lang in $Languages) {
    $langId = Get-PredictLangId $lang.LanguageTag
    if (-not $langId) { continue }
    if ($lang.InputMethodTips -notcontains (Get-PredictTip $langId)) { continue }
    $base = "0000$langId".ToLower()
    $key = @($Substitutes.Keys | Where-Object { $_ -ieq $base }) | Select-Object -First 1
    if (-not $key) { continue }
    $mine = @(Get-PredictUserLayouts $lang.InputMethodTips | ForEach-Object { $_.ToLower() })
    if ($mine -notcontains ([string] $Substitutes[$key]).ToLower()) { $stale += $key }
  }
  return , $stale
}

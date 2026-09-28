<#
.SYNOPSIS
  Installe le modèle et l'autodémarrage du daemon predictord sous Windows.

.DESCRIPTION
  Trois étapes, toutes ré-exécutables sans dommage :
    1. modèle    — télécharge la release .tar.zst et l'extrait
    2. config    — dépose un config.json par défaut s'il n'y en a pas
    3. autostart — tâche planifiée « à l'ouverture de session »

  Pourquoi une tâche planifiée et pas un service Windows : le daemon DOIT
  tourner dans la session de l'utilisateur — il lit %APPDATA%\ime-predictord
  et écrit les journaux de mots appris dans %LOCALAPPDATA%. Un service tourne
  sous un autre compte et ne verrait ni l'un ni l'autre.

  Aucune élévation nécessaire : tout est per-user.

.EXAMPLE
  .\scripts\setup-windows.ps1
  .\scripts\setup-windows.ps1 -SkipModel      # modèle déjà en place
  .\scripts\setup-windows.ps1 -Uninstall
#>
[CmdletBinding()]
param(
  [string] $ModelTag = 'model-v1',
  [switch] $SkipModel,
  [switch] $SkipAutostart,
  [switch] $SkipIme,
  # Avec -SkipIme (installeur : la DLL est déjà inscrite par Inno Setup),
  # ajoute quand même « Predict » aux méthodes de saisie de l'utilisateur.
  [switch] $AddInputMethod,
  [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'

# GUID du service et du profil ($PREDICT_CLSID, $PREDICT_PROFILE — ils
# doivent coller à win/tsf/Guids.h) et règles de disposition, partagés avec
# scripts/tests/Test-PredictLayout.ps1.
. (Join-Path $PSScriptRoot 'PredictLayout.ps1')

# Enregistrer le profil ne suffit PAS à le faire apparaître : il faut encore
# l'ajouter à la liste de saisie de l'utilisateur. Le format d'un « tip » est
# LANGID:{CLSID}{PROFIL} — sans ça, « Predict » reste introuvable dans les
# Paramètres et l'utilisateur croit que l'installation a échoué.
#
# Disposition : on ne le propose QUE là où il tapera comme l'utilisateur
# (Test-PredictLayoutFits) — l'anglais configuré en AZERTY donnerait un
# Predict EN en QWERTY. Predict FR prédit aussi l'anglais.
function Add-PredictToLanguageList {
  try {
    $list = Get-WinUserLanguageList
    $changed = $false
    foreach ($lang in $list) {
      $langid = Get-PredictLangId $lang.LanguageTag
      if (-not $langid) { continue }
      $tip = Get-PredictTip $langid
      if (-not (Test-PredictLayoutFits $langid $lang.InputMethodTips)) {
        if ($lang.InputMethodTips -contains $tip) {
          [void] $lang.InputMethodTips.Remove($tip)
          $changed = $true
        }
        $first = Get-PredictUserLayouts $lang.InputMethodTips | Select-Object -First 1
        Write-Host "IME      : Predict non propose en $($lang.LanguageTag) — votre disposition $first n'est pas de cette langue, Windows l'y forcerait en QWERTY. Utilisez Predict (FR), qui predit aussi l'anglais." -ForegroundColor Yellow
        continue
      }
      if ($lang.InputMethodTips -notcontains $tip) {
        $lang.InputMethodTips.Add($tip)
        $changed = $true
      }
    }
    if ($changed) {
      Set-WinUserLanguageList $list -Force
      Write-Host 'IME      : methodes de saisie mises a jour (Win+Espace)'
    } else {
      Write-Host 'IME      : deja present dans vos methodes de saisie'
    }
  } catch {
    Write-Host "IME      : ajout a la liste de saisie impossible ($($_.Exception.Message))" -ForegroundColor Yellow
    Write-Host '           Ajoutez-le a la main : Parametres > Heure et langue > Saisie > Clavier.'
  }
}

# Windows charge la disposition déclarée pour Predict (0000LLLL) À TRAVERS
# HKCU\Keyboard Layout\Substitutes. Tant que Predict n'en déclarait pas, il y
# a écrit 0000040c → 00000409 (US), et recalcule ensuite cette entrée à
# travers elle-même à chaque session : elle ne disparaît jamais seule, et
# Predict tapait en QWERTY malgré l'AZERTY déclaré. Effet à la prochaine
# ouverture de session.
function Clear-PredictStaleSubstitute {
  $key = 'HKCU:\Keyboard Layout\Substitutes'
  $props = Get-ItemProperty $key -EA SilentlyContinue
  if (-not $props) { return }
  $subs = @{}
  foreach ($p in $props.PSObject.Properties) {
    if ($p.Name -notlike 'PS*') { $subs[$p.Name] = [string] $p.Value }
  }
  foreach ($name in (Get-PredictStaleSubstitutes (Get-WinUserLanguageList) $subs)) {
    Remove-ItemProperty $key -Name $name
    Write-Host "clavier  : redirection $name -> $($subs[$name]) retiree — Predict tapera avec votre disposition a la prochaine ouverture de session" -ForegroundColor Yellow
  }
}

# L'inverse, à la désinstallation : une entrée qui pointe vers un profil
# désinscrit reste sinon listée (« clavier inconnu ») dans Win+Espace.
function Remove-PredictFromLanguageList {
  try {
    $list = Get-WinUserLanguageList
    $changed = $false
    foreach ($lang in $list) {
      $mine = @($lang.InputMethodTips | Where-Object { $_ -like "*$PREDICT_CLSID*" })
      foreach ($tip in $mine) { [void] $lang.InputMethodTips.Remove($tip); $changed = $true }
    }
    if ($changed) {
      Set-WinUserLanguageList $list -Force
      Write-Host 'IME      : « Predict » retire de vos methodes de saisie'
    }
  } catch {
    Write-Host "IME      : retrait de la liste de saisie impossible ($($_.Exception.Message))" -ForegroundColor Yellow
  }
}

$repo     = Split-Path -Parent $PSScriptRoot
$dataDir  = Join-Path $env:LOCALAPPDATA 'ime-predictord'   # modèle, socket, appris
$cfgDir   = Join-Path $env:APPDATA      'ime-predictord'   # réglages éditables
$modelDir = Join-Path $dataDir 'model'
$binDir   = Join-Path $dataDir 'bin'
$taskName = 'ime-predictord'

# ---------------------------------------------------------------- désinstall --
if ($Uninstall) {
  Remove-PredictFromLanguageList
  foreach ($tsf in (Get-ChildItem $binDir -Filter 'predict-tsf*.dll' -EA SilentlyContinue).FullName) {
    # Désenregistrer AVANT de toucher au reste : une DLL désinscrite mais
    # présente est inerte, l'inverse laisse un profil clavier fantôme que
    # l'utilisateur devrait retirer à la main.
    $admin = ([Security.Principal.WindowsPrincipal] `
      [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($admin) {
      Start-Process regsvr32.exe -ArgumentList '/s', '/u', "`"$tsf`"" -Wait
      Write-Host 'IME desenregistre'
    } else {
      Write-Host "A faire EN ADMINISTRATEUR :  regsvr32 /u `"$tsf`"" -ForegroundColor Yellow
    }
  }
  if (Get-ScheduledTask -TaskName $taskName -EA SilentlyContinue) {
    Stop-ScheduledTask   -TaskName $taskName -EA SilentlyContinue
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    Write-Host "tâche $taskName supprimée"
  }
  Get-Process predictord -EA SilentlyContinue | Stop-Process -Force
  Write-Host "Modèle et réglages CONSERVÉS :`n  $dataDir`n  $cfgDir"
  return
}

New-Item -ItemType Directory -Force $dataDir, $cfgDir, $modelDir, $binDir | Out-Null

# -------------------------------------------------------------------- binaire --
$built = @(
  "$repo\build-win-x64\daemon\Release\predictord.exe",
  "$repo\build-win-x64\daemon\predictord.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if ($built) {
  # Windows verrouille un .exe en cours d'execution : impossible de l'ecraser
  # tant que le daemon tourne. On l'arrete d'abord, il sera relance a la fin.
  # On mémorise qu'il tournait : un installeur qui arrête un service DOIT le
  # relancer. Sans ça on repart d'une session où plus rien ne prédit, et le
  # symptôme ressemble à s'y méprendre à un bug de l'IME.
  if (Get-Process predictord -EA SilentlyContinue) {
    Write-Host 'daemon   : arret pour remplacer le binaire'
    $script:daemonWasRunning = $true
    Stop-ScheduledTask -TaskName $taskName -EA SilentlyContinue
    Get-Process predictord -EA SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 700
  }
  Copy-Item $built (Join-Path $binDir 'predictord.exe') -Force
  # Les DLL deposees a cote par vcpkg (libcurl, zlib) DOIVENT suivre le binaire.
  # Sans elles le chargement echoue au demarrage — et comme le daemon est lie en
  # sous-systeme WINDOWS, il n'a aucune console ou se plaindre : echec silencieux.
  $dlls = Get-ChildItem (Split-Path $built) -Filter *.dll -EA SilentlyContinue
  foreach ($d in $dlls) { Copy-Item $d.FullName (Join-Path $binDir $d.Name) -Force }
  Write-Host "binaire  : $binDir\predictord.exe$(if ($dlls) { ' (+ ' + (($dlls.Name) -join ', ') + ')' })"
} elseif (-not (Test-Path (Join-Path $binDir 'predictord.exe'))) {
  throw "predictord.exe introuvable — lancez d'abord .\scripts\build-windows.ps1"
}

# --------------------------------------------------------------------- modèle --
if (-not $SkipModel) {
  if (Test-Path (Join-Path $modelDir 'words.tsv')) {
    Write-Host "modèle   : déjà présent ($modelDir)"
  } else {
    if (-not (Get-Command zstd -EA SilentlyContinue)) {
      throw 'zstd introuvable — winget install Meta.Zstandard (puis rouvrir le terminal)'
    }
    $url = "https://github.com/titoo-dev/predictive-ime/releases/download/$ModelTag/ime-model-$ModelTag.tar.zst"
    $zst = Join-Path $dataDir "ime-model-$ModelTag.tar.zst"
    if (-not (Test-Path $zst)) {
      Write-Host "==> téléchargement du modèle ($ModelTag)" -ForegroundColor Cyan
      curl.exe -fL --progress-bar -o $zst $url
      if ($LASTEXITCODE -ne 0) { throw "téléchargement échoué : $url" }
    }
    Write-Host '==> extraction' -ForegroundColor Cyan
    # tar est natif depuis Windows 10 1803, mais ne gère pas zstd : on décompresse
    # d'abord, puis on déballe le .tar.
    $tar = [IO.Path]::ChangeExtension($zst, $null).TrimEnd('.')
    zstd -d -f $zst -o $tar
    if ($LASTEXITCODE -ne 0) { throw 'zstd -d a échoué' }
    tar -xf $tar -C $modelDir
    if ($LASTEXITCODE -ne 0) { throw 'tar -xf a échoué' }
    Remove-Item $tar -Force
    Write-Host "modèle   : $modelDir"
  }
}

$words = Join-Path $modelDir 'words.tsv'
if (-not (Test-Path $words)) {
  # La release peut se déballer dans un sous-dossier selon sa version.
  $found = Get-ChildItem $modelDir -Recurse -Filter words.tsv -EA SilentlyContinue | Select-Object -First 1
  if ($found) { $words = $found.FullName }
}
if (-not (Test-Path $words)) { throw "words.tsv introuvable sous $modelDir" }

# --------------------------------------------------------------------- config --
$cfgFile = Join-Path $cfgDir 'config.json'
if (-not (Test-Path $cfgFile)) {
  @'
{
  "lang": "auto",
  "frenchSpacing": false,
  "autoCapitalize": false,
  "recencyBoost": 1.3,
  "agreeBoost": 2.0,
  "learnedBoost": 1.0
}
'@ | Set-Content -LiteralPath $cfgFile -Encoding utf8NoBOM
  Write-Host "config   : $cfgFile (défauts)"
} else {
  Write-Host "config   : $cfgFile (conservé)"
}

# ------------------------------------------------------------------ autostart --
if (-not $SkipAutostart) {
  $exe = Join-Path $binDir 'predictord.exe'
  $action = New-ScheduledTaskAction -Execute $exe -Argument "`"$words`"" -WorkingDirectory $binDir
  $trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
  # Hidden + pas de limite d'exécution : c'est un daemon, il vit toute la session.
  $settings = New-ScheduledTaskSettingsSet -Hidden `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)

  Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
    -Settings $settings -Description 'predictive-ime — daemon de prédiction n-gram' `
    -Force | Out-Null
  Write-Host "autostart: tâche « $taskName » à l'ouverture de session"
}

# ------------------------------------------------------- text service TSF ----
# C'est CE composant qui fait de predictive-ime une vraie méthode de saisie :
# il se charge dans chaque application et y pose préedit, candidats et commit.
if (-not $SkipIme) {
  $tsfSrc = @(
    "$repo\build-win-x64\win\tsf\Release\predict-tsf.dll",
    "$repo\build-win-x64\win\tsf\predict-tsf.dll"
  ) | Where-Object { Test-Path $_ } | Select-Object -First 1

  if (-not $tsfSrc) {
    Write-Host 'IME      : predict-tsf.dll absente — lancez .\scripts\build-windows.ps1' -ForegroundColor Yellow
  } else {
    # Une DLL de text service est chargée dans CHAQUE application qui reçoit de
    # la frappe (explorateur, navigateur, terminal…) : Windows la verrouille et
    # l'écraser est IMPOSSIBLE sans fermer tout le bureau. On installe donc un
    # nom horodaté et on enregistre celui-là ; les anciens sont supprimés dès
    # que plus personne ne les tient (au prochain passage, ou après une
    # reconnexion).
    $stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
    $tsfDst = Join-Path $binDir "predict-tsf-$stamp.dll"
    try {
      Copy-Item $tsfSrc $tsfDst -Force -ErrorAction Stop
    } catch {
      Write-Host "IME      : copie impossible ($($_.Exception.Message))" -ForegroundColor Red
      $tsfDst = $null
    }

    # Ménage des versions précédentes, sans bruit si elles sont encore chargées.
    Get-ChildItem $binDir -Filter 'predict-tsf*.dll' -EA SilentlyContinue |
      Where-Object { $_.FullName -ne $tsfDst } |
      ForEach-Object {
        try {
          Start-Process regsvr32.exe -ArgumentList '/s','/u',"`"$($_.FullName)`"" -Wait -EA SilentlyContinue
          Remove-Item $_.FullName -Force -ErrorAction Stop
        } catch { }  # encore chargée : elle partira au prochain coup
      }

    if ($tsfDst) {
      # AppContainer (applications du Store, navigateurs) : deux droits
      # distincts, et aucun de trop.
      #   - LIRE/EXÉCUTER la DLL, sinon elle ne se charge pas du tout ;
      #   - ATTEINDRE le socket, sinon elle se charge mais ne prédit RIEN
      #     (c'est exactement le symptôme « la barre s'affiche, vide »).
      # Le droit sur le socket porte sur le SOUS-DOSSIER ipc\ seulement : le
      # donner sur le dossier parent exposerait user.log, c'est-à-dire les mots
      # que l'utilisateur a tapés, à n'importe quelle application du Store.
      # DEUX SID, pas un : S-1-15-2-1 (ALL APPLICATION PACKAGES) ne couvre PAS
      # les bacs à sable restreints (LPAC) — Chrome et une partie des
      # applications du Store en sont. Pour eux seul S-1-15-2-2 (ALL RESTRICTED
      # APPLICATION PACKAGES) ouvre l'accès, et son absence se manifeste
      # exactement comme ici : la DLL se charge, mais n'atteint pas le daemon.
      # On passe par les SID, pas par les noms : ceux-ci dépendent de la langue
      # de Windows.
      $sids = @(
        (New-Object System.Security.Principal.SecurityIdentifier 'S-1-15-2-1'),
        (New-Object System.Security.Principal.SecurityIdentifier 'S-1-15-2-2')
      )
      $ipcDir = Join-Path $dataDir 'ipc'
      New-Item -ItemType Directory -Force $ipcDir | Out-Null

      function Grant-Sids([string] $path, [string] $rights, $sidList,
                          [switch] $ThisFolderOnly) {
        $acl = Get-Acl $path
        $inherit = if ($ThisFolderOnly -or -not (Test-Path $path -PathType Container)) {
          [System.Security.AccessControl.InheritanceFlags]::None
        } else {
          [System.Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
        }
        foreach ($s in $sidList) {
          $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule(
            $s, [System.Security.AccessControl.FileSystemRights]$rights, $inherit,
            [System.Security.AccessControl.PropagationFlags]::None,
            [System.Security.AccessControl.AccessControlType]::Allow)))
        }
        Set-Acl $path $acl
      }
      Grant-Sids $binDir 'ReadAndExecute' $sids
      Grant-Sids $tsfDst 'ReadAndExecute' $sids
      # Le socket est recréé à chaque démarrage du daemon : l'héritage du
      # dossier est ce qui fait tenir le droit dans la durée.
      Grant-Sids $ipcDir 'Modify' $sids
      # Traversée du dossier parent : sans elle, le bac à sable ne peut pas
      # ATTEINDRE ipc\ même avec tous les droits dessus. -ThisFolderOnly est
      # essentiel — hérité, ce droit s'appliquerait aussi à user.log, qu'on
      # veut laisser hors de portée.
      Grant-Sids $dataDir 'ExecuteFile' $sids -ThisFolderOnly

      # regsvr32 écrit sous HKCR et déclare le profil clavier : administrateur
      # requis. C'est le SEUL moment de l'installation qui élève.
      $admin = ([Security.Principal.WindowsPrincipal] `
        [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
          [Security.Principal.WindowsBuiltInRole]::Administrator)
      if ($admin) {
        $p = Start-Process regsvr32.exe -ArgumentList '/s', "`"$tsfDst`"" -Wait -PassThru
        if ($p.ExitCode -eq 0) {
          Write-Host "IME      : enregistre ($tsfDst)"
          Add-PredictToLanguageList
          Clear-PredictStaleSubstitute
        }
        else { Write-Host "IME      : regsvr32 a echoue (code $($p.ExitCode))" -ForegroundColor Red }
      } else {
        Write-Host "IME      : copiee, enregistrement a faire EN ADMINISTRATEUR :" -ForegroundColor Yellow
        Write-Host "           regsvr32 `"$tsfDst`""
      }
    }
  }
} elseif ($AddInputMethod) {
  # Installeur : la DLL est inscrite, il reste à la proposer à l'utilisateur.
  Add-PredictToLanguageList
  Clear-PredictStaleSubstitute
}

# Relance du daemon si on l'a arrêté (ou s'il ne tournait pas et que
# l'autodémarrage vient d'être posé) : l'installation doit rendre la main sur
# un système qui PRÉDIT, pas sur un système à redémarrer à la main.
if (-not $SkipAutostart -and -not (Get-Process predictord -EA SilentlyContinue)) {
  Start-ScheduledTask -TaskName $taskName -EA SilentlyContinue
  $deadline = (Get-Date).AddSeconds(90)
  while (-not (Get-Process predictord -EA SilentlyContinue) -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 400
  }
  Write-Host ("daemon   : " + $(if (Get-Process predictord -EA SilentlyContinue) {
    'relance (chargement du modele ~6 s)' } else { 'NON RELANCE — Start-ScheduledTask a echoue' }))
}

# Une DLL de text service est chargée DANS chaque application qui tape, et
# Windows l'y garde jusqu'à la fin du PROCESSUS. Après une mise à jour, ces
# applications continuent donc d'exécuter l'ancienne version — le symptôme est
# « ça marche dans une appli et pas dans les autres », qu'on ne devinerait
# jamais. Pire : fermer la fenêtre ne suffit pas pour Terminal, Chrome ou les
# applis du Store, qui réutilisent le même processus. On les NOMME donc.
if (-not $SkipIme) {
  $current = (Get-ItemProperty 'Registry::HKEY_CLASSES_ROOT\CLSID\{5F0A1C7E-3B84-4D2E-9C31-7A6E2D4B8F10}\InprocServer32' -EA SilentlyContinue).'(default)'
  $stale = @()
  foreach ($p in Get-Process -EA SilentlyContinue) {
    try {
      $m = $p.Modules | Where-Object { $_.ModuleName -like 'predict-tsf*' }
      if ($m -and $current -and $m[0].FileName -ne $current) { $stale += $p }
    } catch { }
  }
  if ($stale) {
    Write-Host "`nCes applications tournent encore sur l'ANCIENNE version :" -ForegroundColor Yellow
    $stale | Sort-Object ProcessName -Unique | ForEach-Object {
      Write-Host "  - $($_.ProcessName) (PID $($_.Id))"
    }
    Write-Host "Fermer leur fenetre NE SUFFIT PAS (Terminal, Chrome et les applis" -ForegroundColor Yellow
    Write-Host "du Store gardent leur processus). Le balayage fiable est une" -ForegroundColor Yellow
    Write-Host "deconnexion/reconnexion de session ; sinon, par application :" -ForegroundColor Yellow
    Write-Host "  Stop-Process -Name WindowsTerminal, chrome   # etc."
  }
}

Write-Host "`nDémarrer maintenant :  Start-ScheduledTask -TaskName $taskName" -ForegroundColor Green
Write-Host "Vérifier          :  .\scripts\probe-daemon.ps1 -Context je -Prefix v"
Write-Host "Puis ajoutez « Predict » dans Paramètres > Heure et langue > Saisie > Clavier."

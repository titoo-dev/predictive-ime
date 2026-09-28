<#
.SYNOPSIS
  Testeur interactif : tape une phrase, vois les suggestions en direct.

.DESCRIPTION
  Reproduit dans le terminal la boucle que tiendra le text service TSF —
  frappe → requête au daemon → barre de candidats — pour juger la QUALITÉ du
  modèle sans attendre la phase 3. Ce n'est PAS l'IME : rien n'est inséré dans
  les autres applications.

  Différence importante avec l'engine fcitx5 : ici UNE connexion est gardée
  ouverte pour toute la session (le daemon multiplexe les lignes), alors que
  l'engine rouvre une connexion par requête. Plus rapide pour un REPL.

  L'apprentissage est DÉSACTIVÉ par défaut : un testeur ne doit pas polluer
  ton journal de mots appris. -Learn l'active si tu veux justement l'observer.

.PARAMETER Learn
  Envoie un `learn` à chaque mot validé (écrit dans user.log).

.EXAMPLE
  .\scripts\try-daemon.ps1
  .\scripts\try-daemon.ps1 -Learn
#>
[CmdletBinding()]
param(
  [string] $SocketPath = "$env:LOCALAPPDATA\ime-predictord\ipc\predictord.sock",
  [switch] $Learn,
  [int]    $TimeoutMs = 3000
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $SocketPath)) {
  Write-Host "socket absent : $SocketPath" -ForegroundColor Red
  Write-Host 'Demarre le daemon :  Start-ScheduledTask -TaskName ime-predictord'
  return
}

# Sans ca les accents et les emoji sortent en mojibake dans la console.
$prevOut = [Console]::OutputEncoding
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

# ----------------------------------------------------------------- connexion --
$utf8   = [System.Text.UTF8Encoding]::new($false)
$sock   = [System.Net.Sockets.Socket]::new('Unix', 'Stream', 'Unspecified')
$sock.ReceiveTimeout = $TimeoutMs
$sock.SendTimeout    = $TimeoutMs
try {
  $sock.Connect([System.Net.Sockets.UnixDomainSocketEndPoint]::new($SocketPath))
} catch [System.Net.Sockets.SocketException], [System.Management.Automation.MethodInvocationException] {
  # Le fichier de socket SURVIT a l'arret du daemon : Test-Path ne prouve donc
  # rien, seule la connexion tranche. « Connection refused » = fichier orphelin.
  Write-Host "le daemon ne repond pas ($SocketPath)" -ForegroundColor Red
  Write-Host 'Le fichier de socket existe mais personne n''ecoute : le daemon est arrete.'
  Write-Host ''
  Write-Host '  Start-ScheduledTask -TaskName ime-predictord' -ForegroundColor Yellow
  Write-Host ''
  Write-Host "Journal : $env:LOCALAPPDATA\ime-predictord\predictord.log"
  [Console]::OutputEncoding = $prevOut
  return
}
$stream = [System.Net.Sockets.NetworkStream]::new($sock, $false)
$reader = [System.IO.StreamReader]::new($stream, $utf8)
$writer = [System.IO.StreamWriter]::new($stream, $utf8)
$writer.NewLine  = "`n"    # le protocole attend '\n', surtout pas "\r\n"
$writer.AutoFlush = $true

function Send-Req([hashtable] $req) {
  $writer.WriteLine(($req | ConvertTo-Json -Compress -Depth 6))
  # Toute requete a une reponse — y compris `learn`. Il FAUT la lire, sinon les
  # reponses se decalent d'un cran sur cette connexion persistante.
  $line = $reader.ReadLine()
  if (-not $line) { throw 'connexion fermee par le daemon' }
  $line | ConvertFrom-Json
}

# --------------------------------------------------------------------- etat --
# Caracteres non alphanumeriques qui font partie d'un mot : apostrophe droite,
# apostrophe typographique (j'ai / j’ai), ':' (declencheur emoji), trait d'union.
$WORD_EXTRA = [char[]] @(0x27, 0x2019, 0x3A, 0x2D)

$words  = [System.Collections.Generic.List[string]]::new()  # mots valides
$buffer = ''                                                # mot en cours
$cands  = @()
$ghost  = ''
$auto   = ''
$sel    = -1        # -1 = aucune navigation ; sinon index dans $cands
$err    = ''

function Update-Suggestions {
  $script:sel = -1
  $ctx = @()
  if ($words.Count -gt 0) { $ctx = @($words | Select-Object -Last 2) }
  try {
    $r = Send-Req @{ context = $ctx; prefix = $buffer }
    $script:cands = @($r.candidates)
    $script:ghost = [string]$r.ghost
    $script:auto  = [string]$r.autocomplete
    $script:err   = ''
  } catch {
    $script:cands = @(); $script:ghost = ''; $script:auto = ''
    $script:err = $_.Exception.Message
  }
}

# ------------------------------------------------------------------ affichage --
$RESET = "`e[0m"; $DIM = "`e[2m"; $BOLD = "`e[1m"
$CYAN  = "`e[36m"; $GREEN = "`e[32m"; $YELLOW = "`e[33m"; $RED = "`e[31m"

Write-Host ''
Write-Host "${BOLD}predictive-ime — testeur interactif${RESET}"
Write-Host "${DIM}Espace = valider (applique la completion si elle est en gras) · Tab = candidat suivant"
Write-Host "Entree = valider le candidat choisi · Retour arriere = effacer · Echap = quitter${RESET}"
if ($Learn) { Write-Host "${YELLOW}apprentissage ACTIF — les mots valides sont ecrits dans user.log${RESET}" }
else        { Write-Host "${DIM}apprentissage desactive (-Learn pour l'activer)${RESET}" }
Write-Host ''

$LINES = 5
1..$LINES | ForEach-Object { Write-Host '' }
$anchor = [Console]::CursorTop - $LINES
[Console]::CursorVisible = $false

function Write-Padded([string] $text, [int] $row) {
  $w = [Math]::Max(20, [Console]::WindowWidth - 1)
  # Longueur VISIBLE : les sequences ANSI ne prennent pas de colonne.
  $plain = [regex]::Replace($text, "`e\[[0-9;]*m", '')
  if ($plain.Length -gt $w) {
    $text = $plain.Substring(0, $w); $plain = $text
  }
  [Console]::SetCursorPosition(0, $row)
  [Console]::Write($text + (' ' * ($w - $plain.Length)))
}

function Render {
  # @(...) des deux cotes : une List[string] et un tableau ne se concatenent pas
  # directement avec '+' en PowerShell.
  $typed = ((@($words) + @($buffer)) -join ' ').TrimEnd()
  if ($words.Count -gt 0 -and $buffer -eq '') { $typed = (@($words) -join ' ') + ' ' }

  # Le fantome : la fin du mot que l'Espace completerait.
  $tail = ''
  if ($ghost -and $buffer -and $ghost.StartsWith($buffer, 'CurrentCultureIgnoreCase')) {
    $tail = $ghost.Substring($buffer.Length)
  }

  Write-Padded "  $typed${DIM}${tail}${RESET}${CYAN}|${RESET}" ($anchor + 1)

  if ($err) {
    Write-Padded "  ${RED}$err${RESET}" ($anchor + 3)
  } elseif ($cands.Count -eq 0) {
    Write-Padded "  ${DIM}(aucune suggestion)${RESET}" ($anchor + 3)
  } else {
    $parts = for ($i = 0; $i -lt $cands.Count -and $i -lt 6; $i++) {
      $c = $cands[$i]
      if ($i -eq $sel)          { "${CYAN}${BOLD}[$c]${RESET}" }   # choisi au Tab
      elseif ($c -eq $auto -and $auto) { "${GREEN}${BOLD}$c${RESET}" } # applique par Espace
      else                      { "${DIM}$($i+1)${RESET} $c" }
    }
    Write-Padded ('  ' + ($parts -join '   ')) ($anchor + 3)
  }

  $hint = if ($buffer -eq '') { 'mot suivant' } else { "prefixe « $buffer »" }
  Write-Padded "  ${DIM}$hint · $($cands.Count) candidat(s)${RESET}" ($anchor + 4)
}

# ---------------------------------------------------------------- validation --
function Commit-Word([string] $word) {
  if (-not $word) { return }
  $words.Add($word)
  if ($Learn) {
    $prev = if ($words.Count -ge 2) { $words[$words.Count - 2] } else { '' }
    try { Send-Req @{ learn = @{ prev = $prev; word = $word } } | Out-Null } catch { }
  }
  $script:buffer = ''
}

# --------------------------------------------------------------------- boucle --
try {
  Update-Suggestions
  Render

  while ($true) {
    $k = [Console]::ReadKey($true)

    switch ($k.Key) {
      'Escape' { return }

      'Backspace' {
        if ($buffer.Length -gt 0) {
          $buffer = $buffer.Substring(0, $buffer.Length - 1)
        } elseif ($words.Count -gt 0) {
          # Rien a effacer dans le mot courant : on reprend le precedent.
          $buffer = $words[$words.Count - 1]
          $words.RemoveAt($words.Count - 1)
        }
        Update-Suggestions
      }

      'Tab' {
        if ($cands.Count -gt 0) { $script:sel = ($sel + 1) % $cands.Count }
      }

      'LeftArrow'  { if ($cands.Count -gt 0) { $script:sel = (($sel - 1) + $cands.Count) % $cands.Count } }
      'RightArrow' { if ($cands.Count -gt 0) { $script:sel = ($sel + 1) % $cands.Count } }

      'Enter' {
        # Entree valide le candidat surligne (ou le 1er s'il y en a un).
        $pick = if ($sel -ge 0) { $cands[$sel] } elseif ($cands.Count -gt 0) { $cands[0] } else { $buffer }
        Commit-Word $pick
        Update-Suggestions
      }

      'Spacebar' {
        # Le VRAI comportement de l'engine : l'Espace applique `autocomplete`
        # quand le daemon en propose un, sinon il garde le mot tape tel quel.
        $pick = if ($sel -ge 0) { $cands[$sel] } elseif ($auto) { $auto } else { $buffer }
        Commit-Word $pick
        Update-Suggestions
      }

      default {
        $ch = $k.KeyChar
        # Lettres, chiffres, apostrophe (droite ET typographique), ':' pour les
        # emoji, '-' — mais pas les touches mortes, qui renvoient le caractere nul.
        # Codes plutot que litteraux : PowerShell traite U+2019 comme un
        # DELIMITEUR de chaine, donc '<U+2019>' ne se termine jamais.
        if ($ch -ne [char]0 -and
            ([char]::IsLetterOrDigit($ch) -or $WORD_EXTRA -contains $ch)) {
          $buffer += $ch
          Update-Suggestions
        }
      }
    }
    Render
  }
}
finally {
  [Console]::CursorVisible = $true
  [Console]::SetCursorPosition(0, $anchor + $LINES)
  Write-Host ''
  if ($words.Count -gt 0 -or $buffer) {
    Write-Host "texte : $(((@($words) + @($buffer)) -join ' ').TrimEnd())"
  }
  foreach ($d in @($reader, $writer, $stream, $sock)) {
    try { $d.Dispose() } catch { }
  }
  [Console]::OutputEncoding = $prevOut
}

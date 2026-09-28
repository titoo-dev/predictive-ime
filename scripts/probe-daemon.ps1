<#
.SYNOPSIS
  Sonde le daemon predictord depuis Windows — une requête JSON, une réponse.

.DESCRIPTION
  Équivalent Windows de `echo '{...}' | nc -U /tmp/ime-predictord.sock`.
  `nc -U` n'existe pas ici et Node mappe net.connect({path}) sur les named
  pipes, pas sur AF_UNIX : on passe donc par .NET (UnixDomainSocketEndPoint,
  .NET 5+), qui parle le VRAI AF_UNIX de Windows 10 1803+/11 — exactement le
  transport qu'utilisera le text service TSF.

.EXAMPLE
  .\probe-daemon.ps1 -Context je -Prefix v
  .\probe-daemon.ps1 -Raw '{"stats":true}'
#>
[CmdletBinding()]
param(
  [string[]] $Context = @(),
  [string]   $Prefix  = '',
  [string]   $Raw,
  [string]   $SocketPath = "$env:LOCALAPPDATA\ime-predictord\ipc\predictord.sock",
  [int]      $TimeoutMs = 5000
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $SocketPath)) {
  throw "socket absent : $SocketPath — le daemon tourne-t-il ?"
}

$payload = if ($Raw) { $Raw } else {
  # -Compress : le protocole est UNE ligne JSON terminée par '\n'.
  @{ context = @($Context); prefix = $Prefix } | ConvertTo-Json -Compress
}

$sock = [System.Net.Sockets.Socket]::new(
  [System.Net.Sockets.AddressFamily]::Unix,
  [System.Net.Sockets.SocketType]::Stream,
  [System.Net.Sockets.ProtocolType]::Unspecified)
$sock.ReceiveTimeout = $TimeoutMs
$sock.SendTimeout    = $TimeoutMs

try {
  try {
    $sock.Connect([System.Net.Sockets.UnixDomainSocketEndPoint]::new($SocketPath))
  } catch {
    # Le fichier de socket survit a l'arret du daemon — Test-Path ci-dessus ne
    # prouve rien, seule la connexion tranche.
    throw ("daemon injoignable sur $SocketPath (fichier orphelin ?) — " +
           'demarre-le : Start-ScheduledTask -TaskName ime-predictord')
  }

  # UTF8Encoding($false) : surtout pas de BOM, le daemon parse la ligne brute.
  $utf8 = [System.Text.UTF8Encoding]::new($false)
  $out  = $utf8.GetBytes($payload + "`n")
  [void] $sock.Send($out)

  $buf = [byte[]]::new(65536)
  $acc = [System.Text.StringBuilder]::new()
  while ($true) {
    $n = $sock.Receive($buf)
    if ($n -le 0) { break }
    [void] $acc.Append($utf8.GetString($buf, 0, $n))
    if ($acc.ToString().Contains("`n")) { break }   # une ligne = une réponse
  }

  $line = $acc.ToString().Split("`n")[0]
  if (-not $line) { throw 'réponse vide' }
  $line | ConvertFrom-Json | ConvertTo-Json -Depth 6
}
finally {
  $sock.Dispose()
}

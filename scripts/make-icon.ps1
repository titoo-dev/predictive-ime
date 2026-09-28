<#
.SYNOPSIS
  Génère win/tsf/predict.ico — l'icône du profil clavier, de l'installeur et
  de « Applications installées ».

.DESCRIPTION
  Le motif est celui du produit : du texte tapé (barre pleine), le curseur,
  puis la suggestion fantôme (barre pâle). Squircle au dégradé de l'accent
  Windows 11 par défaut. Chaque taille est dessinée À SA taille (pas de
  réduction d'une image 256) pour rester nette en 16 px, et l'ICO embarque
  des PNG — format accepté depuis Windows Vista.

  L'icône est versionnée ; ce script n'est à relancer que pour la modifier.

.EXAMPLE
  .\scripts\make-icon.ps1
#>
[CmdletBinding()]
param([string] $Out = (Join-Path (Split-Path -Parent $PSScriptRoot) 'win\tsf\predict.ico'))

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-RoundedPath([float] $x, [float] $y, [float] $w, [float] $h, [float] $r) {
  $p = New-Object System.Drawing.Drawing2D.GraphicsPath
  $d = 2 * $r
  $p.AddArc($x, $y, $d, $d, 180, 90)
  $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
  $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
  $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
  $p.CloseFigure()
  return $p
}

function New-IconPng([int] $s) {
  $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'
  $g.PixelOffsetMode = 'HighQuality'
  $g.Clear([System.Drawing.Color]::Transparent)

  # Fond : squircle, dégradé Light1 → Dark1 de l'accent par défaut.
  $inset = [Math]::Max(0.5, $s * 0.03)
  $bg = New-RoundedPath $inset $inset ($s - 2 * $inset) ($s - 2 * $inset) ($s * 0.23)
  $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush `
    (New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF $s, $s), `
    ([System.Drawing.Color]::FromArgb(255, 0x00, 0x91, 0xF8)), `
    ([System.Drawing.Color]::FromArgb(255, 0x00, 0x5F, 0xB8))
  $g.FillPath($grad, $bg)

  # Motif « tapé | fantôme ». Les petites tailles arrondissent sur la grille
  # de pixels, sinon les barres deviennent une bouillie grise en 16 px.
  $snap = { param($v) if ($s -le 32) { [Math]::Round($v) } else { $v } }
  $barH = & $snap ([Math]::Max(2, $s * 0.15))
  $cy = $s / 2
  $top = & $snap ($cy - $barH / 2)
  $r = $barH / 2
  $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
  $ghost = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(115, 255, 255, 255))

  # Asymétrique à dessein : centré, le motif se lit comme un « + ».
  $typedX = & $snap ($s * 0.16); $typedW = & $snap ($s * 0.36)
  $g.FillPath($white, (New-RoundedPath $typedX $top $typedW $barH $r))

  $caretW = & $snap ([Math]::Max(1.5, $s * 0.07))
  # Plus haut en petite taille : à 16 px, un curseur court se lit « + ».
  $caretH = & $snap ($s * $(if ($s -le 24) { 0.62 } else { 0.46 }))
  $caretX = & $snap ($s * 0.60 - $caretW / 2)
  $caretY = & $snap ($cy - $caretH / 2)
  $g.FillPath($white, (New-RoundedPath $caretX $caretY $caretW $caretH ([Math]::Min($caretW / 2, $r))))

  $ghostX = & $snap ($s * 0.68); $ghostW = & $snap ($s * 0.17)
  $g.FillPath($ghost, (New-RoundedPath $ghostX $top $ghostW $barH $r))

  $g.Dispose()
  $ms = New-Object System.IO.MemoryStream
  $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  return , $ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($s in $sizes) { , (New-IconPng $s) }

# ICONDIR + ICONDIRENTRY[] + données PNG.
$fs = [System.IO.File]::Create($Out)
$w = New-Object System.IO.BinaryWriter $fs
$w.Write([UInt16] 0); $w.Write([UInt16] 1); $w.Write([UInt16] $sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $s = $sizes[$i]; $len = $images[$i].Length
  $dim = if ($s -ge 256) { 0 } else { $s }
  $w.Write([byte] $dim); $w.Write([byte] $dim); $w.Write([byte] 0); $w.Write([byte] 0)
  $w.Write([UInt16] 1); $w.Write([UInt16] 32)
  $w.Write([UInt32] $len); $w.Write([UInt32] $offset)
  $offset += $len
}
foreach ($img in $images) { $w.Write($img) }
$w.Close()
Write-Host "icône : $Out ($($sizes -join ', ') px)"

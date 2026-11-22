# Windows icon from the Mac one: Resources/AppIcon-1024.png -> Resources/AppIcon.ico
# (PNG-compressed entries at the sizes Explorer, the taskbar and the tray ask for).
#   powershell -ExecutionPolicy Bypass -File scripts\make_ico.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$src = [System.Drawing.Image]::FromFile((Join-Path $root 'Resources\AppIcon-1024.png'))
$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$pngs = foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    # the Mac icon has macOS's ~10% margin around its rounded square; Windows icons fill the tile
    $crop = New-Object System.Drawing.Rectangle 92, 92, 840, 840
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, $s, $s), $crop, [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}
$src.Dispose()
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)   # ICONDIR
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {                                      # ICONDIRENTRY
    $s = $sizes[$i]
    $w.Write([byte]($s % 256)); $w.Write([byte]($s % 256))                     # 256 is stored as 0
    $w.Write([byte]0); $w.Write([byte]0); $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $root 'Resources\AppIcon.ico'), $out.ToArray())
Write-Host "wrote Resources\AppIcon.ico ($($sizes -join ', ') px)"

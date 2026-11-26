# Builds docs/Surround-Bridge-User-Manual.pdf from docs/manual/manual.html:
#   1. fresh screenshots from the app's --snapshot (light theme; never switches anything on; test
#      settings and a generic install path, so nothing of this PC's shows up), unless -NoSnapshots
#   2. Edge (headless) prints the page to PDF.
#   scripts\make_manual.ps1 [-App path\to\SurroundBridge.exe or the Setup exe] [-NoSnapshots]
param([string]$App = "", [switch]$NoSnapshots)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$manual = Join-Path $root "docs\manual"
$img = Join-Path $manual "img"
$pdf = Join-Path $root "docs\Surround-Bridge-User-Manual.pdf"

if (-not $NoSnapshots) {
    if (-not $App) {
        $App = Get-ChildItem (Join-Path $root "build") -Filter "SurroundBridge-Setup-*-windows-x64.exe" -ErrorAction SilentlyContinue |
               Sort-Object LastWriteTime | Select-Object -Last 1 -ExpandProperty FullName
    }
    if (-not $App) { throw "No app to take screenshots with: run scripts\build_win.ps1 -Package first, or pass -App" }
    $tmp = Join-Path $env:TEMP ("sb-manual-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $env:SURROUND_BRIDGE_SETTINGS = Join-Path $tmp "settings.json"     # never the real settings
    $env:SURROUND_BRIDGE_TEST_ROOT = "C:\Users\You\AppData\Local"       # the install path shown (nothing is written there)
    $p = Start-Process $App -ArgumentList "--snapshot", "$tmp\shots", "--theme", "light" -PassThru -Wait
    if ($p.ExitCode -ne 0) { throw "Snapshot failed ($($p.ExitCode))" }
    $names = "flyout-setup", "flyout-meters-upmix", "flyout-meters-51", "flyout-meters-stereo-in-51",
             "settings-general", "settings-upmix", "setup-install"
    foreach ($n in $names) { Copy-Item "$tmp\shots\$n-light.png" (Join-Path $img "$n.png") -Force }
    Copy-Item (Join-Path $root "Resources\AppIcon-1024.png") (Join-Path $img "icon.png") -Force
    Remove-Item -Recurse -Force $tmp
}

# the meter examples: just the flyout's header and meters (the top 262 of its 770 px)
Add-Type -AssemblyName System.Drawing
foreach ($n in "flyout-meters-51", "flyout-meters-stereo-in-51") {
    $src = [System.Drawing.Bitmap]::FromFile((Join-Path $img "$n.png"))
    $crop = $src.Clone((New-Object System.Drawing.Rectangle 0, 0, $src.Width, 262), $src.PixelFormat)
    $src.Dispose()
    $crop.Save((Join-Path $img "$n-top.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $crop.Dispose()
}

$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw "Microsoft Edge not found" }
$url = "file:///" + ((Join-Path $manual "manual.html") -replace "\\", "/")
$profileDir = Join-Path $env:TEMP ("sb-manual-edge-" + [guid]::NewGuid().ToString("N"))
$p = Start-Process $edge -ArgumentList "--headless=new", "--disable-gpu", "--no-pdf-header-footer", "--user-data-dir=$profileDir",
                                       "--print-to-pdf=$pdf", $url -PassThru -Wait
Remove-Item -Recurse -Force $profileDir -ErrorAction SilentlyContinue
if (-not (Test-Path $pdf)) { throw "Edge didn't write the PDF" }
"manual: $pdf ($([math]::Round((Get-Item $pdf).Length / 1KB)) KB)"

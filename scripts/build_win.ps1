# Windows build: the C core with clang-cl + Ninja (CMakeLists.txt), and with -App the tray app
# (Windows\SurroundBridge, WPF, needs the .NET 8 SDK).
#   powershell -ExecutionPolicy Bypass -File scripts\build_win.ps1 [-Config Release|Debug] [-App] [-Package]
# Output: .build\win-<config>\sbridge.exe, bridgecore.dll
#   -App:     Windows\SurroundBridge\bin\x64\Release\net8.0-windows\win-x64\SurroundBridge.exe (runs in place)
#   -Package: build\SurroundBridge-Setup-<ver>-windows-x64.exe: one self-contained exe (nothing else
#             to install) that installs itself when opened. Version from SurroundBridge.csproj.
# FFmpeg: vendor\ffmpeg\windows-x64 (scripts\build_ffmpeg_win.ps1), else ..\music_player's.
param([string]$Config = 'Release', [switch]$App, [switch]$Package)
if ($Package) { $App = $true; $Config = 'Release' }
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\win_env.ps1"
Enter-PcDevShell
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root ".build\win-$($Config.ToLower())"
cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DCMAKE_C_COMPILER=clang-cl
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $build
if ($LASTEXITCODE -or -not $App) { exit $LASTEXITCODE }

# the app always takes the Release engine (EngineDir in SurroundBridge.csproj)
$env:PATH = "$env:ProgramFiles\dotnet;$env:PATH"
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_NOLOGO = '1'
$proj = Join-Path $root 'Windows\SurroundBridge\SurroundBridge.csproj'
if (-not $Package) {
    dotnet build $proj -c Release -p:Platform=x64
    exit $LASTEXITCODE
}
$out = Join-Path $root 'build\windows\publish'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
# own obj/bin, so the single-file build never mixes with the normal one
$sfObj = Join-Path $root 'build\windows\obj-singlefile\'
$sfBin = Join-Path $root 'build\windows\bin-singlefile\'
dotnet publish $proj -c Release -p:Platform=x64 -r win-x64 --self-contained true -o $out `
    -p:PublishSingleFile=true "-p:IntermediateOutputPath=$sfObj" "-p:OutDir=$sfBin"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$ver = ([xml](Get-Content $proj)).Project.PropertyGroup.Version | Where-Object { $_ } | Select-Object -First 1
$exe = Join-Path $root "build\SurroundBridge-Setup-$ver-windows-x64.exe"
Copy-Item (Join-Path $out 'SurroundBridge.exe') $exe -Force
$extra = Get-ChildItem $out -File | Where-Object { $_.Name -ne 'SurroundBridge.exe' }
if ($extra) { Write-Warning "files left beside the exe (not inside it): $($extra.Name -join ', ')" }
Write-Host ("packaged: $exe ({0:N1} MB)" -f ((Get-Item $exe).Length / 1MB))
exit 0

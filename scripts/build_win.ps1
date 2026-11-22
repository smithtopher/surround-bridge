# Configures and builds the Windows targets (CMakeLists.txt) with clang-cl + Ninja.
#   powershell -ExecutionPolicy Bypass -File scripts\build_win.ps1 [-Config Release|Debug]
# Output: .build\win-<config>\sbridge.exe
# FFmpeg: vendor\ffmpeg\windows-x64 (scripts\build_ffmpeg_win.ps1), else ..\music_player's.
param([string]$Config = 'Release')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\win_env.ps1"
Enter-PcDevShell
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root ".build\win-$($Config.ToLower())"
cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DCMAKE_C_COMPILER=clang-cl
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $build
exit $LASTEXITCODE

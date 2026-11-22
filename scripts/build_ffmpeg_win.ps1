# Builds FFmpeg for Windows: enters the Visual Studio x64 developer environment, then
# runs scripts/build_ffmpeg_win.sh in MSYS2 bash with that environment inherited.
#   powershell -ExecutionPolicy Bypass -File scripts\build_ffmpeg_win.ps1
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\win_env.ps1"
Enter-PcDevShell
$env:MSYSTEM = 'MSYS'            # plain MSYS tools; nothing from ucrt64 leaks into the build
$env:MSYS2_PATH_TYPE = 'inherit' # keep cl.exe / link.exe from the VS environment
$env:CHERE_INVOKING = '1'
& "$env:MSYS2_ROOT\usr\bin\bash.exe" -l "$PSScriptRoot/build_ffmpeg_win.sh"
exit $LASTEXITCODE

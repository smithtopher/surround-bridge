# Shared Windows build environment helpers (dot-source this file).
#   Enter-PcDevShell : imports the Visual Studio x64 developer environment (cl, link,
#                      clang-cl, cmake, ninja, Windows SDK) into the current PowerShell.
#   $env:MSYS2_ROOT  : MSYS2 install (default C:\msys64).
if (-not $env:MSYS2_ROOT) { $env:MSYS2_ROOT = 'C:\msys64' }

function Enter-PcDevShell {
    if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64') { return }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio C++ build tools not found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    # run vcvars64 in cmd and copy the resulting environment back (vswhere on PATH
    # silences vcvars' "'vswhere.exe' is not recognized" message)
    $env:PATH = "$(Split-Path $vswhere);$env:PATH"
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
    }
    if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') { throw 'vcvars64.bat did not set up an x64 environment' }
}

<#
.SYNOPSIS
    Build and run the host tests with MSVC (the Windows path).

.DESCRIPTION
    The Makefile targets GCC/Clang, which is the canonical path (CI, Linux, and
    the repo's existing Docker build wrappers). This script exists because a
    stock Windows dev box has MSVC and no GCC — see compat.hh for the one
    incompatibility that matters (__builtin_memcpy).

.EXAMPLE
    .\build_msvc.ps1            # build + run the unit tests
    .\build_msvc.ps1 -Vectors   # regenerate the cross-language frame corpus
#>
param(
    [switch]$Vectors
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - is Visual Studio installed?" }

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio install with the C++ toolchain was found." }

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$build = Join-Path $here "build"
New-Item -ItemType Directory -Force $build | Out-Null

$srcDir  = Join-Path $here "..\..\src"
$vecDir  = Join-Path $here "..\vectors"

# /FI force-includes the shim so the engine's __builtin_* calls resolve without
# any edit to engine source.
$common = "/nologo /std:c++20 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /FI compat.hh /I `"$here`" /I `"$srcDir`""
$engine = "`"$srcDir\netlink.cpp`""

if ($Vectors) {
    New-Item -ItemType Directory -Force $vecDir | Out-Null
    $cmd = "cl $common /Fe:`"$build\gen_vectors.exe`" /Fo:`"$build\\`" `"$here\gen_vectors.cpp`" $engine"
    cmd /c "`"$vcvars`" >nul 2>&1 && $cmd"
    if ($LASTEXITCODE -ne 0) { throw "compile failed" }
    & "$build\gen_vectors.exe" $vecDir
    if ($LASTEXITCODE -ne 0) { throw "vector generation failed" }
} else {
    # Each binary gets its own object directory: /Fo takes a directory, not a
    # prefix, once more than one source file is on the command line.
    $netObj = Join-Path $build "net"
    $sprObj = Join-Path $build "sprite"
    $tileObj = Join-Path $build "tile"
    New-Item -ItemType Directory -Force $netObj, $sprObj, $tileObj | Out-Null

    $cmd = "cl $common /Fe:`"$build\netlink_tests.exe`" /Fo:`"$netObj\\`" `"$here\test_netlink.cpp`" $engine"
    cmd /c "`"$vcvars`" >nul 2>&1 && $cmd"
    if ($LASTEXITCODE -ne 0) { throw "compile failed" }
    Write-Host "== netlink =="
    & "$build\netlink_tests.exe"
    $netExit = $LASTEXITCODE

    # spritemath.cpp is split out of spritesystem.cpp so it pulls in no psyqo and
    # builds natively; the rendering around it still needs a PlayStation.
    $cmd = "cl $common /Fe:`"$build\sprite_tests.exe`" /Fo:`"$sprObj\\`" `"$here\test_sprite.cpp`" `"$srcDir\spritemath.cpp`""
    cmd /c "`"$vcvars`" >nul 2>&1 && $cmd"
    if ($LASTEXITCODE -ne 0) { throw "compile failed" }
    Write-Host "== sprite =="
    & "$build\sprite_tests.exe"
    $spriteExit = $LASTEXITCODE

    # tilemath.cpp, like spritemath.cpp, is split from its system file so it pulls
    # in no psyqo and builds natively; the tile rendering still needs a console.
    $cmd = "cl $common /Fe:`"$build\tile_tests.exe`" /Fo:`"$tileObj\\`" `"$here\test_tile.cpp`" `"$srcDir\tilemath.cpp`""
    cmd /c "`"$vcvars`" >nul 2>&1 && $cmd"
    if ($LASTEXITCODE -ne 0) { throw "compile failed" }
    Write-Host "== tile =="
    & "$build\tile_tests.exe"
    $tileExit = $LASTEXITCODE

    if ($netExit -ne 0 -or $spriteExit -ne 0 -or $tileExit -ne 0) { exit 1 }
    exit 0
}

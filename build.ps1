# ---------------------------------------------------------------------------
#  build.ps1 - build jubeat_quickskip.dll (32-bit) and the optional injector.
#
#  usage:
#    powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
#    powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Gxx "C:\mingw32\bin\i686-w64-mingw32-g++.exe"
#    powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Clean
#
#  requirements:
#    * i686 (32-bit) MinGW-w64 g++, GCC 8 or newer, on PATH or via -Gxx.
#      e.g. winlibs i686 build, or `sudo apt install g++-mingw-w64-i686` on Linux
#      (the binary is then called i686-w64-mingw32-g++).
#
#  outputs (git-ignored):
#    bin/jubeat_quickskip.dll   the mod
#    bin/injector.exe           optional: attach-injection helper (only needed
#                               if you cannot use spice2x's -k option)
#    bin/jubeat_quickskip.ini   default config (copied from config\ if missing)
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$Gxx = '',
    [switch]$Clean,
    [switch]$NoInjector
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$src  = Join-Path $root 'src'
$bin  = Join-Path $root 'bin'

function Resolve-Gxx {
    if ($Gxx -and (Test-Path $Gxx)) { return (Resolve-Path $Gxx).Path }
    foreach ($name in @('i686-w64-mingw32-g++.exe', 'g++.exe')) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) {
            $out = & $cmd.Source -dumpmachine 2>$null
            if ($out -match 'i686|i386|mingw32') { return $cmd.Source }
        }
    }
    throw "32-bit MinGW-w64 g++ not found. Install one or pass -Gxx <path\\to\\i686-w64-mingw32-g++.exe>."
}

$gxx = Resolve-Gxx
$env:PATH = (Split-Path $gxx) + ';' + $env:PATH
Write-Host "== toolchain =="
Write-Host "  g++   : $gxx"
Write-Host "  $(& $gxx --version | Select-Object -First 1)"
Write-Host "  target: $(& $gxx -dumpmachine)"

if ($Clean -and (Test-Path $bin)) {
    Remove-Item $bin -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $bin | Out-Null

# Common flags: static runtime so the output has no MinGW DLL dependencies.
$flags = @(
    '-std=c++17', '-O2', '-Wall', '-Wextra',
    '-Wno-cast-function-type', '-Wno-unused-parameter',
    '-DWIN32_LEAN_AND_MEAN', '-D_WIN32_WINNT=0x0601', '-DNOMINMAX',
    '-fno-strict-aliasing',
    '-static', '-static-libgcc', '-static-libstdc++',
    '-DHARNESS_PROBE',
    "-I$src", "-I$(Join-Path $src 'common')", "-I$(Join-Path $src 'probe')"
)

$modSources = @(
    'common\log.cpp', 'common\modules.cpp', 'common\pattern.cpp',
    'common\hook.cpp', 'common\iat.cpp', 'common\hotkey.cpp',
    'dllmain.cpp', 'worker.cpp', 'feature_quickskip.cpp'
) | ForEach-Object { Join-Path $src $_ }

Write-Host "== compile bin\jubeat_quickskip.dll =="
$dllArgs = $flags + @(
    '-shared',
    '-o', (Join-Path $bin 'jubeat_quickskip.dll'),
    '-Wl,--kill-at'
) + $modSources
& $gxx @dllArgs
if ($LASTEXITCODE -ne 0) { throw "dll compile failed, exit code $LASTEXITCODE" }

if (-not $NoInjector) {
    Write-Host "== compile bin\injector.exe (optional) =="
    $injArgs = $flags + @(
        '-o', (Join-Path $bin 'injector.exe'),
        (Join-Path $root 'tools\injector\injector.cpp')
    )
    & $gxx @injArgs
    if ($LASTEXITCODE -ne 0) { throw "injector compile failed, exit code $LASTEXITCODE" }
}

$iniSrc = Join-Path $root 'config\jubeat_quickskip.ini'
$iniDst = Join-Path $bin   'jubeat_quickskip.ini'
if ((Test-Path $iniSrc) -and -not (Test-Path $iniDst)) {
    Copy-Item $iniSrc $iniDst
    Write-Host "== copied config\jubeat_quickskip.ini -> bin\jubeat_quickskip.ini =="
}

# verify every output is a 32-bit PE (i386 / PE32)
function Assert-Pe32([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $e = [BitConverter]::ToInt32($bytes, 0x3c)
    $machine = [BitConverter]::ToUInt16($bytes, $e + 4)
    $magic = [BitConverter]::ToUInt16($bytes, $e + 24)
    if ($machine -ne 0x14c -or $magic -ne 0x10b) {
        throw ("{0}: unexpected PE (machine=0x{1:X4} magic=0x{2:X4}, need i386/PE32)" -f (Split-Path $Path -Leaf), $machine, $magic)
    }
    Write-Host ("  {0}: {1} bytes, i386/PE32 OK" -f (Split-Path $Path -Leaf), $bytes.Length)
}

Write-Host "== verify PE images =="
Assert-Pe32 (Join-Path $bin 'jubeat_quickskip.dll')
if (-not $NoInjector) { Assert-Pe32 (Join-Path $bin 'injector.exe') }
Write-Host "== done =="

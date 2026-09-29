param([string]$QtRoot = $env:QT_ROOT_DIR, [string]$OpenSslRoot = $env:OPENSSL_ROOT_DIR,
      [ValidateRange(1,64)][int]$Jobs = 4, [switch]$SkipPackage)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$build = Join-Path $repo 'build'
$cache = Join-Path $build 'CMakeCache.txt'
function Get-CacheValue($name) {
    if (Test-Path -LiteralPath $cache) {
        $line = Get-Content -LiteralPath $cache | Where-Object { $_ -match "^${name}:[^=]+=" } | Select-Object -First 1
        if ($line) { return ($line -split '=',2)[1] }
    }
}
function Invoke-Checked($program, [string[]]$arguments) {
    & $program @arguments
    if ($LASTEXITCODE -ne 0) { throw "$program failed ($LASTEXITCODE)" }
}
if (!$QtRoot) { $QtRoot = $env:QTDIR }
if (!$QtRoot) { $QtRoot = Get-CacheValue 'CMAKE_PREFIX_PATH' }
if (!$QtRoot) {
    $deploy = Get-Command windeployqt.exe -ErrorAction SilentlyContinue
    if ($deploy) { $QtRoot = Split-Path (Split-Path $deploy.Source -Parent) -Parent }
}
if (!$QtRoot -or !(Test-Path -LiteralPath (Join-Path $QtRoot 'bin\windeployqt.exe'))) {
    throw 'Set QT_ROOT_DIR to your Qt MSVC installation, or retain the existing CMake cache.'
}
if (!$OpenSslRoot) { $OpenSslRoot = Get-CacheValue 'OPENSSL_ROOT_DIR' }
if (!$OpenSslRoot) {
    $qtBase = Split-Path (Split-Path $QtRoot -Parent) -Parent
    $candidate = Join-Path $qtBase 'Tools\OpenSSLv3\Win_x64'
    if (Test-Path -LiteralPath $candidate) { $OpenSslRoot = $candidate }
}
if (!$OpenSslRoot -or !(Test-Path -LiteralPath (Join-Path $OpenSslRoot 'include\openssl\ssl.h'))) {
    throw 'Set OPENSSL_ROOT_DIR to your OpenSSL x64 development installation.'
}
$ninja = Get-Command ninja.exe -ErrorAction SilentlyContinue
if ($ninja) { $ninjaPath = $ninja.Source } else { $ninjaPath = Get-CacheValue 'CMAKE_MAKE_PROGRAM' }
if (!$ninjaPath -or !(Test-Path -LiteralPath $ninjaPath)) { throw 'Ninja was not found in PATH or the CMake cache.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere is required.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio with the C++ build tools was not found.' }
$devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
$environment = & $env:ComSpec /d /s /c "`"$devcmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment setup failed.' }
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') }
}
# Normalize SDK library paths before invoking the compiler.
$env:LIB = ($env:LIB -split ';' | Where-Object { $_ } | ForEach-Object { [IO.Path]::GetFullPath($_) }) -join ';'
$env:PATH = (Join-Path $QtRoot 'bin') + ';' + (Join-Path $OpenSslRoot 'bin') + ';' + $env:PATH
foreach ($command in @('cmake.exe','ctest.exe','node.exe','npm.cmd')) {
    if (!(Get-Command $command -ErrorAction SilentlyContinue)) { throw "$command must be available in PATH." }
}
Invoke-Checked cmake.exe @('-S',(Join-Path $repo 'src'),'-B',$build,'-G','Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninjaPath","-DCMAKE_PREFIX_PATH=$QtRoot","-DOPENSSL_ROOT_DIR=$OpenSslRoot",
    '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_POLICY_VERSION_MINIMUM=3.5','-DVERSION_PLATFORM=x64','-UVERSION','-UVERSION_DISPLAY','-Utest_result')
Invoke-Checked cmake.exe @('--build',$build,'--parallel',"$Jobs",'--target',
    'gui','cli','CrashReporter','lib-tests','gui-tests','cli-tests','crash-reporter-tests','Grabber_lrelease')
Invoke-Checked npm.cmd @('--prefix',(Join-Path $repo 'src\sites'),'run','check')
$oldQpa = $env:QT_QPA_PLATFORM
try {
    $env:QT_QPA_PLATFORM = 'offscreen'
    Invoke-Checked ctest.exe @('--test-dir',$build,'--output-on-failure','--timeout','120')
} finally {
    [Environment]::SetEnvironmentVariable('QT_QPA_PLATFORM',$oldQpa,'Process')
}
if (!$SkipPackage) {
    & (Join-Path $PSScriptRoot 'package-windows.ps1') -QtRoot $QtRoot -OpenSslRoot $OpenSslRoot -SkipArchive
}

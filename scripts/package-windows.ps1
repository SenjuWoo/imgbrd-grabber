param([string]$QtRoot = $env:QT_ROOT_DIR, [string]$OpenSslRoot = $env:OPENSSL_ROOT_DIR, [switch]$SkipArchive)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$build = Join-Path $repo 'build'
$stage = Join-Path $repo 'release.new'
$release = Join-Path $repo 'release'
function Get-CacheValue($name) {
    $line = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') | Where-Object { $_ -match "^${name}:[^=]+=" } | Select-Object -First 1
    if ($line) { return ($line -split '=',2)[1] }
}
function Copy-Tree($source, $target, [string[]]$exclusions = @()) {
    & robocopy.exe $source $target /E /NFL /NDL /NJH /NJS /NP @exclusions | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copy failed: $source ($LASTEXITCODE)" }
}
if (!$QtRoot) { $QtRoot = Get-CacheValue 'CMAKE_PREFIX_PATH' }
if (!$OpenSslRoot) { $OpenSslRoot = Get-CacheValue 'OPENSSL_ROOT_DIR' }
if (!$QtRoot -or !$OpenSslRoot) { throw 'Qt and OpenSSL paths are required.' }
if (Test-Path -LiteralPath $stage) { throw "Inspect/remove the previous incomplete package first: $stage" }
# Generated package paths are fixed children of this repository; never delete an app/profile here.
$repoPath = [IO.Path]::GetFullPath($repo).TrimEnd('\') + '\'
foreach ($path in @($stage,$release)) {
    if ([IO.Path]::GetFullPath($path) -ne ($repoPath + (Split-Path $path -Leaf))) { throw 'Invalid package path.' }
    if ((Test-Path -LiteralPath $path) -and (Get-Item -LiteralPath $path -Force).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) { throw "Refusing a package junction: $path" }
}
New-Item -ItemType Directory -Path $stage | Out-Null
foreach ($binary in @('gui\Grabber.exe','cli\Grabber-cli.exe','CrashReporter\CrashReporter.exe')) {
    Copy-Item -LiteralPath (Join-Path $build $binary) -Destination $stage
}
Copy-Tree (Join-Path $repo 'src\dist\common') $stage
Copy-Tree (Join-Path $repo 'src\dist\windows') $stage
$excludedDirs = Get-Content -LiteralPath (Join-Path $repo 'src\sites\exclude.txt') | Where-Object { $_ -and !$_.StartsWith('#') -and $_ -notmatch '[.*\\/]' }
# Retain compiled fork sources; discard only disabled templates without a runtime model.
$excludedDirs = @($excludedDirs | Where-Object {
    $_ -eq 'node_modules' -or !(Test-Path -LiteralPath (Join-Path (Join-Path $repo 'src\sites') ($_ + '\model.js')))
})
Copy-Tree (Join-Path $repo 'src\sites') (Join-Path $stage 'sites') (@('/XD') + $excludedDirs + @('/XF','*.ts','package*.json','*.config.js','tsconfig.json','tslint.json','exclude.txt','exclude_xcopy.txt','CMakeLists.txt'))
Get-ChildItem -LiteralPath (Join-Path $build 'languages') -File -Filter '*.qm' | Where-Object Name -ne 'YourLanguage.qm' | Copy-Item -Destination (Join-Path $stage 'languages')
$deploy = Join-Path $QtRoot 'bin\windeployqt.exe'
& $deploy --dir $stage --release --no-quick-import --no-opengl-sw --force-openssl (Join-Path $stage 'Grabber.exe') (Join-Path $stage 'Grabber-cli.exe') (Join-Path $stage 'CrashReporter.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE)" }
foreach ($pattern in @('libcrypto-3*.dll','libssl-3*.dll')) {
    $sslFiles = @(Get-ChildItem -LiteralPath $OpenSslRoot -File -Filter $pattern)
    if (!$sslFiles) { $sslFiles = @(Get-ChildItem -LiteralPath (Join-Path $OpenSslRoot 'bin') -File -Filter $pattern) }
    if (!$sslFiles) { throw "Missing OpenSSL runtime: $pattern" }
    $sslFiles | Copy-Item -Destination $stage
}
# These dependencies are optional and are only present when built/installed with Qt.
Get-ChildItem -LiteralPath (Join-Path $QtRoot 'lib') -File -Filter '*scintilla*qt6.dll' | Copy-Item -Destination $stage
if ($env:MYSQL_DRIVER_DIR) {
    Copy-Item -LiteralPath (Join-Path $env:MYSQL_DRIVER_DIR 'libmysql.dll') -Destination $stage
}
[IO.File]::WriteAllText((Join-Path $stage 'settings.ini'),'')
$translations = @(Get-ChildItem -LiteralPath (Join-Path $stage 'languages') -File -Filter '*.qm')
$expectedTranslations = @(Get-ChildItem -LiteralPath (Join-Path $repo 'src\languages') -File -Filter '*.ts' | Where-Object Name -ne 'YourLanguage.ts')
if ($translations.Count -ne $expectedTranslations.Count) { throw 'Missing compiled translations.' }
foreach ($source in Get-ChildItem -LiteralPath (Join-Path $stage 'sites') -Directory) {
    if ((Test-Path -LiteralPath (Join-Path $source.FullName 'sites.txt')) -and !(Test-Path -LiteralPath (Join-Path $source.FullName 'model.js'))) {
        throw "Missing source script: $($source.Name)"
    }
}
foreach ($required in @('platforms\qwindows.dll','tls\qopensslbackend.dll','sites\helper.js','Qt6Core.dll','sqldrivers\qsqlite.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $stage $required))) { throw "Missing runtime file: $required" }
}
$files = @(Get-ChildItem -LiteralPath $stage -File -Recurse | Where-Object { $_.FullName -ne (Join-Path $stage 'settings.ini') } | ForEach-Object {
    @{path=$_.FullName.Substring($stage.Length+1).Replace('\','/'); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$manifest = @{commit=(& git.exe -C $repo rev-parse HEAD); files=$files}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'package-manifest.json') -Encoding utf8
if (Test-Path -LiteralPath $release) { Remove-Item -LiteralPath $release -Recurse -Force }
Move-Item -LiteralPath $stage -Destination $release
if (!$SkipArchive) { Compress-Archive -Path (Join-Path $release '*') -DestinationPath (Join-Path $repo 'Grabber.zip') -Force }
Write-Output "Verified portable package: $release ($($translations.Count) translations)"

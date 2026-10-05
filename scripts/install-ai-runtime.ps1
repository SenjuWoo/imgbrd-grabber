param([Parameter(Mandatory=$true)][string]$Target, [string]$CacheDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$CacheDirectory) { $CacheDirectory = Join-Path $repo 'build\downloads' }
$version = '1.30.0'
$sha = 'c6ba983baf5681af108599675d2a89c2d145512d02de28aed0bff177cd0ba949'
$size = 82645522
$archive = Join-Path $CacheDirectory "onnxruntime-win-x64-$version.zip"
New-Item -ItemType Directory -Force -Path $CacheDirectory | Out-Null
if (!(Test-Path -LiteralPath $archive) -or (Get-Item -LiteralPath $archive).Length -ne $size -or (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sha) {
    $temporaryDownload = Join-Path $CacheDirectory ('ort-' + [Guid]::NewGuid().ToString('N') + '.download')
    try {
        Invoke-WebRequest -Uri "https://github.com/microsoft/onnxruntime/releases/download/v$version/onnxruntime-win-x64-$version.zip" -OutFile $temporaryDownload -UseBasicParsing
        if ((Get-Item -LiteralPath $temporaryDownload).Length -ne $size -or (Get-FileHash -LiteralPath $temporaryDownload -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sha) { throw 'ONNX Runtime archive failed publisher size/SHA-256 verification.' }
        Move-Item -LiteralPath $temporaryDownload -Destination $archive -Force
    } finally {
        if (Test-Path -LiteralPath $temporaryDownload) { Remove-Item -LiteralPath $temporaryDownload -Force }
    }
}
$cacheRoot = [IO.Path]::GetFullPath($CacheDirectory).TrimEnd('\') + '\'
$extract = Join-Path $CacheDirectory ('ort-extract-' + [Guid]::NewGuid().ToString('N'))
if (![IO.Path]::GetFullPath($extract).StartsWith($cacheRoot,[StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid extraction path.' }
try {
    Expand-Archive -LiteralPath $archive -DestinationPath $extract
    $runtime = @(Get-ChildItem -LiteralPath $extract -Recurse -File -Filter 'onnxruntime.dll')
    if ($runtime.Count -ne 1) { throw 'Expected exactly one native ONNX Runtime DLL.' }
    New-Item -ItemType Directory -Force -Path $Target | Out-Null
    Get-ChildItem -LiteralPath $runtime[0].Directory.FullName -File -Filter '*.dll' | Copy-Item -Destination $Target
    $licenses = Join-Path $Target 'licenses\onnxruntime'
    New-Item -ItemType Directory -Force -Path $licenses | Out-Null
    foreach ($name in @('LICENSE','ThirdPartyNotices.txt','Privacy.md','VERSION_NUMBER')) {
        Get-ChildItem -LiteralPath $extract -Recurse -File -Filter $name | Copy-Item -Destination $licenses
    }
    if (!(Test-Path -LiteralPath (Join-Path $licenses 'LICENSE'))) { throw 'Missing ONNX Runtime license.' }
    $provenance = @{version=$version; url="https://github.com/microsoft/onnxruntime/releases/tag/v$version"; archive_sha256=$sha; files=@(Get-ChildItem -LiteralPath $runtime[0].Directory.FullName -File -Filter '*.dll' | ForEach-Object { @{path=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()} })}
    $provenance | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $licenses 'provenance.json') -Encoding utf8
} finally {
    if ((Test-Path -LiteralPath $extract) -and !((Get-Item -LiteralPath $extract).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint))) { Remove-Item -LiteralPath $extract -Recurse -Force }
}
Write-Output "Verified ONNX Runtime $version deployed to $Target"

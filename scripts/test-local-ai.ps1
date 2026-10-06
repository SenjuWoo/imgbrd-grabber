param([Parameter(Mandatory=$true)][string]$Root, [Parameter(Mandatory=$true)][string]$TestDirectory, [string]$ModelFile)
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($Root)
$TestDirectory = [IO.Path]::GetFullPath($TestDirectory)
if (Test-Path -LiteralPath $TestDirectory) { throw 'Use a new private AI test directory; existing files will not be replaced.' }
New-Item -ItemType Directory -Path $TestDirectory | Out-Null
$portable = Join-Path $TestDirectory 'portable'
New-Item -ItemType Directory -Path $portable | Out-Null
$manifest = Get-Content -Raw -LiteralPath (Join-Path $Root 'package-manifest.json') | ConvertFrom-Json
foreach ($entry in $manifest.files) {
    $source = [IO.Path]::GetFullPath((Join-Path $Root $entry.path))
    $destination = [IO.Path]::GetFullPath((Join-Path $portable $entry.path))
    if (!$source.StartsWith($Root.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or !$destination.StartsWith($portable+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid manifest path.' }
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) { throw "Runtime integrity mismatch: $($entry.path)" }
    New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination
}
[IO.File]::WriteAllText((Join-Path $portable 'settings.ini'),'')
$expectedHash = 'b95448754a6ae56964ec80c570c80bb9863787ee85f51b664abed8c0e5f22a7a'
if (!$ModelFile) {
    $ModelFile = Join-Path $TestDirectory 'clip.onnx'
    Invoke-WebRequest -UseBasicParsing -Uri 'https://huggingface.co/Xenova/clip-vit-base-patch32/resolve/d15189d7028b43f1d3e65039190477f6af591c2a/onnx/vision_model_uint8.onnx' -OutFile $ModelFile
}
if ((Get-Item -LiteralPath $ModelFile).Length -ne 88648915 -or (Get-FileHash -LiteralPath $ModelFile -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) { throw 'Model integrity mismatch.' }
New-Item -ItemType Directory -Path (Join-Path $portable 'models') | Out-Null
$installedModel = Join-Path $portable 'models\clip-vit-base-patch32-uint8.onnx'
Copy-Item -LiteralPath $ModelFile -Destination $installedModel
$fixtures = Join-Path $TestDirectory 'pictures'
New-Item -ItemType Directory -Path $fixtures | Out-Null
Add-Type -AssemblyName System.Drawing
foreach ($variant in 0..2) {
    $bitmap = [Drawing.Bitmap]::new(320,240)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear(@([Drawing.Color]::DarkSeaGreen,[Drawing.Color]::MidnightBlue,[Drawing.Color]::Coral)[$variant])
        $brush = [Drawing.SolidBrush]::new(@([Drawing.Color]::ForestGreen,[Drawing.Color]::Gold,[Drawing.Color]::Ivory)[$variant])
        try { $graphics.FillEllipse($brush,40+20*$variant,20,180,180) } finally { $brush.Dispose() }
        $bitmap.Save((Join-Path $fixtures "$variant.png"),[Drawing.Imaging.ImageFormat]::Png)
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
$cli = Join-Path $portable 'Grabber-cli.exe'
$previousPath = $env:PATH
$previousPlugin = $env:QT_PLUGIN_PATH
$previousQpa = $env:QT_QPA_PLATFORM
try {
    $env:PATH = $portable+';'+(Join-Path $env:SystemRoot 'System32')
    $env:QT_PLUGIN_PATH = $portable
    $env:QT_QPA_PLATFORM = 'offscreen'
    $import = & $cli --import-library $fixtures | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or $import.total -ne 3) { throw 'Portable fixture import failed.' }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $first = & $cli --index-library | ConvertFrom-Json
    $firstExit = $LASTEXITCODE
    $timer.Stop()
    if ($firstExit -ne 0 -or $first.indexed -ne 3 -or $first.failed -ne 0 -or $first.skipped -ne 0) { throw "Actual CPU indexing failed: $($first | ConvertTo-Json -Compress)" }
    $cache = Join-Path $portable 'recommendations\index.json'
    $index = Get-Content -Raw -LiteralPath $cache | ConvertFrom-Json
    $entries = @($index.entries.PSObject.Properties)
    if ($entries.Count -ne 3) { throw 'Index coverage mismatch.' }
    foreach ($entry in $entries) {
        $vector = @($entry.Value.vector)
        if ($vector.Count -ne 512) { throw 'Wrong embedding size.' }
        $norm = 0.0
        foreach ($value in $vector) {
            if ([Double]::IsNaN($value) -or [Double]::IsInfinity($value)) { throw 'Nonfinite embedding.' }
            $norm += $value*$value
        }
        if ([Math]::Abs($norm-1) -gt 0.01) { throw 'Embedding not normalized.' }
    }
    if (($entries[0].Value.vector -join ',') -eq ($entries[1].Value.vector -join ',')) { throw 'Distinct images returned identical embeddings.' }
    $before = (Get-FileHash -LiteralPath $cache -Algorithm SHA256).Hash
    $second = & $cli --index-library | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or $second.reused -ne 3 -or $second.indexed -ne 0) { throw 'Index cache reuse failed.' }
    if ((Get-FileHash -LiteralPath $cache -Algorithm SHA256).Hash -ne $before) { throw 'Unchanged index was not deterministic.' }
    $stream = [IO.File]::Open($installedModel,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    try { $stream.WriteByte(0) } finally { $stream.Dispose() }
    $corrupt = & $cli --index-library | ConvertFrom-Json
    if ($LASTEXITCODE -eq 0 -or $corrupt.error -notmatch 'checksum') { throw 'Same-size corrupt model was accepted.' }
    if ((Get-FileHash -LiteralPath $cache -Algorithm SHA256).Hash -ne $before) { throw 'Model failure changed a completed index.' }
    $evidence = @{commit=$manifest.commit; model_sha256=$expectedHash; first=$first; elapsed_seconds=$timer.Elapsed.TotalSeconds; reuse=$second; corrupt_model=$corrupt; runtime_path=$portable}
    $evidence | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $TestDirectory 'evidence.json') -Encoding utf8
    $evidence | ConvertTo-Json -Depth 6
} finally {
    $env:PATH = $previousPath
    $env:QT_PLUGIN_PATH = $previousPlugin
    $env:QT_QPA_PLATFORM = $previousQpa
}
# The expected corrupt-model failure must not become the script's exit status.
exit 0

param([Parameter(Mandatory=$true)][string]$Root, [string]$Evidence,
      [string]$QtVersion, [string]$OpenSslVersion)
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($Root)
if (!(Test-Path -LiteralPath (Join-Path $Root 'package-manifest.json'))) { throw 'Extract the package before testing it.' }
$settingsPath = Join-Path $Root 'settings.ini'
$originalSettings = [IO.File]::ReadAllBytes($settingsPath)
$environment = @{}
foreach ($key in @('PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM')) { $environment[$key] = [Environment]::GetEnvironmentVariable($key,'Process') }
$process = $null
$fixture = $null
try {
    $env:PATH = $Root + ';' + $env:SystemRoot + '\System32;' + $env:SystemRoot
    $env:QT_PLUGIN_PATH = $Root
    [Environment]::SetEnvironmentVariable('QT_QPA_PLATFORM',$null,'Process')
    # Explicitly exercise the browser User-Agent preference in the headless CLI.
    [IO.File]::WriteAllText($settingsPath,"[General]`r`nuseQtUserAgent=true`r`n")
    $version = & (Join-Path $Root 'Grabber-cli.exe') --version
    if ($LASTEXITCODE -ne 0 -or $version -notmatch '^Grabber (\d+\.\d+\.\d+.*)$') { throw "Packaged CLI failed ($LASTEXITCODE)." }
    $appVersion = $matches[1].Trim()
    if (!(Test-Path -LiteralPath (Join-Path $Root 'library.sqlite'))) {
        $fixture = Join-Path $Root ('package-test-' + [guid]::NewGuid().ToString('N') + '.png')
        Add-Type -AssemblyName System.Drawing
        $pixels = [Drawing.Bitmap]::new(16,16)
        try { $pixels.Save($fixture,[Drawing.Imaging.ImageFormat]::Png) } finally { $pixels.Dispose() }
        foreach ($attempt in 1..2) {
            $imported = & (Join-Path $Root 'Grabber-cli.exe') --import-library $fixture
            if ($LASTEXITCODE -ne 0) { throw "Packaged Library import failed: $imported" }
            $report = $imported | ConvertFrom-Json
            if ($report.processed -ne 1 -or $report.failed -ne 0 -or $report.total -ne 1) { throw 'Library CLI import duplicated or lost its picture.' }
        }
    }
    # Test-only startup preferences; restore the exact original profile below.
    $settings = [Text.Encoding]::UTF8.GetString($originalSettings)
    foreach ($key in @('check_for_updates','send_usage_data','firstload','crashed','start','sites')) {
        $settings = [regex]::Replace($settings,'(?m)^' + $key + '=.*\r?$','')
    }
    $settings += "`r`n[General]`r`ncheck_for_updates=-1`r`nsend_usage_data=false`r`nfirstload=false`r`ncrashed=false`r`nstart=empty`r`nsites=`r`n"
    [IO.File]::WriteAllText($settingsPath,$settings,[Text.UTF8Encoding]::new($false))
    $previousLogs = @(Get-ChildItem -LiteralPath (Join-Path $Root 'logs') -File -Filter 'main_*.log' -ErrorAction SilentlyContinue | ForEach-Object FullName)
    $started = [DateTime]::UtcNow
    $process = Start-Process -FilePath (Join-Path $Root 'Grabber.exe') -ArgumentList '-d' -WorkingDirectory $Root -WindowStyle Hidden -PassThru
    $deadline = $started.AddSeconds(40)
    $log = ''
    while ([DateTime]::UtcNow -lt $deadline) {
        $process.Refresh()
        if ($process.HasExited) { throw "Packaged GUI exited during startup ($($process.ExitCode))." }
        $latest = Get-ChildItem -LiteralPath (Join-Path $Root 'logs') -File -Filter 'main_*.log' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notin $previousLogs } | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        if ($latest) { $log = Get-Content -LiteralPath $latest.FullName -Raw }
        if ($log.Contains('End of initialization')) { break }
        Start-Sleep -Milliseconds 250
    }
    if (!$log.Contains('End of initialization')) { throw ("Packaged desktop did not finish initialization. Window: " + $process.MainWindowTitle + ". Startup log:`n" + $log) }
    if ($log -notmatch ('Software version: ' + [regex]::Escape($appVersion) + '\.')) { throw 'CLI and desktop versions differ.' }
    if ($OpenSslVersion -and $log -notmatch ('SSL libraries: OpenSSL ' + [regex]::Escape($OpenSslVersion))) { throw 'Unexpected packaged TLS version.' }
    if ($log -match 'Missing SSL libraries|Cannot load library|Driver not loaded') { throw 'Missing runtime dependency.' }
    $module = $process.Modules | Where-Object ModuleName -eq 'Qt6Core.dll' | Select-Object -First 1
    if (!$module -or [IO.Path]::GetFullPath($module.FileName) -ne (Join-Path $Root 'Qt6Core.dll')) { throw 'Qt loaded outside the package.' }
    if ($QtVersion -and $module.FileVersionInfo.FileVersion -notlike ($QtVersion + '.*')) { throw 'Unexpected Qt version.' }
    $result = @{cli_version=$version; initialized=$true; qt=$module.FileVersionInfo.FileVersion; qt_path=$module.FileName; ssl=($log -split "`n" | Where-Object { $_ -match 'SSL libraries:' } | Select-Object -First 1).Trim(); title=$process.MainWindowTitle}
    $json = $result | ConvertTo-Json
    if ($Evidence) { $json | Set-Content -LiteralPath $Evidence -Encoding utf8 }
    $json
} finally {
    if ($process) {
        $process.Refresh()
        if (!$process.HasExited) {
            if ($process.CloseMainWindow()) { $null = $process.WaitForExit(5000) }
            $process.Refresh()
            if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
        }
    }
    if ($fixture -and (Test-Path -LiteralPath $fixture)) { Remove-Item -LiteralPath $fixture -Force }
    [IO.File]::WriteAllBytes($settingsPath,$originalSettings)
    foreach ($key in $environment.Keys) { [Environment]::SetEnvironmentVariable($key,$environment[$key],'Process') }
}

# Proves update-install.ps1 on a throwaway install: personal files survive byte for byte, the backup
# lands beside the install and stays small, old backups are pruned, and a broken package is rolled back.
#
# Usage:
#   pwsh -File scripts/test-update-install.ps1 -Package <release ZIP or unpacked package>

[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Package)

$ErrorActionPreference = 'Stop'
$updater = Join-Path $PSScriptRoot 'update-install.ps1'
$root = Join-Path ([IO.Path]::GetTempPath()) ('grabber-update-test-' + [Guid]::NewGuid().ToString('N'))
$install = Join-Path $root 'App'
$backups = $install + ' Backups'

function Assert($condition, [string]$message) {
	if (!$condition) { throw "FAILED: $message" }
	Write-Output "ok   $message"
}

try {
	# An "old version" install: personal files, a rebuilt folder with bulk in it, and a stale program file.
	New-Item -ItemType Directory -Path $install, (Join-Path $install 'cache'), (Join-Path $install 'models'), (Join-Path $install 'themes\Mine') -Force | Out-Null
	$personal = @{ 'settings.ini' = "[General]`nlanguage=fr`n"; 'tabs.json' = '{"tabs":[]}'; 'discover.json' = '{"version":3}'; 'themes\Mine\style.qss' = 'QWidget{}' }
	foreach ($name in $personal.Keys) { [IO.File]::WriteAllText((Join-Path $install $name), $personal[$name]) }
	[IO.File]::WriteAllBytes((Join-Path $install 'cache\thumb.bin'), (New-Object byte[] (2MB)))
	[IO.File]::WriteAllBytes((Join-Path $install 'models\model.onnx'), (New-Object byte[] (3MB)))
	[IO.File]::WriteAllText((Join-Path $install 'Grabber.exe'), 'old program file')
	$before = @{}
	foreach ($name in $personal.Keys) { $before[$name] = (Get-FileHash -LiteralPath (Join-Path $install $name) -Algorithm SHA256).Hash }

	# 1. A good package installs, and nothing personal changes.
	$output = & $updater -Package $Package -Install $install -KeepBackups 2
	Assert ($output -match 'Personal data verified unchanged') 'the update reports verified personal data'
	foreach ($name in $personal.Keys) {
		Assert ((Get-FileHash -LiteralPath (Join-Path $install $name) -Algorithm SHA256).Hash -eq $before[$name]) "personal file unchanged: $name"
	}
	Assert ((Get-Item -LiteralPath (Join-Path $install 'Grabber.exe')).Length -gt 100000) 'the program file was replaced by the package'
	Assert (Test-Path -LiteralPath (Join-Path $install 'portable')) 'the install stays portable'

	# 2. The backup is beside the install, holds the personal files, and none of the bulk.
	Assert (@(Get-ChildItem -LiteralPath $install -Directory -Filter 'backup-*').Count -eq 0) 'no backup folder inside the install'
	$made = @(Get-ChildItem -LiteralPath $backups -Directory -Filter 'update-*')
	Assert ($made.Count -eq 1) 'one backup beside the install'
	Assert (Test-Path -LiteralPath (Join-Path $made[0].FullName 'settings.ini')) 'the backup holds settings.ini'
	Assert (Test-Path -LiteralPath (Join-Path $made[0].FullName 'themes\Mine\style.qss')) 'the backup holds custom themes'
	Assert (!(Test-Path -LiteralPath (Join-Path $made[0].FullName 'cache'))) 'the cache is not copied'
	Assert (!(Test-Path -LiteralPath (Join-Path $made[0].FullName 'models'))) 'the AI model is not copied'
	Assert (!(Test-Path -LiteralPath (Join-Path $made[0].FullName 'app'))) 'replaced program files are dropped after success'
	$size = (Get-ChildItem -LiteralPath $made[0].FullName -Recurse -File | Measure-Object Length -Sum).Sum
	Assert ($size -lt 1MB) "the backup is small ($size bytes)"

	# 3. Only the newest backups are kept.
	foreach ($i in 1..3) { Start-Sleep -Milliseconds 1100; & $updater -Package $Package -Install $install -KeepBackups 2 | Out-Null }
	Assert (@(Get-ChildItem -LiteralPath $backups -Directory -Filter 'update-*').Count -eq 2) 'older backups are pruned to the number kept'

	# 4. A package whose program cannot start is rolled back.
	$broken = Join-Path $root 'broken'
	New-Item -ItemType Directory -Path $broken | Out-Null
	[IO.File]::WriteAllText((Join-Path $broken 'Grabber.exe'), 'not a program')
	[IO.File]::WriteAllText((Join-Path $broken 'Grabber-cli.exe'), 'not a program')
	$good = (Get-FileHash -LiteralPath (Join-Path $install 'Grabber-cli.exe') -Algorithm SHA256).Hash
	$failed = $false
	try { & $updater -Package $broken -Install $install -KeepBackups 2 | Out-Null } catch { $failed = $_.Exception.Message -match 'rolled back' }
	Assert $failed 'a broken package is refused with a rollback'
	Assert ((Get-FileHash -LiteralPath (Join-Path $install 'Grabber-cli.exe') -Algorithm SHA256).Hash -eq $good) 'the working program file is back after the rollback'
	foreach ($name in $personal.Keys) {
		Assert ((Get-FileHash -LiteralPath (Join-Path $install $name) -Algorithm SHA256).Hash -eq $before[$name]) "personal file unchanged after rollback: $name"
	}
	Write-Output 'All update-install checks passed.'
} finally {
	if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}

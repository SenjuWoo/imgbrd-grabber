# Updates an existing portable Grabber install from a release ZIP or an unpacked package,
# without ever touching personal data.
#
# Why this exists: a release package contains `defaults/settings.ini`, and an update done with a plain
# recursive copy can replace the live `settings.ini`. This script copies the package over the install
# while protecting the files that hold the user's library, settings and history, proves they are
# unchanged afterwards, and keeps a timestamped copy of them before it starts.
#
# Usage:
#   pwsh -File scripts/update-install.ps1 -Package "S:\Misc\out\Grabber-Woo-Edit-7.22.3-windows-x64.zip" -Install "S:\Misc\Grabber Woo Edit"

[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$Package,
	[Parameter(Mandatory = $true)][string]$Install
)

$ErrorActionPreference = 'Stop'

# Everything in here belongs to the person using the program, never to a package.
$personalFiles = @(
	'settings.ini', 'tabs.json', 'history.json', 'favorites.json', 'discover.json', 'stories.json',
	'library.sqlite', 'md5s.sqlite', 'story-key.dat', 'restore.igl',
	'blacklist.txt', 'ignore.txt', 'filenamehistory.txt', 'viewitlater.txt', 'words.txt', 'wordsc.txt',
	'position'
)
# Directories that are personal or rebuilt by the program.
$personalDirs = @('logs', 'cache', 'recommendations', 'themes', 'webservices', 'models')

if (!(Test-Path -LiteralPath $Install -PathType Container)) { throw "Install folder not found: $Install" }

# 0. Refuse to touch binaries that are in use: a running Grabber holds its DLLs and would leave a
#    half-replaced install behind.
if (@(Get-Process -Name 'Grabber', 'Grabber-cli' -ErrorAction SilentlyContinue).Count -gt 0) {
	throw "Grabber is running. Close it before updating: a half-replaced install is worse than an old one."
}

# Puts the install back the way it was. Wildcards are never passed to -LiteralPath: every entry in the
# backup folder is enumerated and copied by its own full path, and a failed restore throws.
function Restore-Install {
	param([string]$Backup)

	foreach ($entry in @(Get-ChildItem -LiteralPath $Backup -Force)) {
		if ($entry.Name -eq 'app') { continue }
		Copy-Item -LiteralPath $entry.FullName -Destination (Join-Path $Install $entry.Name) -Force -Recurse
	}
	$appBackup = Join-Path $Backup 'app'
	if (Test-Path -LiteralPath $appBackup) {
		Get-ChildItem -LiteralPath $appBackup -Recurse -File | ForEach-Object {
			$relative = $_.FullName.Substring($appBackup.Length).TrimStart('\', '/')
			$target = Join-Path $Install $relative
			$parent = Split-Path -Parent $target
			if (!(Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
			Copy-Item -LiteralPath $_.FullName -Destination $target -Force
		}
	}
	foreach ($relative in $script:addedPaths) {
		$path = Join-Path $Install $relative
		if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
	}
}

# 1. Unpack the package into a temporary folder when we were given an archive.
$root = Join-Path ([IO.Path]::GetTempPath()) ('grabber-update-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$work = $root
try {
	$source = $Package
	if (Test-Path -LiteralPath $Package -PathType Leaf) {
		Expand-Archive -LiteralPath $Package -DestinationPath $work -Force
		# A CI artifact is a ZIP inside a ZIP; keep unwrapping until a Grabber.exe shows up. The level
		# moves into a subfolder of $root, which the cleanup below removes as a whole.
		for ($i = 0; $i -lt 3 -and !(Test-Path -LiteralPath (Join-Path $work 'Grabber.exe')); $i++) {
			$inner = @(Get-ChildItem -LiteralPath $work -Recurse -File -Filter '*.zip' | Select-Object -First 1)
			if ($inner.Count -eq 0) { break }
			$next = Join-Path $root ('unpack-' + $i)
			Expand-Archive -LiteralPath $inner[0].FullName -DestinationPath $next -Force
			$work = $next
		}
		# An archive that wrapped a folder produces `<level>/<folder>/Grabber.exe`; accept that too.
		if (!(Test-Path -LiteralPath (Join-Path $work 'Grabber.exe'))) {
			$nested = @(Get-ChildItem -LiteralPath $work -Recurse -File -Filter 'Grabber.exe' | Select-Object -First 1)
			if ($nested.Count -gt 0) { $work = Split-Path -Parent $nested[0].FullName }
		}
		$source = $work
	}
	if (!(Test-Path -LiteralPath (Join-Path $source 'Grabber.exe'))) { throw "No Grabber.exe in the package: $source" }

	# 2. A package must never carry a root settings.ini: that is exactly how personal settings get lost.
	if (Test-Path -LiteralPath (Join-Path $source 'settings.ini')) {
		throw "Package contains a root settings.ini. Refusing to install it over personal data."
	}

	# 3. Copy personal files aside before touching anything.
	$stamp = Get-Date -Format 'yyyy-MM-dd-HHmmss'
	$backup = Join-Path $Install ("backup-" + $stamp)
	New-Item -ItemType Directory -Path $backup | Out-Null
	$before = @{}
	foreach ($name in $personalFiles) {
		$path = Join-Path $Install $name
		if (Test-Path -LiteralPath $path) {
			Copy-Item -LiteralPath $path -Destination (Join-Path $backup $name) -Force
			$before[$name] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
		}
	}
	foreach ($dir in $personalDirs) {
		$path = Join-Path $Install $dir
		if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination (Join-Path $backup $dir) -Recurse -Force }
	}

	# 4. Copy the package over the install, skipping every personal name and folder. Application files
	#    that are about to be replaced are kept first, so a failure can be undone.
	$skipFiles = @{}
	foreach ($name in $personalFiles) { $skipFiles[$name.ToLowerInvariant()] = $true }
	$skipDirs = @{}
	foreach ($dir in $personalDirs) { $skipDirs[$dir.ToLowerInvariant()] = $true }

	$appBackup = Join-Path $backup 'app'
	New-Item -ItemType Directory -Path $appBackup | Out-Null
	$script:addedPaths = @()
	$planned = @()
	Get-ChildItem -LiteralPath $source -Recurse -File | ForEach-Object {
		$relative = $_.FullName.Substring($source.Length).TrimStart('\', '/')
		$top = ($relative -split '[\\/]')[0].ToLowerInvariant()
		if ($skipFiles.ContainsKey($relative.ToLowerInvariant()) -or $skipDirs.ContainsKey($top)) { return }
		$planned += $relative
		$existing = Join-Path $Install $relative
		if (Test-Path -LiteralPath $existing -PathType Leaf) {
			$keep = Join-Path $appBackup $relative
			$keepParent = Split-Path -Parent $keep
			if (!(Test-Path -LiteralPath $keepParent)) { New-Item -ItemType Directory -Path $keepParent -Force | Out-Null }
			Copy-Item -LiteralPath $existing -Destination $keep -Force
		} else {
			$script:addedPaths += $relative
		}
	}

	$copied = 0
	try {
		foreach ($relative in $planned) {
			$target = Join-Path $Install $relative
			$parent = Split-Path -Parent $target
			if (!(Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
			Copy-Item -LiteralPath (Join-Path $source $relative) -Destination $target -Force
			$copied++
		}
	} catch {
		# A half-copied install is worse than an old one: put every replaced file back and say so.
		$reason = $_.Exception.Message
		Restore-Install -Backup $backup
		throw ("Copy step failed: " + $reason + ". The install was rolled back from " + $backup + ".")
	}

	# Portable marker: an update must keep the install portable.
	[IO.File]::WriteAllText((Join-Path $Install 'portable'), '')

	# 5. Ask the new binary its version before the hash check: starting the program can rewrite
	#    settings.ini, so the personal-file verification has to come after every operation.
	try {
		$version = (& (Join-Path $Install 'Grabber-cli.exe') -v | Out-String).Trim()
		if ($version -notmatch 'Grabber') { throw "unexpected version output: '$version'" }
	} catch {
		Restore-Install -Backup $backup
		throw "The new binaries did not report a version ($($_.Exception.Message)). The install was rolled back from $backup."
	}

	# 6. Prove nothing personal changed.
	$changed = @()
	foreach ($name in $before.Keys) {
		$path = Join-Path $Install $name
		if (!(Test-Path -LiteralPath $path)) { $changed += "$name (missing)"; continue }
		$after = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
		if ($after -ne $before[$name]) { $changed += $name }
	}
	if ($changed.Count -gt 0) {
		Restore-Install -Backup $backup
		throw ("Personal files changed during the update: " + ($changed -join ', ') + ". The install was rolled back from $backup.")
	}

	Write-Output "Updated $copied file(s). Personal data verified unchanged. Backup: $backup"
	Write-Output $version
} finally {
	if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}

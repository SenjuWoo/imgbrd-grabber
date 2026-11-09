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
	'blacklist.txt', 'ignore.txt', 'filenamehistory.txt', 'viewitlater.txt', 'words.txt', 'wordsc.txt'
)
# Directories that are personal or rebuilt by the program.
$personalDirs = @('logs', 'cache', 'recommendations')

if (!(Test-Path -LiteralPath $Install -PathType Container)) { throw "Install folder not found: $Install" }

# 0. Refuse to touch binaries that are in use: a running Grabber holds its DLLs and would leave a
#    half-replaced install behind.
if (@(Get-Process -Name 'Grabber', 'Grabber-cli' -ErrorAction SilentlyContinue).Count -gt 0) {
	throw "Grabber is running. Close it before updating: a half-replaced install is worse than an old one."
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

	# 4. Copy the package over the install, skipping every personal name and folder.
	$skipFiles = @{}
	foreach ($name in $personalFiles) { $skipFiles[$name.ToLowerInvariant()] = $true }
	$skipDirs = @{}
	foreach ($dir in $personalDirs) { $skipDirs[$dir.ToLowerInvariant()] = $true }

	$copied = 0
	try {
		Get-ChildItem -LiteralPath $source -Recurse -File | ForEach-Object {
			$relative = $_.FullName.Substring($source.Length).TrimStart('\', '/')
			$top = ($relative -split '[\\/]')[0].ToLowerInvariant()
			if ($skipFiles.ContainsKey($relative.ToLowerInvariant()) -or $skipDirs.ContainsKey($top)) { return }
			$target = Join-Path $Install $relative
			$parent = Split-Path -Parent $target
			if (!(Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
			Copy-Item -LiteralPath $_.FullName -Destination $target -Force
			$copied++
		}
	} catch {
		# A half-copied install is worse than an old one: put the personal files back and say so.
		Copy-Item -LiteralPath (Join-Path $backup '*') -Destination $Install -Force -ErrorAction SilentlyContinue
		throw "Copy step failed ($($_.Exception.Message)). Personal files were restored from $backup."
	}

	# Portable marker: an update must keep the install portable.
	[IO.File]::WriteAllText((Join-Path $Install 'portable'), '')

	# 5. Ask the new binary its version before the hash check: starting the program can rewrite
	#    settings.ini, so the personal-file verification has to come after every operation.
	$version = & (Join-Path $Install 'Grabber-cli.exe') -v

	# 6. Prove nothing personal changed.
	$changed = @()
	foreach ($name in $before.Keys) {
		$path = Join-Path $Install $name
		if (!(Test-Path -LiteralPath $path)) { $changed += "$name (missing)"; continue }
		$after = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
		if ($after -ne $before[$name]) { $changed += $name }
	}
	if ($changed.Count -gt 0) {
		Copy-Item -LiteralPath (Join-Path $backup '*') -Destination $Install -Force -ErrorAction SilentlyContinue
		throw ("Personal files changed during the update: " + ($changed -join ', ') + ". Restored from $backup.")
	}

	Write-Output "Updated $copied file(s). Personal data verified unchanged. Backup: $backup"
	Write-Output $version
} finally {
	if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}

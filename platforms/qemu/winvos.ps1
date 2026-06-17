Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Test-IsAdministrator {
	$currentIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
	$principal = New-Object Security.Principal.WindowsPrincipal($currentIdentity)
	return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-FreeDriveLetters {
	param(
		[int] $Count
	)

	$used = (Get-Volume | Where-Object { $_.DriveLetter } | ForEach-Object { $_.DriveLetter.ToString().ToUpperInvariant() })
	$available = @('S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z') | Where-Object { $_ -notin $used }
	if ($available.Count -lt $Count) {
		throw "Not enough free drive letters for VHDX staging."
	}

	return $available[0..($Count - 1)]
}

function Invoke-External {
	param(
		[string] $FilePath,
		[string[]] $Arguments
	)

	& $FilePath @Arguments
	if ($LASTEXITCODE -ne 0) {
		throw "$FilePath failed with exit code $LASTEXITCODE"
	}
}

function Get-IsoContentRoot {
	param(
		[string] $IsoPath,
		[string] $TempRoot
	)

	$isoMount = $null
	$contentRoot = $null
	$usingMount = $false

	try {
		$isoMount = Mount-DiskImage -ImagePath (Resolve-Path $IsoPath).Path -PassThru -ErrorAction Stop
		$isoDrive = ($isoMount | Get-Volume).DriveLetter
		if (-not $isoDrive) {
			throw "Could not resolve mounted ISO drive letter."
		}

		$contentRoot = "{0}:\" -f $isoDrive
		$usingMount = $true
	} catch {
		if ($isoMount) {
			Dismount-DiskImage -ImagePath $isoMount.ImagePath -ErrorAction SilentlyContinue
		}

		$extractRoot = Join-Path $TempRoot "ValidationOS_iso"
		if (Test-Path $extractRoot) {
			Remove-Item -Path $extractRoot -Recurse -Force
		}

		New-Item -Path $extractRoot -ItemType Directory -Force | Out-Null
		Write-Host "Mount-DiskImage unavailable. Extracting ISO to $extractRoot..."
		Invoke-External -FilePath "tar.exe" -Arguments @("-xf", $IsoPath, "-C", $extractRoot)

		if (-not (Test-Path (Join-Path $extractRoot "GenImage\GenImage.cmd"))) {
			throw "Failed to locate GenImage.cmd after ISO extraction at $extractRoot"
		}

		$contentRoot = $extractRoot
		$isoMount = $null
	}

	return @{
		ContentRoot = $contentRoot
		IsoMount = $isoMount
		UsingMount = $usingMount
	}
}

function Get-RepoRoot {
	param(
		[string] $StartPath
	)

	$repoRoot = $null
	try {
		$repoRoot = (& git -C $StartPath rev-parse --show-toplevel 2>$null | Select-Object -First 1)
	} catch {
		$repoRoot = $null
	}

	if ([string]::IsNullOrWhiteSpace($repoRoot)) {
		$repoRoot = Split-Path -Parent (Split-Path -Parent $StartPath)
	}

	return (Resolve-Path $repoRoot).Path
}

function Get-DriversFromList {
	param(
		[string] $DriverListPath,
		[string] $RepoRoot
	)

	if (-not (Test-Path $DriverListPath)) {
		throw "Missing driver list file: $DriverListPath"
	}

	$drivers = Get-Content -Path $DriverListPath |
		ForEach-Object { $_.Trim() } |
		Where-Object { $_ -and -not $_.StartsWith('#') }

	if (-not $drivers) {
		throw "No driver entries found in: $DriverListPath"
	}

	$driverPaths = New-Object System.Collections.Generic.List[string]

	foreach ($driver in $drivers) {
		$driverPath = if ([System.IO.Path]::IsPathRooted($driver)) {
			$driver
		} else {
			Join-Path $RepoRoot $driver
		}

		if (-not (Test-Path $driverPath)) {
			throw "Driver path from list does not exist: $driver"
		}

		$driverPaths.Add((Resolve-Path $driverPath).Path)
	}

	return ($driverPaths | Sort-Object -Unique)
}

function Get-DriverInfsFromList {
	param(
		[string] $DriverListPath,
		[string] $RepoRoot
	)

	$driverPaths = Get-DriversFromList -DriverListPath $DriverListPath -RepoRoot $RepoRoot
	$infPaths = New-Object System.Collections.Generic.List[string]

	foreach ($driverPath in $driverPaths) {
		$releaseInfs = Get-ChildItem -Path $driverPath -Recurse -Filter *.inf -File |
			Where-Object { $_.FullName -imatch '[\\/]arm64[\\/]release[\\/]' }

		if (-not $releaseInfs) {
			throw "No ARM64 Release INF files found under driver path: $driverPath"
		}

		foreach ($inf in $releaseInfs) {
			$infPaths.Add($inf.FullName)
		}
	}

	return ($infPaths | Sort-Object -Unique)
}

function Apply-DriversToOfflineImage {
	param(
		[string] $DriverListPath,
		[string] $RepoRoot,
		[string] $OfflineWindowsRoot
	)

	$infPaths = Get-DriverInfsFromList -DriverListPath $DriverListPath -RepoRoot $RepoRoot

	Write-Host "Applying drivers from $DriverListPath to offline image at $OfflineWindowsRoot"
	foreach ($infPath in $infPaths) {
		Write-Host "Adding driver $infPath"
		Invoke-External -FilePath "dism.exe" -Arguments @("/Image:$OfflineWindowsRoot", "/Add-Driver", "/Driver:$infPath")
	}
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Get-RepoRoot -StartPath $scriptDir
$driverList = Join-Path $scriptDir "driverlist.txt"
$isoPath = Join-Path $scriptDir "ValidationOS.iso"
$pkgList = Join-Path $scriptDir "armvirt_config.pkg"
$policyReg = Join-Path $scriptDir "armvirt_policy.reg"
$wimPath = Join-Path $scriptDir "ValidationOS.wim"
$vhdxPath = Join-Path $scriptDir "ValidationOS.vhdx"
$isAdministrator = Test-IsAdministrator

if (-not $isAdministrator) {
	throw "This script must be run from an elevated PowerShell session (Run as Administrator)."
}

if (-not (Test-Path $pkgList)) {
	throw "Missing package list: $pkgList"
}

if (-not (Test-Path $policyReg)) {
	throw "Missing registry policy file: $policyReg"
}

if (Test-Path $isoPath) {
	Write-Host "Validation OS ISO already present, skipping download."
} else {
	Write-Host "Downloading Validation OS ISO..."
	$ProgressPreference = 'SilentlyContinue'
	Invoke-WebRequest -Uri "https://aka.ms/DownloadValidationOS_arm64" -OutFile $isoPath
}

$isoMount = $null
$attachedVhdx = $false
$efiLetter = $null
$windowsLetter = $null
$isoStagingPath = $null

try {
	Write-Host "Preparing ISO content and running GenImage.cmd..."
	$isoContext = Get-IsoContentRoot -IsoPath $isoPath -TempRoot $scriptDir
	$isoMount = $isoContext.IsoMount
	$isoStagingPath = $isoContext.ContentRoot

	$genImageCmd = Join-Path $isoStagingPath "GenImage\GenImage.cmd"
	if (-not (Test-Path $genImageCmd)) {
		throw "GenImage.cmd not found at $genImageCmd"
	}
	$packagePath = Join-Path $isoStagingPath "cabs"

	$genImageArgs = @(
		"/c"
		"`"$genImageCmd`" -PackagesList:`"$pkgList`" -PackagePath:`"$packagePath`" -ImagePath:`"$isoStagingPath`" -RegistryImport:`"$policyReg`" -OutPath:`"$scriptDir`" -wim -NoWait"
	)
	Invoke-External -FilePath "cmd.exe" -Arguments $genImageArgs

	if (-not (Test-Path $wimPath)) {
		$generatedWim = Get-ChildItem -Path $scriptDir -Filter "*.wim" -File | Select-Object -First 1
		if (-not $generatedWim) {
			throw "No WIM was generated in $scriptDir"
		}
		$wimPath = $generatedWim.FullName
	}

	if (Test-Path $vhdxPath) {
		Remove-Item -Path $vhdxPath -Force
	}

	Write-Host "Creating and partitioning ValidationOS.vhdx..."
	$letters = Get-FreeDriveLetters -Count 2
	$efiLetter = $letters[0]
	$windowsLetter = $letters[1]

	$createScript = @"
create vdisk file="$vhdxPath" maximum=32768 type=expandable
select vdisk file="$vhdxPath"
attach vdisk
convert gpt
create partition efi size=100
format quick fs=fat32 label="SYSTEM"
assign letter=$efiLetter
create partition msr size=16
create partition primary
format quick fs=ntfs label="Windows"
assign letter=$windowsLetter
exit
"@

	$createScriptPath = Join-Path $env:TEMP ("diskpart-create-{0}.txt" -f [guid]::NewGuid().ToString())
	Set-Content -Path $createScriptPath -Value $createScript -Encoding ascii
	try {
		Invoke-External -FilePath "diskpart.exe" -Arguments @("/s", $createScriptPath)
	} finally {
		Remove-Item -Path $createScriptPath -Force -ErrorAction SilentlyContinue
	}
	$attachedVhdx = $true

	Write-Host "Applying WIM image to VHDX..."
	Invoke-External -FilePath "dism.exe" -Arguments @("/Apply-Image", "/ImageFile:$wimPath", "/Index:1", "/ApplyDir:${windowsLetter}:\")

	Write-Host "Creating UEFI boot files..."
	Invoke-External -FilePath "bcdboot.exe" -Arguments @("${windowsLetter}:\Windows", "/s", "${efiLetter}:", "/f", "UEFI")

	Apply-DriversToOfflineImage -DriverListPath $driverList -RepoRoot $repoRoot -OfflineWindowsRoot "${windowsLetter}:\"

	Write-Host "ValidationOS.vhdx generated at: $vhdxPath"
}
finally {
	if ($attachedVhdx) {
		$detachScript = @"
select vdisk file="$vhdxPath"
detach vdisk
exit
"@
		$detachScriptPath = Join-Path $env:TEMP ("diskpart-detach-{0}.txt" -f [guid]::NewGuid().ToString())
		Set-Content -Path $detachScriptPath -Value $detachScript -Encoding ascii
		try {
			& diskpart.exe /s $detachScriptPath | Out-Null
		} finally {
			Remove-Item -Path $detachScriptPath -Force -ErrorAction SilentlyContinue
		}
	}

	if ($isoMount) {
		Dismount-DiskImage -ImagePath $isoMount.ImagePath
	}

	if ($isoStagingPath -and -not $isoMount -and (Test-Path $isoStagingPath)) {
		Remove-Item -Path $isoStagingPath -Recurse -Force -ErrorAction SilentlyContinue
	}
}

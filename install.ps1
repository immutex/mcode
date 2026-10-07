# mcode installer for Windows.
#
#   irm https://raw.githubusercontent.com/immutex/mcode/master/install.ps1 | iex
#
# Downloads the release binary, verifies its SHA-256 against the release's
# SHA256SUMS, installs it, adds it to PATH, and hands off to `mcode setup`.
#
# Runs on the Windows PowerShell 5.1 that ships with Windows 10/11 as well as on
# PowerShell 7, so nothing here may use a 7-only operator or cmdlet.

[CmdletBinding()]
param(
	# Pin a version instead of resolving the latest release.
	[string]$Version,

	# Override the install directory.
	[string]$InstallDir,

	# Skip the interactive `mcode setup` hand-off.
	[switch]$NoSetup
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Repository = 'immutex/mcode'
$ReleaseBase = "https://github.com/$Repository/releases"

# ---------------------------------------------------------------------------
# Output. Colour is off when the host is redirected, so a captured log gets
# clean text, and off when NO_COLOR is set.
# ---------------------------------------------------------------------------

$script:UseColour = -not [Console]::IsOutputRedirected -and -not $env:NO_COLOR

function Write-Plain( [string]$Text ) {
	Write-Host $Text
}

function Write-Step( [string]$Text ) {
	if ( $script:UseColour ) {
		Write-Host '  ' -NoNewline
		Write-Host '::' -ForegroundColor DarkCyan -NoNewline
		Write-Host " $Text"
	} else {
		Write-Host "  :: $Text"
	}
}

function Write-Ok( [string]$Text ) {
	if ( $script:UseColour ) {
		Write-Host '  ' -NoNewline
		Write-Host 'ok' -ForegroundColor Green -NoNewline
		Write-Host " $Text"
	} else {
		Write-Host "  ok $Text"
	}
}

function Write-Note( [string]$Text ) {
	if ( $script:UseColour ) {
		Write-Host '  ' -NoNewline
		Write-Host '!!' -ForegroundColor Yellow -NoNewline
		Write-Host " $Text"
	} else {
		Write-Host "  !! $Text"
	}
}

function Write-Fail( [string]$Text ) {
	Write-Host ''
	if ( $script:UseColour ) {
		Write-Host '  error ' -ForegroundColor Red -NoNewline
		Write-Host $Text
	} else {
		Write-Host "  error $Text"
	}
	Write-Host ''
	throw $Text
}

function Write-Banner( [string]$Text ) {
	Write-Host ''
	if ( $script:UseColour ) {
		Write-Host '  mcode' -ForegroundColor Cyan -NoNewline
		Write-Host " $Text" -ForegroundColor DarkGray
	} else {
		Write-Host "  mcode $Text"
	}
	Write-Host ''
}

# ---------------------------------------------------------------------------
# Platform. Only Windows x64 is built; a 32-bit shell or an ARM64 device is
# refused by name rather than by a 404 on the archive.
# ---------------------------------------------------------------------------

function Get-Platform {
	$architecture = $env:PROCESSOR_ARCHITECTURE

	# A 32-bit PowerShell on 64-bit Windows reports x86 and hides the real CPU.
	if ( $architecture -eq 'x86' -and $env:PROCESSOR_ARCHITEW6432 ) {
		$architecture = $env:PROCESSOR_ARCHITEW6432
	}

	if ( $architecture -eq 'ARM64' ) {
		Write-Fail @"
unsupported architecture: ARM64
    mcode ships a Windows x64 build today.
    On an ARM64 device, run the x64 build under emulation by installing from an
    x64 PowerShell session.
"@
	}

	if ( $architecture -ne 'AMD64' ) {
		Write-Fail "unsupported architecture: $architecture`n    mcode ships a Windows x64 build."
	}

	return 'windows-x64'
}

function Resolve-Version {
	if ( $Version ) {
		return $Version.TrimStart( 'v' )
	}

	# Follow the `releases/latest` redirect rather than the JSON API, which is
	# rate-limited without a token.
	try {
		$response = Invoke-WebRequest -Uri "$ReleaseBase/latest" -MaximumRedirection 0 `
			-ErrorAction SilentlyContinue -UseBasicParsing
		$location = $response.Headers.Location
	} catch {
		# PowerShell 5.1 surfaces the 302 as an exception carrying the response.
		$location = $_.Exception.Response.Headers.Location
		if ( -not $location ) {
			$location = $_.Exception.Response.Headers['Location']
		}
	}

	if ( $location -is [array] ) {
		$location = $location[0]
	}

	if ( "$location" -match '/tag/v(.+)$' ) {
		return $Matches[1]
	}

	Write-Fail @"
could not determine the latest version
    Set -Version x.y.z to pin one, or check your network connection.
"@
}

function Get-Checksum {
	param( [string]$Path )

	return ( Get-FileHash -Path $Path -Algorithm SHA256 ).Hash.ToLowerInvariant()
}

# ---------------------------------------------------------------------------
# PATH. The user scope, so no elevation is needed and the change is reversible
# from the same Settings screen that shows it.
# ---------------------------------------------------------------------------

function Add-ToUserPath {
	param( [string]$Directory )

	$current = [Environment]::GetEnvironmentVariable( 'Path', 'User' )
	$entries = @()
	if ( $current ) {
		$entries = $current -split ';' | Where-Object { $_ }
	}

	foreach ( $entry in $entries ) {
		if ( $entry.TrimEnd( '\' ) -ieq $Directory.TrimEnd( '\' ) ) {
			Write-Ok "$Directory is already on PATH"
			return
		}
	}

	$updated = ( @( $entries ) + $Directory ) -join ';'
	[Environment]::SetEnvironmentVariable( 'Path', $updated, 'User' )

	# The current process does not see the registry change, so make it usable now.
	$env:Path = "$env:Path;$Directory"

	Write-Ok "added $Directory to the user PATH"
}

function Get-InstallDirectory {
	if ( $InstallDir ) {
		return $InstallDir
	}

	return ( Join-Path $env:LOCALAPPDATA 'mcode\bin' )
}

# ---------------------------------------------------------------------------
function Install-Mcode {
	Write-Banner 'installer'

	$platform = Get-Platform
	Write-Step "platform windows-x64"

	$resolved = Resolve-Version
	Write-Step "version v$resolved"

	$archive = "mcode-$resolved-$platform.zip"
	$url = "$ReleaseBase/download/v$resolved/$archive"
	$sumsUrl = "$ReleaseBase/download/v$resolved/SHA256SUMS"

	$work = Join-Path ([System.IO.Path]::GetTempPath( ) ) ( "mcode-" + [Guid]::NewGuid( ).ToString( 'N' ) )
	New-Item -ItemType Directory -Path $work -Force | Out-Null

	try {
		$archivePath = Join-Path $work $archive

		Write-Step "downloading $archive"
		try {
			Invoke-WebRequest -Uri $url -OutFile $archivePath -UseBasicParsing
		} catch {
			Write-Fail @"
download failed: $url
    Check the version exists: $ReleaseBase/tag/v$resolved
"@
		}

		# Verify before anything is expanded. A missing SHA256SUMS is a hard
		# failure, not a skipped check: an unverified binary is exactly what this
		# step exists to prevent.
		Write-Step 'verifying checksum'
		$sumsPath = Join-Path $work 'SHA256SUMS'
		try {
			Invoke-WebRequest -Uri $sumsUrl -OutFile $sumsPath -UseBasicParsing
		} catch {
			Write-Fail 'could not fetch SHA256SUMS, so the download cannot be verified'
		}

		$expected = $null
		foreach ( $line in ( Get-Content $sumsPath ) ) {
			if ( $line -match "\s\*?$([regex]::Escape( $archive ))$" ) {
				$expected = ( $line -split '\s+' )[0].ToLowerInvariant( )
				break
			}
		}

		if ( -not $expected ) {
			Write-Fail "SHA256SUMS has no entry for $archive"
		}

		$actual = Get-Checksum -Path $archivePath

		if ( $expected -ne $actual ) {
			Write-Fail @"
checksum mismatch for $archive
    expected $expected
    got      $actual
    The download is corrupt or tampered with; nothing was installed.
"@
		}

		Write-Ok "sha256 $actual"

		Write-Step 'unpacking'
		$extract = Join-Path $work 'extract'
		Expand-Archive -Path $archivePath -DestinationPath $extract -Force

		# The archive holds `mcode.exe` at its root. Search as a fallback so a
		# change to the packaging layout cannot silently produce "no binary".
		$binary = Join-Path $extract 'mcode.exe'
		if ( -not ( Test-Path $binary ) ) {
			$found = Get-ChildItem -Path $extract -Filter 'mcode.exe' -Recurse -File |
				Select-Object -First 1
			if ( $found ) {
				$binary = $found.FullName
			}
		}

		if ( -not ( Test-Path $binary ) ) {
			Write-Fail 'the archive does not contain mcode.exe'
		}

		$directory = Get-InstallDirectory
		New-Item -ItemType Directory -Path $directory -Force | Out-Null

		Write-Step "installing to $directory"
		$target = Join-Path $directory 'mcode.exe'

		# A running mcode holds its own image open; moving over it fails cleanly
		# rather than leaving a half-written executable.
		Copy-Item -Path $binary -Destination $target -Force
		Write-Ok "installed $target"

		Add-ToUserPath -Directory $directory

		$versionOutput = & $target --version 2>$null
		Write-Ok "runs: $versionOutput"

		Write-Host ''
		if ( $script:UseColour ) {
			Write-Host '  installed' -ForegroundColor Green
		} else {
			Write-Host '  installed'
		}
		Write-Host ''

		# The wizard lives in the binary, so the interactive part is identical on
		# every platform and in the packaged build.
		if ( $NoSetup ) {
			Write-Plain '  Next: run mcode setup to choose a model and provider.'
			Write-Host ''
			return
		}

		$interactive = -not [Console]::IsInputRedirected -and -not [Console]::IsOutputRedirected
		if ( $interactive ) {
			& $target setup
		} else {
			Write-Plain '  Next: run mcode setup to choose a model and provider.'
			Write-Host ''
		}
	} finally {
		Remove-Item -Path $work -Recurse -Force -ErrorAction SilentlyContinue
	}
}

Install-Mcode

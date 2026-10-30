<#
.SYNOPSIS
  Install Wolang on Windows.

.DESCRIPTION
  irm https://raw.githubusercontent.com/MowoGames/Wolang/alpha/install.ps1 | iex

  Works out which build this machine wants, downloads it from the
  releases, checks it against the published checksum, unpacks it under
  %LOCALAPPDATA%\Wolang, and puts that on the PATH for this user.
  Nothing is written outside the prefix and nothing needs
  administrator (D185).

.PARAMETER Version
  A particular release, rather than the newest.

.PARAMETER Prefix
  Somewhere other than %LOCALAPPDATA%\Wolang.

.PARAMETER FromSource
  Clone and build instead of downloading. Needs clang-cl, CMake and
  Ninja.

.PARAMETER Uninstall
  Take it away again.

.PARAMETER NoModifyPath
  Do not touch the PATH.

.PARAMETER NoVerify
  Install without checking SHA256SUMS (not advised).
#>
[CmdletBinding()]
param(
  [string]$Version = $env:WOLANG_VERSION,
  [string]$Prefix = $env:WOLANG_HOME,
  [switch]$FromSource,
  [switch]$Uninstall,
  [switch]$NoModifyPath,
  [switch]$NoVerify
)

$ErrorActionPreference = 'Stop'
$repo = 'MowoGames/Wolang'

if (-not $Prefix) {
  $Prefix = Join-Path $env:LOCALAPPDATA 'Wolang'
}
if (-not $Version) {
  $Version = 'latest'
}

function Say($message) { Write-Host $message }
function Die($message) {
  Write-Host "wolang: $message" -ForegroundColor Red
  exit 1
}

if ($Uninstall) {
  if (-not (Test-Path $Prefix)) {
    Die "nothing installed at $Prefix"
  }
  Remove-Item -Recurse -Force $Prefix
  Say "removed $Prefix"
  Say 'the PATH entry it added is still there; take it out in System > Environment Variables'
  exit 0
}

# What this machine is. Windows on arm64 runs x86-64 binaries through
# emulation, so that build is the honest fallback until there is a
# native one.
$arch = switch ($env:PROCESSOR_ARCHITECTURE) {
  'AMD64' { 'x86_64' }
  'ARM64' { 'x86_64' }
  default { 'x86_64' }
}
$target = "wolang-windows-$arch"

if ($FromSource) {
  foreach ($tool in @('git', 'cmake', 'ninja', 'clang-cl')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
      Die "this needs $tool, which is not on the PATH. See docs/building.md"
    }
  }
  $work = Join-Path $env:TEMP ("wolang-" + [guid]::NewGuid())
  New-Item -ItemType Directory -Path $work | Out-Null
  Say "cloning into $work"
  git clone --depth 1 "https://github.com/$repo" "$work\wolang"
  Push-Location "$work\wolang"
  cmake --preset windows -DCMAKE_BUILD_TYPE=Release
  cmake --build --preset windows -j
  cmake --install build/windows --prefix $Prefix
  Pop-Location
  Say "built and installed into $Prefix"
} else {
  if ($Version -eq 'latest') {
    Say 'looking up the newest release'
    $Version = ''
    # The API first; it is rate limited, so when it refuses, read where
    # /releases/latest redirects to, which is not.
    try {
      $release = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest"
      $Version = $release.tag_name
    } catch {
      # A redirect is an error when redirects are refused; the answer is
      # in the exception. Windows PowerShell and PowerShell 7 spell it
      # differently, so ask both ways.
      try {
        Invoke-WebRequest "https://github.com/$repo/releases/latest" -MaximumRedirection 0 -UseBasicParsing | Out-Null
      } catch {
        $headers = $_.Exception.Response.Headers
        $location = [string]$headers.Location
        if (-not $location) {
          $location = [string]$headers['Location']
        }
        if ($location -match '/releases/tag/(.+)$') {
          $Version = $Matches[1]
        }
      }
    }
    if (-not $Version) {
      Die 'could not find a release; try -FromSource'
    }
  }
  # v1.1.0 and 1.1.0 are the same request.
  $Version = $Version -replace '^v', ''
  if ($Version -notmatch '^\d+\.\d+\.\d+') {
    Die "'$Version' is not a version like 1.1.0"
  }

  # WOLANG_DOWNLOAD_BASE is for tools/package/test_install.sh and the
  # release smoke test only (D248).
  $base = $env:WOLANG_DOWNLOAD_BASE
  if (-not $base) {
    $base = "https://github.com/$repo/releases/download/v$Version"
  }
  $archive = "$target.zip"
  $work = Join-Path $env:TEMP ("wolang-" + [guid]::NewGuid())
  New-Item -ItemType Directory -Path $work | Out-Null

  Say "downloading $archive ($Version)"
  try {
    Invoke-WebRequest "$base/$archive" -OutFile "$work\$archive"
  } catch {
    Die "no build for $target in v$Version; try -FromSource"
  }

  # A download nobody checked is a download somebody else could have
  # written, so a missing checksum is a failure, not a shrug.
  if ($NoVerify) {
    Say 'skipping the checksum (-NoVerify)'
  } else {
    try {
      Invoke-WebRequest "$base/SHA256SUMS" -OutFile "$work\SHA256SUMS"
    } catch {
      Die "v$Version published no SHA256SUMS; refusing to install (-NoVerify skips the check)"
    }
    $line = Get-Content "$work\SHA256SUMS" |
            Where-Object { $_ -match ('^[0-9a-fA-F]{64}\s+\*?(\./)?' + [regex]::Escape($archive) + '$') } |
            Select-Object -First 1
    if (-not $line) {
      Die "SHA256SUMS has no line for $archive; refusing to install (-NoVerify skips the check)"
    }
    $expected = ($line -split '\s+')[0]
    $actual = (Get-FileHash "$work\$archive" -Algorithm SHA256).Hash
    if ($actual -ne $expected.ToUpper()) {
      Die 'the checksum does not match; refusing to install'
    }
    Say 'checksum matches'
  }

  # The archive's one top-level directory is named after the platform
  # today; read it rather than assume.
  Expand-Archive "$work\$archive" -DestinationPath "$work\x" -Force
  $top = Get-ChildItem "$work\x" -Directory | Select-Object -First 1
  if (-not $top -or -not (Test-Path (Join-Path $top.FullName 'bin\wolang.exe'))) {
    Die 'the archive does not have bin\wolang.exe in its top directory'
  }
  $current = Join-Path $Prefix 'current'
  if (Test-Path $current) {
    Remove-Item -Recurse -Force $current
  }
  New-Item -ItemType Directory -Path $Prefix -Force | Out-Null
  Move-Item $top.FullName $current
  Remove-Item -Recurse -Force $work
  Say "installed $Version into $Prefix"
}

$bin = Join-Path (Join-Path $Prefix 'current') 'bin'
if (-not (Test-Path $bin)) {
  $bin = Join-Path $Prefix 'bin'
}

if (-not $NoModifyPath) {
  $user = [Environment]::GetEnvironmentVariable('Path', 'User')
  if ($user -notlike "*$bin*") {
    [Environment]::SetEnvironmentVariable('Path', "$bin;$user", 'User')
    Say "added $bin to your PATH; open a new terminal for it to take"
  }
  $env:Path = "$bin;$env:Path"
}

Say ''
$wolang = Join-Path $bin 'wolang.exe'
if (Test-Path $wolang) {
  & $wolang --version
}
Say ''
Say 'Try it:'
Say '  wolang new hello; cd hello; wolang run .'

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
#>
[CmdletBinding()]
param(
  [string]$Version = $env:WOLANG_VERSION,
  [string]$Prefix = $env:WOLANG_HOME,
  [switch]$FromSource,
  [switch]$Uninstall,
  [switch]$NoModifyPath
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
    $release = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest"
    $Version = $release.tag_name -replace '^v', ''
    if (-not $Version) {
      Die 'could not find a release; try -FromSource'
    }
  }

  $base = "https://github.com/$repo/releases/download/v$Version"
  $archive = "$target.zip"
  $work = Join-Path $env:TEMP ("wolang-" + [guid]::NewGuid())
  New-Item -ItemType Directory -Path $work | Out-Null

  Say "downloading $archive ($Version)"
  try {
    Invoke-WebRequest "$base/$archive" -OutFile "$work\$archive"
  } catch {
    Die "no build for $target in v$Version; try -FromSource"
  }

  # The checksum, when the release published one: a download nobody
  # checked is a download somebody else could have written.
  try {
    Invoke-WebRequest "$base/SHA256SUMS" -OutFile "$work\SHA256SUMS"
    $line = Select-String -Path "$work\SHA256SUMS" -Pattern ([regex]::Escape($archive)) |
            Select-Object -First 1
    if ($line) {
      $expected = ($line.Line -split '\s+')[0]
      $actual = (Get-FileHash "$work\$archive" -Algorithm SHA256).Hash.ToLower()
      if ($actual -ne $expected.ToLower()) {
        Die 'the checksum does not match; refusing to install'
      }
      Say 'checksum matches'
    }
  } catch {
    Say 'no checksums published for this release; continuing'
  }

  Expand-Archive "$work\$archive" -DestinationPath $work -Force
  $current = Join-Path $Prefix 'current'
  if (Test-Path $current) {
    Remove-Item -Recurse -Force $current
  }
  New-Item -ItemType Directory -Path $Prefix -Force | Out-Null
  Move-Item "$work\$target" $current
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

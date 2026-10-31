<#
.SYNOPSIS
  Builds the Windows installer.

.DESCRIPTION
  Assembles what `cmake --install` lays out into a payload directory,
  turns the licence into the RTF the installer shows, draws the icon,
  and runs WiX over tools/package/wolang.wxs (D188).

      python3 ... is not needed; this is the whole thing:
      pwsh tools/package/make_msi.ps1 -Build build/windows

  WiX 5 comes from dotnet:

      dotnet tool install --global wix --version 6.0.2

.PARAMETER Build
  The build directory to install from. Default build\windows.

.PARAMETER Version
  What the installer calls itself. Default is read from CMakeLists.

.PARAMETER Out
  Where the .msi goes. Default build.
#>
[CmdletBinding()]
param(
  [string]$Build = "build\windows",
  [string]$Version,
  [string]$Out = "build"
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path

if (-not $Version) {
  $line = Select-String -Path "$root\CMakeLists.txt" -Pattern 'VERSION\s+(\d+\.\d+\.\d+)' |
          Select-Object -First 1
  $Version = if ($line) { $line.Matches[0].Groups[1].Value } else { '0.1.0' }
}

if (-not (Get-Command wix -ErrorAction SilentlyContinue)) {
  Write-Host 'wix is not on the PATH. Install it with:' -ForegroundColor Yellow
  Write-Host '  dotnet tool install --global wix --version 6.0.2'
  exit 1
}

$payload = Join-Path $root 'build\msi-payload'
if (Test-Path $payload) {
  Remove-Item -Recurse -Force $payload
}

Write-Host "installing from $Build into the payload"
cmake --install (Join-Path $root $Build) --prefix $payload

# The three things the installer itself shows: a licence in the format
# it can render, an icon, and the readme.
$licence = Get-Content (Join-Path $root 'LICENSE') -Raw
$escaped = $licence -replace '\\', '\\\\' -replace '([{}])', '\$1' `
                    -replace "`r`n", "\par`r`n" -replace "`n", "\par`n"
@"
{\rtf1\ansi\deff0{\fonttbl{\f0\fnil\fcharset0 Segoe UI;}}
\f0\fs18
$escaped
}
"@ | Set-Content -Path (Join-Path $payload 'LICENSE.rtf') -Encoding ASCII

Copy-Item (Join-Path $root 'README.md') $payload -Force
if (Test-Path (Join-Path $root 'assets\wolang.ico')) {
  Copy-Item (Join-Path $root 'assets\wolang.ico') $payload -Force
} else {
  # The icon is drawn from the same shapes as everything else (D174).
  python3 (Join-Path $root 'assets\make_art.py')
  Copy-Item (Join-Path $root 'assets\wolang.ico') $payload -Force
}

New-Item -ItemType Directory -Force -Path (Join-Path $root $Out) | Out-Null
$msi = Join-Path $root "$Out\wolang-$Version-x86_64.msi"

Write-Host "building $msi"
wix build (Join-Path $root 'tools\package\wolang.wxs') `
  -arch x64 `
  -ext WixToolset.UI.wixext `
  -d Version=$Version `
  -d Payload=$payload `
  -o $msi

Write-Host ""
Write-Host "wrote $msi"
Write-Host "install it with: msiexec /i `"$msi`""

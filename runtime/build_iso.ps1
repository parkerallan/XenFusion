# Build the Xbox 360 game .xex, deploy the current project's content, then pack it
# into an Xbox 360 disc image (.iso). Xbox 360 discs are NOT standard ISO-9660/UDF
# - they use Microsoft's XDVDFS / XGD layout, so the image is produced by the XDK's
# XDiscBld.exe from a small .xgd layout file (not mkisofs). The resulting .iso can
# be mounted directly in Xenia (or burned for a real console). Invoked by the
# editor's "Build .iso Image" menu item; also runnable by hand:
#
#   powershell -ExecutionPolicy Bypass -File build_iso.ps1 -Project <dir> [-Xedk <xdk-root>] [-OutDir <dir>] [-IsoName <name>] [-Config Release|Debug]

param(
    [Parameter(Mandatory=$true)][string]$Project,
    [string]$Xedk    = "",
    [string]$OutDir  = "",
    [string]$IsoName = "",
    [string]$Config  = "Release"
)

$root    = $PSScriptRoot
$msbuild = "C:\Windows\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe"
# Where build artifacts + the .iso go (empty -OutDir = the runtime\ dir).
$base    = if ($OutDir) { $OutDir } else { $root }

if (-not (Test-Path $msbuild)) { Write-Output "ERROR: MSBuild v4.0 not found ($msbuild)"; exit 1 }
if (-not (Test-Path $Project)) { Write-Output "ERROR: project not found ($Project)"; exit 1 }

# Resolve the XDK (same rule as build_run/deploy): -Xedk wins, then process env,
# then the persisted Machine value. XDiscBld.exe lives under it.
if (-not $Xedk) {
    $Xedk = if ($env:XEDK) { $env:XEDK } else { [Environment]::GetEnvironmentVariable('XEDK','Machine') }
}
if (-not $Xedk -or -not (Test-Path $Xedk)) { Write-Output "ERROR: XDK not found (set the XDK path in Settings > Toolchain)"; exit 1 }
$env:XEDK = $Xedk
$xdiscbld = Join-Path $Xedk "bin\win32\XDiscBld.exe"
if (-not (Test-Path $xdiscbld)) { Write-Output "ERROR: XDiscBld.exe not found under the XDK ($xdiscbld)"; exit 1 }

Write-Output "[1/4] Building game360.xex ($Config, Xbox 360)..."
# Redirect the .xex + obj into the output base (trailing '\\' survives arg quoting).
$msbProps = @("/p:Configuration=$Config", "/p:Platform=Xbox 360", "/v:minimal", "/nologo")
if ($OutDir) {
    $cfgDir = Join-Path $base $Config
    New-Item -ItemType Directory -Force $cfgDir | Out-Null
    $msbProps += "/p:OutDir=$cfgDir\\"
    $msbProps += "/p:IntDir=$cfgDir\obj\\"
}
& $msbuild (Join-Path $root "game360.vcxproj") @msbProps
if ($LASTEXITCODE -ne 0) { Write-Output "ERROR: build FAILED"; exit 1 }

Write-Output "[2/4] Deploying content from $Project..."
try { & (Join-Path $root "deploy.ps1") -Project $Project -Config $Config -Xedk $Xedk -OutDir $OutDir }
catch { Write-Output "ERROR: deploy FAILED - $_"; exit 1 }

$deploy = Join-Path $base "deploy"
if (-not (Test-Path (Join-Path $deploy "default.xex"))) { Write-Output "ERROR: deploy\default.xex missing - nothing to pack"; exit 1 }

# Write the XGD (v2) disc layout: add the whole deploy folder to the disc root.
# XDiscBld reads a UTF-16 XML layout; SOURCE is the folder whose contents land at
# DEST on the disc. default.xex ends up at \default.xex, which Xenia boots.
Write-Output "[3/4] Writing disc layout (.xgd)..."
$xgd = Join-Path $base "game.xgd"
$deployEsc = ($deploy.TrimEnd('\')) + '\'
$layout = @"
<LAYOUT MAJORVERSION="2" MINORVERSION="2">
    <AVATARASSETPACK INCLUDE="NO"/>
    <DISC NAME="" LayoutType="XGD2">
        <ADD NAME="" SOURCE="$deployEsc" DEST="\" FILESPEC="*.*" RECURSE="YES" LAYER="ANY" ALIGN="1"/>
    </DISC>
</LAYOUT>
"@
Set-Content -Path $xgd -Value $layout -Encoding Unicode

# ISO filename: -IsoName if given (append .iso if the user omitted it), else the
# project folder name.
$isoName = if ($IsoName) { $IsoName } else { (Split-Path $Project -Leaf) + ".iso" }
if ($isoName -notmatch '\.iso$') { $isoName += ".iso" }
$iso     = Join-Path $base $isoName

Write-Output "[4/4] Packing $isoName with XDiscBld (XDVDFS/XGD2)..."
if (Test-Path $iso) { Remove-Item $iso -Force }   # so the produced-file check below is meaningful
& $xdiscbld $xgd $iso /nologo

# XDiscBld returns a non-zero exit for submission-only warnings (missing NxeArt
# dashboard art / Xdb debug database) even when it writes a perfectly valid image,
# so trust the produced file rather than the exit code.
if (-not (Test-Path $iso)) { Write-Output "ERROR: XDiscBld did not produce $isoName (exit $LASTEXITCODE)"; exit 1 }
if ($LASTEXITCODE -ne 0) {
    Write-Output "Note: XDiscBld's 'NxeArt'/'Xdb' warnings above are submission-only (dashboard art / debug database needed to submit to Microsoft) and do not affect booting in Xenia - the image is valid."
}

$sizeMB = [math]::Round((Get-Item $iso).Length / 1MB, 1)
Write-Output "OK: built $isoName ($sizeMB MB) at $iso"
Write-Output "Load it in Xenia:  <xenia.exe> `"$iso`""

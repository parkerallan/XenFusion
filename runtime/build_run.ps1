# Build the Xbox 360 game .xex, deploy the current project's content, and launch
# it in Xenia. Invoked by the editor's "Build and Run XEX" menu item (output is
# tailed into the editor Log panel), but also runnable by hand:
#
#   powershell -ExecutionPolicy Bypass -File build_run.ps1 -Project <dir> -Xenia <dir-or-exe> [-Xedk <xdk-root>] [-OutDir <dir>]

param(
    [Parameter(Mandatory=$true)][string]$Project,
    [Parameter(Mandatory=$true)][string]$Xenia,
    [string]$Xedk   = "",
    [string]$OutDir = "",
    [string]$Config = "Release"
)

$root    = $PSScriptRoot
$msbuild = "C:\Windows\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe"
# Where build artifacts go (empty -OutDir = the runtime\ dir, i.e. legacy behavior).
$base    = if ($OutDir) { $OutDir } else { $root }

if (-not (Test-Path $msbuild)) { Write-Output "ERROR: MSBuild v4.0 not found ($msbuild)"; exit 1 }
if (-not (Test-Path $Project)) { Write-Output "ERROR: project not found ($Project)"; exit 1 }

# The XDK root (from the editor's Settings) becomes XEDK for this build: MSBuild's
# Xbox 360 platform and deploy's fxc both resolve the SDK from it. Set it in this
# process env so the spawned MSBuild inherits it; if not given, the machine-wide
# XEDK is used as before.
if ($Xedk) {
    if (-not (Test-Path $Xedk)) { Write-Output "ERROR: XDK path not found ($Xedk)"; exit 1 }
    $env:XEDK = $Xedk
    Write-Output "Using XDK: $Xedk"
}

Write-Output "[1/3] Building game360.xex ($Config, Xbox 360)..."
# Redirect the .xex + obj into the output base (trailing '\\' survives arg quoting
# when the path has spaces). Empty -OutDir leaves MSBuild's runtime\ defaults.
$msbProps = @("/p:Configuration=$Config", "/p:Platform=Xbox 360", "/v:minimal", "/nologo")
if ($OutDir) {
    $cfgDir = Join-Path $base $Config
    New-Item -ItemType Directory -Force $cfgDir | Out-Null
    $msbProps += "/p:OutDir=$cfgDir\\"
    $msbProps += "/p:IntDir=$cfgDir\obj\\"
}
& $msbuild (Join-Path $root "game360.vcxproj") @msbProps
if ($LASTEXITCODE -ne 0) { Write-Output "ERROR: build FAILED"; exit 1 }

Write-Output "[2/3] Deploying content from $Project..."
try { & (Join-Path $root "deploy.ps1") -Project $Project -Config $Config -Xedk $Xedk -OutDir $OutDir }
catch { Write-Output "ERROR: deploy FAILED - $_"; exit 1 }

Write-Output "[3/3] Launching Xenia..."
$xe = $Xenia
if (Test-Path $xe -PathType Container)
{
    $found = Get-ChildItem $Xenia -Filter "xenia*.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    $xe = if ($found) { $found.FullName } else { $null }
}
if (-not $xe -or -not (Test-Path $xe)) { Write-Output "ERROR: Xenia executable not found under $Xenia"; exit 1 }

$xex = Join-Path $base "deploy\default.xex"
Start-Process -FilePath $xe -ArgumentList "`"$xex`""
Write-Output "OK: launched $([System.IO.Path]::GetFileName($xe)) with default.xex"

# Build the spakc cooker (if needed) and cook one texture into a game.spak.
# Phase 1 helper: run AFTER deploy.ps1 (which wipes deploy\), to drop a
# game.spak next to default.xex so the runtime's stream probe picks it up.
#
#   powershell -ExecutionPolicy Bypass -File cook.ps1 `
#       -Source  E:\Projects\360proj\assets\models\checker.png `
#       -Name    "assets/models/checker.png" `
#       -Out     E:\Projects\360engine\runtime\deploy\game.spak `
#       [-Xedk "<xdk-root>"] [-Raw]

param(
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$Name,
    [Parameter(Mandatory=$true)][string]$Out,
    [string]$Xedk = "",
    [switch]$Raw
)

$ErrorActionPreference = "Stop"
$root    = $PSScriptRoot
$msbuild = "C:\Windows\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe"

if ($Xedk) { $env:XEDK = $Xedk }
if (-not $env:XEDK) { $env:XEDK = [Environment]::GetEnvironmentVariable('XEDK','Machine') }
if (-not $env:XEDK) { throw "XEDK not set (needed for Bundler.exe + win32 xcompress)" }
if (-not (Test-Path $msbuild)) { throw "MSBuild v4.0 not found ($msbuild)" }
if (-not (Test-Path $Source))  { throw "Source image not found: $Source" }

$exe = Join-Path $root "Release\spakc.exe"
Write-Output "[1/2] Building spakc (Release|Win32)..."
& $msbuild (Join-Path $root "spakc.vcxproj") /p:Configuration=Release /p:Platform=Win32 /v:minimal /nologo
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) { throw "spakc build failed" }

$outDir = Split-Path -Parent $Out
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }

Write-Output "[2/2] Cooking $Source -> $Out ..."
$rawArg = if ($Raw) { "--raw" } else { $null }
& $exe $Source $Name $Out $rawArg
if ($LASTEXITCODE -ne 0) { throw "spakc cook failed" }
Write-Output "OK"

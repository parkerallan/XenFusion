# Assemble a Xenia-loadable game folder from a built game360.xex plus a project's
# content. Xenia mounts the folder that holds the launched .xex as "game:\", which
# is exactly the paths the runtime reads (game:\game.proj, game:\scenes\...,
# game:\assets\..., game:\shaders\*.cso). Shaders ship precompiled (no HLSL).
#
#   powershell -ExecutionPolicy Bypass -File deploy.ps1 -Project <dir> [-Xedk <xdk-root>] [-OutDir <dir>] [-Config Release|Debug]
#
# Then launch <runtime>\deploy\default.xex in Xenia.

param(
    [Parameter(Mandatory=$true)][string]$Project,
    [string]$Xedk    = "",
    [string]$OutDir  = "",
    [string]$Config  = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
# All build artifacts live under the output base (empty -OutDir = the runtime\ dir).
$base = if ($OutDir) { $OutDir } else { $root }
$xex  = Join-Path $base "$Config\game360.xex"
$out  = Join-Path $base "deploy"

if (-not (Test-Path $xex))     { throw "Build first: $xex not found (run MSBuild for $Config|Xbox 360)." }
if (-not (Test-Path $Project)) { throw "Project not found: $Project" }

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

# The title image Xenia loads. A bare .xex mounts its own folder as game:\.
Copy-Item $xex (Join-Path $out "default.xex")

# Scene + asset content, laid out exactly as the editor's project.
foreach ($sub in @("scenes", "assets")) {
    $src = Join-Path $Project $sub
    if (Test-Path $src) { Copy-Item $src (Join-Path $out $sub) -Recurse }
}
# The game ships no HLSL source — shaders are precompiled to .cso below, so strip
# any .hlsl that got copied in with the assets.
Get-ChildItem (Join-Path $out "assets") -Filter *.hlsl -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force

# Shaders. Compile every HLSL -> Xenos .cso offline with the XDK's PC-side fxc,
# and bake each custom shader's //@ render directives into a tiny .dir sidecar.
# The game ships only .cso + .dir (no HLSL). The runtime loads the .cso and reads
# the .dir for render state. (If fxc is missing it can't ship shaders — the
# runtime's HLSL fallback only helps when run against a source tree, in dev.)
$shaderOut = Join-Path $out "shaders"
New-Item -ItemType Directory -Force $shaderOut | Out-Null

# Resolve the XDK: the -Xedk passed from the editor wins; otherwise this process's
# XEDK, else the persisted Machine value (set machine-wide but often absent from a
# fresh process env block).
$xdk = if ($Xedk) { $Xedk } elseif ($env:XEDK) { $env:XEDK } else { [Environment]::GetEnvironmentVariable('XEDK','Machine') }
$fxc = if ($xdk) { Join-Path $xdk "bin\win32\fxc.exe" } else { $null }
if ($fxc -and (Test-Path $fxc)) {
    function Compile-Xeno($hlsl, $bakeDir) {
        $stem = [System.IO.Path]::GetFileNameWithoutExtension($hlsl)
        & $fxc /Tvs_3_0 /EVSMain /Zpc "/Fo$(Join-Path $shaderOut ($stem + '_vs.cso'))" $hlsl 2>&1 | Out-Null
        & $fxc /Tps_3_0 /EPSMain /Zpc "/Fo$(Join-Path $shaderOut ($stem + '_ps.cso'))" $hlsl 2>&1 | Out-Null
        if ($bakeDir) {
            $dirs = Select-String -Path $hlsl -Pattern '//@' | ForEach-Object { $_.Line.Trim() }
            if ($dirs) { $dirs | Set-Content -Path (Join-Path $shaderOut ($stem + '.dir')) -Encoding ascii }
        }
    }
    # The built-in material is the editor's canonical shader (one source of truth,
    # shared by the PC viewport and the console runtime) - compile that, no //@.
    $stdHlsl = Join-Path (Split-Path -Parent $root) "src\shaders\standard.hlsl"
    if (-not (Test-Path $stdHlsl)) { throw "Built-in material not found: $stdHlsl" }
    Compile-Xeno $stdHlsl $false
    Get-ChildItem (Join-Path $Project "assets") -Filter *.hlsl -Recurse -ErrorAction SilentlyContinue |
        ForEach-Object { Compile-Xeno $_.FullName $true }           # custom shaders (+ //@ sidecar)
    Write-Output "Compiled Xenos shaders (.cso) + baked //@ sidecars"
} else {
    Write-Output "WARN: XEDK/fxc.exe not found - no shaders compiled"
}

# Project manifest -> game.proj (startupScene). Optional: the runtime falls back
# to scenes\Main.scene when it's absent.
$proj = Get-ChildItem (Join-Path $Project "*.proj") -ErrorAction SilentlyContinue | Select-Object -First 1
if ($proj) { Copy-Item $proj.FullName (Join-Path $out "game.proj") }

Write-Output "Deployed to $out"
Write-Output "Launch in Xenia:  <xenia.exe> `"$($out)\default.xex`""

# Assemble a Xenia-loadable game folder from a built game360.xex plus a project's
# content. Xenia mounts the folder that holds the launched .xex as "game:\".
#
# All meshes + textures ship inside a single cooked **game.spak** (the streaming
# subsystem — STREAMING.md); the game image contains NO raw .mesh/.png. Scenes,
# the project manifest, and precompiled Xenos shaders (.cso + .dir) ship as files.
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

# Wipe any previous deploy. If a file is locked (Xenia still running on this
# deploy), Remove-Item deletes some files before failing and leaves a broken
# folder — catch that and give a clear message instead of a partial wipe.
if (Test-Path $out) {
    try { Remove-Item -Recurse -Force $out -ErrorAction Stop }
    catch { throw "Could not clear $out (is Xenia still running on this deploy? close it and retry): $($_.Exception.Message)" }
}
New-Item -ItemType Directory -Force $out | Out-Null

# The title image Xenia loads. A bare .xex mounts its own folder as game:\.
Copy-Item $xex (Join-Path $out "default.xex")

# Scene content (the .scene files the runtime reads). Meshes + textures do NOT
# ship as raw files — they are cooked into game.spak below. Custom-shader .hlsl are
# read from the source project (not the image) and compiled to .cso further down.
$scenesSrc = Join-Path $Project "scenes"
if (Test-Path $scenesSrc) { Copy-Item $scenesSrc (Join-Path $out "scenes") -Recurse }

# Environment cube map faces ship loose (assets/env/env_*.png): one global map
# per project; the runtime assembles the cube at boot via D3DX. Small + loaded
# once, so not worth cooking into the pak.
$envSrc = Join-Path $Project "assets\env"
if (Test-Path $envSrc) {
    New-Item -ItemType Directory -Force (Join-Path $out "assets\env") | Out-Null
    Copy-Item (Join-Path $envSrc "env_*.png") (Join-Path $out "assets\env") -ErrorAction SilentlyContinue
}

# Gameplay scripts ship loose (Lua source, compiled on load — never .luac, which
# is endian-specific). Copy every .lua under the project, preserving its path
# relative to the project so the runtime's game:\<script_path> resolves.
$projRoot = (Resolve-Path $Project).Path
Get-ChildItem $Project -Filter *.lua -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
    $rel = $_.FullName.Substring($projRoot.Length).TrimStart('\','/')
    $dst = Join-Path $out $rel
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item $_.FullName $dst
}

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
    # exit code: errors throw with the compiler's own message, warnings just print.
    function Invoke-Fxc([string[]]$fxcArgs) {
        $prev = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try { $log = (& $fxc @fxcArgs 2>&1 | Out-String) }
        finally { $ErrorActionPreference = $prev }
        if ($LASTEXITCODE -ne 0) { throw "fxc failed: $($fxcArgs -join ' ')`n$log" }
        foreach ($line in ($log -split "`r?`n")) {
            if ($line -match ':\s*warning\s') { Write-Output "WARN: $($line.Trim())" }
        }
    }
    function Compile-Xeno($hlsl, $bakeDir) {
        $stem = [System.IO.Path]::GetFileNameWithoutExtension($hlsl)
        Invoke-Fxc @("/Tvs_3_0", "/EVSMain", "/Zpc", "/Fo$(Join-Path $shaderOut ($stem + '_vs.cso'))", $hlsl)
        Invoke-Fxc @("/Tps_3_0", "/EPSMain", "/Zpc", "/Fo$(Join-Path $shaderOut ($stem + '_ps.cso'))", $hlsl)
        if ($bakeDir) {
            $dirs = Select-String -Path $hlsl -Pattern '//@' | ForEach-Object { $_.Line.Trim() }
            if ($dirs) { $dirs | Set-Content -Path (Join-Path $shaderOut ($stem + '.dir')) -Encoding ascii }
        }
    }
    # The built-in shaders (standard material + bloom post passes) are the
    # editor's canonical sources (one source of truth, shared by the PC viewport
    # and the console runtime) - compile them all, no //@.
    $engineShaderDir = Join-Path (Split-Path -Parent $root) "src\shaders"
    if (-not (Test-Path (Join-Path $engineShaderDir "standard.hlsl"))) { throw "Built-in material not found in: $engineShaderDir" }
    Get-ChildItem $engineShaderDir -Filter *.hlsl | ForEach-Object { Compile-Xeno $_.FullName $false }
    Invoke-Fxc @("/Tvs_3_0", "/ESkinVSMain", "/Zpc", "/Fo$(Join-Path $shaderOut 'standard_skin_vs.cso')", (Join-Path $engineShaderDir "standard.hlsl"))
    Invoke-Fxc @("/Tvs_3_0", "/ELightmapVSMain", "/Zpc", "/Fo$(Join-Path $shaderOut 'standard_lightmap_vs.cso')", (Join-Path $engineShaderDir "standard.hlsl"))
    Invoke-Fxc @("/Tps_3_0", "/EShadowPSMain", "/Zpc", "/Fo$(Join-Path $shaderOut 'standard_shadow_ps.cso')", (Join-Path $engineShaderDir "standard.hlsl"))
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

# Cook every mesh the startup scene references (+ their textures, deduped) into
# game.spak. This is the ONLY place mesh/texture assets ship — the runtime streams
# them from the pak. Needs the XDK (spakc links the win32 xcompress libs and shells
# out to Bundler.exe), so this is mandatory now that no raw assets are shipped.
if ($xdk) { $env:XEDK = $xdk } # so spakc (Bundler + win32 libs) resolves the XDK
$msbuild  = "C:\Windows\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe"
$spakcDir = Join-Path $root "tools\spakc"
$spakcExe = Join-Path $spakcDir "Release\spakc.exe"
& $msbuild (Join-Path $spakcDir "spakc.vcxproj") /p:Configuration=Release /p:Platform=Win32 /v:minimal /nologo | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $spakcExe)) { throw "spakc build failed (needs the XDK win32 libs / VS2010)" }

# Resolve the startup scene: game.proj -> startupScene, else scenes\Main.scene.
$startup = "scenes/Main.scene"
if ($proj) { $s = (Get-Content $proj.FullName -Raw | ConvertFrom-Json).startupScene; if ($s) { $startup = $s } }
$sceneFile = Join-Path $Project $startup

# Cook the assets of EVERY scene, not just the startup one. scene.load() can
# switch to any scene in the project, and a scene whose meshes never reached the
# pak loads and renders nothing at all on the console (raw assets are not
# shipped) — a failure that only shows up on hardware. The residency budget
# evicts what isn't being drawn, so carrying every level in the pak costs disc
# space, not runtime memory.
$meshes = @()
$images = @()
$videos = @()
$audios = @()
$fonts = @()
$animators = @()
$sceneFiles = @(Get-ChildItem (Join-Path $Project "scenes") -Filter *.scene -Recurse -ErrorAction SilentlyContinue)
if (-not $sceneFiles -and (Test-Path $sceneFile)) { $sceneFiles = @(Get-Item $sceneFile) }
foreach ($sf in $sceneFiles) {
    $scene = $null
    try { $scene = Get-Content $sf.FullName -Raw | ConvertFrom-Json }
    catch { Write-Warning "$($sf.Name): not valid scene JSON, skipped"; continue }
    foreach ($o in $scene.objects) {
        foreach ($a in $o.attributes) {
            if ($a.model_path) { $meshes += $a.model_path }
            if ($a.imagePath)  { $images += $a.imagePath }
            if ($a.skyboxPath) { $images += $a.skyboxPath } # "Skybox" attribute -> TX2D, like any image
            if ($a.videoPath)  { $videos += $a.videoPath }  # "Video" attribute -> raw VIDE entry
            if ($a.audioPath)  { $audios += $a.audioPath }  # "Audio" attribute -> raw AUDI entry
            if ($a.fontPath)   { $fonts += $a.fontPath }   # "Text" attribute -> FONT + TX2D atlas
            if ($a.controllerPath) { $animators += $a.controllerPath }
        }
    }
}
Write-Output "spakc: scanning $($sceneFiles.Count) scene(s) for assets"
# Assets a Lua script names (gui.panel{ texture = "assets/ui/panel.png" }). The
# scene scan above only sees attribute paths, so anything a script references
# would be missing from the pak. The path written in the script IS the lookup
# key at runtime, so cooking exactly those paths means a file works wherever the
# developer put it — no folder convention, nothing to declare.
#
# A literal that doesn't resolve is warned about, never fatal: a script may hold
# an asset-shaped string that isn't a path, and a path built at run time
# ("icons/" .. n .. ".png") can't be seen here at all.
Get-ChildItem $Project -Filter *.lua -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
    $script = $_
    foreach ($m in [regex]::Matches((Get-Content $script.FullName -Raw),
                                    '["'']([^"'']+\.(?:png|jpg|jpeg|ttf|otf))["'']')) {
        $rel = $m.Groups[1].Value.Replace('\', '/')
        if (-not (Test-Path (Join-Path $Project $rel))) {
            Write-Warning "$($script.Name): asset not found, not cooked - $rel"
            continue
        }
        if ($rel -match '\.(ttf|otf)$') { $fonts += $rel } else { $images += $rel }
    }
}

$meshes = @($meshes | Select-Object -Unique)
$images = @($images | Select-Object -Unique)
$videos = @($videos | Select-Object -Unique)
$audios = @($audios | Select-Object -Unique)
$fonts = @($fonts | Select-Object -Unique)
$animators = @($animators | Select-Object -Unique)

# Animator controllers are PC-cooked from JSON + Assimp source clips into one
# big-endian ANC1 payload each. The temporary files are fed to spakc with their
# original logical controller paths, then removed after the raw ANIM entries are
# written. Assimp and nlohmann/json remain host-only.
$animArgs = @()
$animCookDir = Join-Path $out ".animcook"
if ($animators.Count -gt 0) {
    $engineRoot = Split-Path -Parent $root
    $hostBuild = Join-Path $engineRoot "build"
    & cmake --build $hostBuild --config Release --target animc | Out-Null
    $animc = Join-Path $hostBuild "Release\animc.exe"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $animc)) { throw "animc build failed" }
    New-Item -ItemType Directory -Force $animCookDir | Out-Null
    for ($index = 0; $index -lt $animators.Count; ++$index) {
        $logical = [string]$animators[$index]
        $source = Join-Path $Project $logical
        $cooked = Join-Path $animCookDir ("controller_{0}.anc" -f $index)
        & $animc $Project $source $cooked
        if ($LASTEXITCODE -ne 0) { throw "animc failed for $logical" }
        $animArgs += @("--anim", $cooked, $logical)
    }
}

$imageArgs = @()
foreach ($image in $images) { $imageArgs += @("--image", [string]$image) }
# Lightmaps are per-scene (bake output sits beside the .scene), so every scene
# that has been baked contributes its atlas pair. The per-mesh UV2 sidecars are
# shared across scenes, so they are added once.
$lightmapArgs = @()
$anyLightmap = $false
foreach ($sf in $sceneFiles) {
    $rel = ($sf.FullName.Substring($Project.Length).TrimStart('\', '/')).Replace('\', '/')
    $lightmapRel = [System.IO.Path]::ChangeExtension($rel, ".lmap").Replace('\', '/')
    if (-not (Test-Path (Join-Path $Project $lightmapRel))) { continue }
    $anyLightmap = $true
    $lightmapArgs += @("--lmap", $lightmapRel)
    $base = $lightmapRel.Substring(0, $lightmapRel.Length - 5)
    $lightmapArgs += @("--image", ($base + "_lm0.png"), "--image", ($base + "_lm1.png"))
}
if ($anyLightmap) {
    foreach ($mesh in $meshes) {
        $uv = [System.IO.Path]::ChangeExtension([string]$mesh, ".lmuv").Replace('\', '/')
        if (Test-Path (Join-Path $Project $uv)) { $lightmapArgs += @("--lmuv", $uv) }
    }
}
$fontArgs = @()
foreach ($font in $fonts) { $fontArgs += @("--font", [string]$font) }
$cookArgs = @($meshes) + @($imageArgs) + @($lightmapArgs) + @($fontArgs) + @($videos) + @($audios) + @($animArgs)
if ($cookArgs.Count -gt 0) {
    & $spakcExe build (Join-Path $out "game.spak") $Project @cookArgs
    if ($LASTEXITCODE -ne 0) { throw "spakc cook failed (exit $LASTEXITCODE)" }
} else {
    Write-Output "Note: startup scene references no packaged assets; no game.spak written (shader-only scene)"
}
if (Test-Path $animCookDir) { Remove-Item -Recurse -Force $animCookDir }

Write-Output "Deployed to $out"
Write-Output "Launch in Xenia:  <xenia.exe> `"$($out)\default.xex`""

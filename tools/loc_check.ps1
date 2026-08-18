# Cross-checks the engine's UI localization keys against the language files.
# A key missing from en.json renders on screen as its own dotted path, so this
# exits 1 for that; a partial translation is only a warning.
#
#   powershell -ExecutionPolicy Bypass -File tools\loc_check.ps1
param(
    [string]$SourceDir = "$PSScriptRoot\..\src",
    [string]$LangDir   = "$PSScriptRoot\..\lang"
)

# --- keys referenced by the source -----------------------------------------
$pattern = 'loc::(?:T|TL|TI|TWin)\(\s*(?:ICON_FA_[A-Z_0-9]+\s*,\s*)?"([^"]+)"'
$used = New-Object System.Collections.Generic.HashSet[string]
Get-ChildItem -Path $SourceDir -Recurse -Include *.cpp, *.h | ForEach-Object {
    foreach ($m in [regex]::Matches((Get-Content $_.FullName -Raw), $pattern)) {
        [void]$used.Add($m.Groups[1].Value)
    }
}
Write-Host ("Source references {0} keys" -f $used.Count)

# Composed at runtime by InspectorPanel::AttributeTypeLabel, not written as
# literals, so they never appear in the source scan.
$dynamicPrefixes = @('inspector.attribute.')

# --- flatten a language file to dotted paths -------------------------------
function Get-FlatKeys($node, $prefix, $acc) {
    foreach ($p in $node.PSObject.Properties) {
        if ($p.Name.StartsWith('_')) { continue }   # reserved metadata
        $path = if ($prefix) { "$prefix.$($p.Name)" } else { $p.Name }
        if ($p.Value -is [System.Management.Automation.PSCustomObject]) {
            Get-FlatKeys $p.Value $path $acc
        } else {
            [void]$acc.Add($path)                   # string or list = a leaf
        }
    }
}

$failed = $false
foreach ($file in Get-ChildItem -Path $LangDir -Filter *.json | Sort-Object Name) {
    $json = Get-Content $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    $have = New-Object System.Collections.Generic.HashSet[string]
    Get-FlatKeys $json '' $have

    $missing = @($used | Where-Object { -not $have.Contains($_) } | Sort-Object)
    $orphan  = @($have | Where-Object {
        $k = $_
        (-not $used.Contains($k)) -and
        (-not ($dynamicPrefixes | Where-Object { $k.StartsWith($_) }))
    } | Sort-Object)

    $isBase = ($file.BaseName -eq 'en')
    Write-Host ""
    Write-Host ("{0}  ({1} keys)" -f $file.Name, $have.Count) -ForegroundColor Cyan

    if ($missing.Count -eq 0 -and $orphan.Count -eq 0) {
        Write-Host "  OK" -ForegroundColor Green
        continue
    }
    if ($missing.Count -gt 0) {
        # A hole in the base table is a defect; elsewhere it is just unfinished.
        $colour = if ($isBase) { 'Red' } else { 'Yellow' }
        $label  = if ($isBase) { 'MISSING (renders as the raw key)' } else { 'untranslated (falls back to English)' }
        Write-Host ("  {0} {1}:" -f $missing.Count, $label) -ForegroundColor $colour
        $missing | ForEach-Object { Write-Host "    $_" -ForegroundColor $colour }
        if ($isBase) { $failed = $true }
    }
    if ($orphan.Count -gt 0) {
        Write-Host ("  {0} orphaned (no longer referenced):" -f $orphan.Count) -ForegroundColor DarkYellow
        $orphan | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkYellow }
    }
}

Write-Host ""
if ($failed) { Write-Host "FAILED: en.json is incomplete." -ForegroundColor Red; exit 1 }
Write-Host "en.json is complete." -ForegroundColor Green

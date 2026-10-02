<#
.SYNOPSIS
    Reapply this project's local LVGL patches after an LVGL install, upgrade or downgrade.

.DESCRIPTION
    The Arduino LVGL library is not under version control, so every reinstall silently
    reverts these fixes. This script restores them, and is deliberately machine- and
    version-agnostic:

      * the sketchbook location comes from arduino-cli rather than being hardcoded, so
        the same script works on any machine;
      * the absolute LV_CONF_PATH is generated from that location instead of baked in;
      * the config-resolution header moved in LVGL 9.6.0, so the correct file is chosen
        based on the installed version;
      * the lv_realloc NULL-check backports are applied only on 9.5.x, where they are
        still needed. LVGL 9.6.0 fixed those upstream.

    Safe to run repeatedly: every step is skipped if already applied.

.EXAMPLE
    pwsh -File .\Reapply-LvglPatches.ps1
    pwsh -File .\Reapply-LvglPatches.ps1 -WhatIf
    pwsh -File .\Reapply-LvglPatches.ps1 -NoVerify
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    # Override the sketchbook location instead of asking arduino-cli.
    [string]$Sketchbook,

    # Skip the post-patch verification build.
    [switch]$NoVerify
)

$ErrorActionPreference = 'Stop'
$patchRoot = $PSScriptRoot

function Say { param($m, $c = 'Gray') Write-Host $m -ForegroundColor $c }

# --- 1. Locate the sketchbook without hardcoding a machine-specific path ------
if (-not $Sketchbook) {
    if (Get-Command arduino-cli -ErrorAction SilentlyContinue) {
        $Sketchbook = (& arduino-cli config get directories.user 2>$null)
    }
    if (-not $Sketchbook) { $Sketchbook = Join-Path $HOME 'Documents/Arduino' }
}
$Sketchbook = "$Sketchbook".Trim()
if (-not (Test-Path -LiteralPath $Sketchbook)) { throw "Sketchbook not found: $Sketchbook" }

$libDir  = Join-Path $Sketchbook 'libraries'
$lvglDir = Join-Path $libDir 'lvgl'
$lvConf  = Join-Path $libDir 'lv_conf.h'

if (-not (Test-Path -LiteralPath $lvglDir)) { throw "LVGL is not installed at $lvglDir" }
if (-not (Test-Path -LiteralPath $lvConf)) {
    throw "No lv_conf.h at $lvConf -- copy lvgl_patches/arduino-libraries-folder/lv_conf.h there first."
}

# --- 2. Installed LVGL version ------------------------------------------------
$propsPath = Join-Path $lvglDir 'library.properties'
$version = (Select-String -LiteralPath $propsPath -Pattern '^version=(.+)$').Matches.Groups[1].Value.Trim()

Say "Sketchbook : $Sketchbook"
Say "LVGL       : $version"
Say "lv_conf.h  : $lvConf"
Say ''

# --- 3. Patch 1: force an absolute LV_CONF_PATH -------------------------------
# The Giga core ships Arduino_H7_Video/src/lv_conf.h, a shim that includes the core's
# own lv_conf_9.h. That folder is unconditionally on the sketch include path, so LVGL's
# __has_include("lv_conf.h") probe finds it and wins over the user config wherever it
# is placed. The core config sets LV_USE_DRAW_ARM2D_SYNC 1 (requires arm_2d.h, which the
# core does not ship) and LV_FONT_DEFAULT montserrat_14. Symptoms: a hard build failure
# on LVGL 9.6.0, and silently tiny fonts on 9.5.0.
$candidates = @(
    (Join-Path $lvglDir 'include/lvgl/config/lv_conf_internal.h')   # 9.6.0 and later
    (Join-Path $lvglDir 'src/lv_conf_internal.h')                   # 9.5.x and earlier
)
$target = $candidates | Where-Object {
    (Test-Path -LiteralPath $_) -and (Select-String -LiteralPath $_ -Pattern 'LV_CONF_SKIP' -Quiet)
} | Select-Object -First 1
if (-not $target) { throw "Could not find LVGL's config-resolution header under $lvglDir" }

Say "config header -> $target"

if (Select-String -LiteralPath $target -Pattern '^\s*#define LV_CONF_PATH' -Quiet) {
    Say '  [skip] LV_CONF_PATH already defined' 'DarkGray'
}
elseif ($PSCmdlet.ShouldProcess($target, 'insert absolute LV_CONF_PATH')) {
    $lines  = [System.IO.File]::ReadAllLines($target)
    $anchor = [Array]::FindIndex($lines, [Predicate[string]] {
        param($l) $l -like '#if !defined(LV_CONF_SKIP)*'
    })
    if ($anchor -lt 0) { throw "Could not find the LV_CONF_SKIP anchor in $target" }

    $confForC = $lvConf -replace '\\', '/'
    $q = [char]34
    $block = @(
        '/* LOCAL PATCH (Reapply-LvglPatches.ps1) -- force an absolute LV_CONF_PATH.'
        ' * The Arduino Giga core ships Arduino_H7_Video/src/lv_conf.h, a shim that includes'
        " * the core's own lv_conf_9.h. That folder is always on the sketch include path, so"
        ' * the __has_include("lv_conf.h") probe below finds it and wins over the user config,'
        ' * wherever that is placed. The core config sets LV_USE_DRAW_ARM2D_SYNC 1 (requires'
        ' * arm_2d.h, which the core does not ship) and LV_FONT_DEFAULT montserrat_14, so the'
        ' * symptoms are a hard build failure on LVGL 9.6.0 and silently tiny fonts on 9.5.0.'
        ' * An absolute LV_CONF_PATH bypasses the __has_include search entirely. */'
        '#ifndef LV_CONF_PATH'
        "    #define LV_CONF_PATH $q$confForC$q"
        '#endif'
        ''
    )

    $out = [System.Collections.Generic.List[string]]::new()
    if ($anchor -gt 0) { $out.AddRange([string[]]$lines[0..($anchor - 1)]) }
    $out.AddRange([string[]]$block)
    $out.AddRange([string[]]$lines[$anchor..($lines.Length - 1)])
    [System.IO.File]::WriteAllLines($target, $out)
    Say "  [done] LV_CONF_PATH inserted at line $($anchor + 1)" 'Green'
}

# --- 4. Patches 2 and 3: lv_realloc NULL checks, 9.5.x only -------------------
# Fixed upstream in LVGL 9.6.0 (issue #9794 / PR #9795); the children-array handling
# also moved into lv_obj.c there. Copying the 9.5.0 files onto 9.6.0 would be a version
# mismatch, so they are skipped on anything newer.
if ($version -like '9.5.*') {
    foreach ($rel in @('src/core/lv_obj_class.c', 'src/core/lv_obj_tree.c')) {
        $src = Join-Path $patchRoot "lvgl/$rel"
        $dst = Join-Path $lvglDir $rel
        if (-not (Test-Path -LiteralPath $src)) { Say "  [warn] missing patch file: $src" 'Yellow'; continue }
        if (-not (Test-Path -LiteralPath $dst)) { Say "  [warn] no such target: $dst" 'Yellow'; continue }

        $a = (Get-Content -Raw -LiteralPath $src) -replace "`r`n", "`n"
        $b = (Get-Content -Raw -LiteralPath $dst) -replace "`r`n", "`n"
        if ($a -eq $b) { Say "  [skip] $rel already patched" 'DarkGray'; continue }

        if ($PSCmdlet.ShouldProcess($dst, 'apply lv_realloc NULL-check backport')) {
            if (-not (Test-Path -LiteralPath "$dst.stock")) { Copy-Item -LiteralPath $dst -Destination "$dst.stock" }
            [System.IO.File]::WriteAllText($dst, $a)
            Say "  [done] $rel patched (stock kept as $(Split-Path $dst -Leaf).stock)" 'Green'
        }
    }
}
else {
    Say "LVGL $version -- NULL-check backports not needed (fixed upstream in 9.6.0)" 'DarkGray'
}

# --- 5. Verify ----------------------------------------------------------------
Say ''
if ($WhatIfPreference) { Say 'WhatIf: no changes written.' 'Yellow'; return }

$m = Select-String -LiteralPath $target -Pattern '#define LV_CONF_PATH "(.+)"'
if (-not $m) { Say 'LV_CONF_PATH is not defined after patching.' 'Red'; exit 1 }
$defined = $m.Matches.Groups[1].Value
Say "LV_CONF_PATH -> $defined"
if (Test-Path -LiteralPath $defined) { Say '  target exists' 'Green' }
else { Say '  TARGET MISSING -- the build will fail' 'Red'; exit 1 }

if ($NoVerify) { Say 'Done (verification build skipped).' 'Green'; return }

$sketch = Split-Path $patchRoot -Parent
Say ''
Say "Verifying with a build of $(Split-Path $sketch -Leaf) (this takes a few minutes) ..."
& arduino-cli compile --fqbn arduino:mbed_giga:giga $sketch 2>&1 | Select-Object -Last 3
if ($LASTEXITCODE -ne 0) { Say 'Verification build FAILED.' 'Red'; exit 1 }
Say 'Verification build succeeded.' 'Green'

# LVGL local patches

The Arduino LVGL library (installed via Library Manager at `<sketchbook>/libraries/lvgl/`)
is not part of this repo and is not under version control, so **every LVGL install,
upgrade or downgrade silently reverts these fixes**. The symptoms are not obvious —
see [Symptoms](#symptoms) — so reapply them before trusting a build.

## Quick start

```powershell
pwsh -File .\Reapply-LvglPatches.ps1
```

That detects the sketchbook via `arduino-cli`, picks the right files for the installed
LVGL version, applies what is needed, and verifies with a build. It is safe to re-run;
every step is skipped if already applied. `-WhatIf` previews, `-NoVerify` skips the build.

Verified 2026-10-01 on LVGL 9.5.0 and 9.6.0, Arduino Giga R1 WiFi, `arduino:mbed_giga` 4.6.0.

## Root cause

The Arduino Giga core ships its own LVGL config and it **wins over yours**.

`Arduino_H7_Video` (in the core, not a Library Manager library) contains a six-line
`src/lv_conf.h`:

```c
#if __has_include("draw/sw/blend/neon/lv_blend_neon.h")
#include "lv_conf_9.h"
#else
#include "lv_conf_8.h"
#endif
```

Because the sketch includes `Arduino_H7_Video.h`, that `src/` folder is unconditionally on
the include path for the whole build. LVGL's own `__has_include("lv_conf.h")` probe finds
*that* shim and succeeds — before any user-supplied `lv_conf.h` is considered, no matter
where it is placed (library root, next to `lvgl/` per the official docs, anywhere). Your
config is never read. LVGL is working exactly as designed; it legitimately finds *a*
`lv_conf.h`, just not the intended one.

Confirmed with `arm-none-eabi-gcc -E -H` include-resolution traces and `#pragma message`
probes. An absolute `LV_CONF_PATH` is the correct fix precisely because it bypasses the
`__has_include` search entirely.

> An earlier version of this writeup misdiagnosed this as an LVGL directory-layout bug and
> filed [lvgl/lvgl#10356](https://github.com/lvgl/lvgl/issues/10356). That diagnosis was
> wrong and the issue was closed. The fix here was always right; only the stated reason was.

## Symptoms

The core's `lv_conf_9.h` differs from this project's config in ways that bite differently
per LVGL version:

| Setting | Core `lv_conf_9.h` | This project | Consequence |
|---|---|---|---|
| `LV_USE_DRAW_ARM2D_SYNC` | `1` | `0` | **Build failure on 9.6.0** — see below |
| `LV_FONT_DEFAULT` | `montserrat_14` | `montserrat_32` | **Silently tiny text on 9.5.0** |
| `LV_FONT_MONTSERRAT_32` | `0` | `1` | intended font not compiled in |
| `LV_MEM_SIZE` | 64 KB | 512 KB | memory pressure, SDRAM pool unused |

**On 9.6.0** the build fails outright:

```
lvgl/src/draw/sw/blend/arm2d/lv_blend_arm2d.c:17:10: fatal error: arm_2d.h: No such file or directory
```

The core enables Arm-2D acceleration but the core does not ship Arm-2D. LVGL 9.5.0 has no
arm2d sources at all, so the flag was inert there; 9.6.0 added the backend that honours it.
This is a **latent Arduino core bug that LVGL 9.6.0 exposed**, not an LVGL regression — it
reproduces in a ten-line sketch with no user config involved. See
[`../tools/arduino_core_lv_conf_shadowing_issue.md`](../tools/arduino_core_lv_conf_shadowing_issue.md).

**On 9.5.0** it builds happily and just renders everything at montserrat_14. That is the
"why is all my text suddenly tiny" failure mode, and it is easy to misread as a lost
`lv_conf.h` edit. Your `lv_conf.h` is fine; it is simply not being read.

## What is needed, per version

| Patch | 9.5.0 | 9.6.0 | Why |
|---|---|---|---|
| Absolute `LV_CONF_PATH` | required | required | the shadowing above |
| `lv_obj_class.c` NULL checks | required | **obsolete** | fixed upstream |
| `lv_obj_tree.c` NULL checks | required | **obsolete** | fixed upstream |

The two NULL-check backports cover unchecked `lv_realloc()` results on the paths that grow
or shrink a parent's children array, which on the grow paths were immediately indexed into —
a raw NULL write under memory pressure rather than a clean failure. Hit on every WiFi
scan-list row build (`lv_obj_create`) and every MQTT keyboard reparent
(`lv_obj_set_parent`). Real upstream defect
([lvgl/lvgl#9794](https://github.com/lvgl/lvgl/issues/9794), fixed in
[PR #9795](https://github.com/lvgl/lvgl/pull/9795), which landed after 9.5.0 shipped).

In 9.6.0 that handling moved to `lv_obj.c` and **is** properly checked in both the grow
(`lv_obj_add_child`) and shrink (`lv_obj_remove_child`) paths, so these two files are no
longer applied there — and copying 9.5.0 sources onto 9.6.0 would be a version mismatch.
The script enforces that automatically.

One instance of the same class does survive in 9.6.0 and is worth a small upstream PR:
`src/core/lv_obj_tree.c` assigns an unchecked shrinking `lv_realloc()` straight to
`disp->screens`, so a failed shrink nulls the array. Noted in the issue document above.

## The config header moved in 9.6.0

The file carrying the `__has_include` logic (and therefore the patch) differs by version:

- 9.5.x and earlier: `lvgl/src/lv_conf_internal.h`
- 9.6.0 and later: `lvgl/include/lvgl/config/lv_conf_internal.h`
  (`src/lv_conf_internal.h` still exists in 9.6.0 but is a deprecation shim)

The script picks the correct one by looking for the `LV_CONF_SKIP` anchor rather than
assuming a path.

## Layout

- `arduino-libraries-folder/lv_conf.h` — copy into `<sketchbook>/libraries/` itself, a
  *sibling* of the `lvgl` folder, per the
  [official Arduino LVGL setup docs](https://lvgl.io/docs/open/integration/frameworks/arduino#configure-lvgl).
  This file is not touched by LVGL reinstalls, which is why it survives while the others do
  not. `LV_MEM_SIZE` raised to 512 KB, `LV_MEM_POOL_ALLOC`/`LV_MEM_POOL_INCLUDE` routed
  through `ea_malloc` (SDRAM-backed), `LV_FONT_DEFAULT` set to `&lv_font_montserrat_32`.
- `lvgl/` — mirrors the LVGL library tree; files here are copied in preserving their
  relative path. Only used on 9.5.x now.
- `Reapply-LvglPatches.ps1` — the script.

Note this config is a 9.5.0-era template. It works correctly on 9.6.0 (verified: the build
links `lv_font_montserrat_32` and `_34` and nothing else), but options added in 9.6.0 are
absent from it and therefore take LVGL's internal defaults. Regenerating it from 9.6.0's
`lv_conf_template.h` and re-porting the four settings above would make that explicit.

## Manual procedure, if you cannot run the script

1. Copy `arduino-libraries-folder/lv_conf.h` into `<sketchbook>/libraries/` (next to, not
   inside, `lvgl/`) if it is not already there.
2. In the config header for your version (see above), insert before the
   `#if !defined(LV_CONF_SKIP) || defined(LV_CONF_PATH)` line:

   ```c
   #ifndef LV_CONF_PATH
       #define LV_CONF_PATH "<absolute path to your lv_conf.h, forward slashes>"
   #endif
   ```

   This path is **machine-specific**. A previous version of this patch hardcoded one
   machine's `C:/Users/noahc/OneDrive/...` path, which silently broke on any other machine;
   the script now generates it from the detected sketchbook instead.
3. On 9.5.x only, copy `lvgl/src/core/lv_obj_class.c` and `lvgl/src/core/lv_obj_tree.c`
   into the library.

## Verifying it worked

A successful build is not sufficient on 9.5.0, where the failure mode is silent. Check
which fonts actually got linked:

```powershell
arduino-cli compile --fqbn arduino:mbed_giga:giga --output-dir build .
& "$env:LOCALAPPDATA\Arduino15\packages\arduino\tools\arm-none-eabi-gcc\7-2017q4\bin\arm-none-eabi-nm.exe" `
    build\Giga_YH_Dashboard_Unit2.ino.elf | Select-String montserrat
```

Expect `lv_font_montserrat_32` and `lv_font_montserrat_34`. If you see
`lv_font_montserrat_14`, the core's config is still winning and the patch did not take.

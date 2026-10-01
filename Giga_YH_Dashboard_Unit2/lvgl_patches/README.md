# LVGL 9.5.0 local patches

The Arduino LVGL library (installed via Library Manager, normally at
`<sketchbook>/libraries/lvgl/`) is not part of this git repo and isn't
under version control on its own, so the fixes below live here instead.
If LVGL is ever reinstalled or updated, these need to be reapplied (or
re-verified as already fixed upstream) before the dashboard hang fix
from v1.0.56 is trustworthy again.

Root-caused and fixed 2026-07-17, corrected 2026-07-18 after an initial
wrong diagnosis. See the v1.0.56 changelog entry in
`Giga_YH_Dashboard_Unit2.ino` for the full investigation writeup.

## Layout

This folder has two subtrees, because the two config-related files below
go in **different** base directories relative to `lvgl`:

- `arduino-libraries-folder/` -- copy `lv_conf.h` into
  `<sketchbook>/libraries/` itself (a *sibling* of the `lvgl` folder, not
  inside it -- per the
  [official Arduino LVGL setup docs](https://lvgl.io/docs/open/integration/frameworks/arduino#configure-lvgl)).
- `lvgl/` -- copy everything under here into the `lvgl` library folder
  itself, preserving the relative path (e.g. `lvgl/src/core/lv_obj_class.c`
  -> `<sketchbook>/libraries/lvgl/src/core/lv_obj_class.c`).

## Files

- `arduino-libraries-folder/lv_conf.h` -- `LV_MEM_SIZE` raised to 512KB,
  `LV_MEM_POOL_ALLOC`/`LV_MEM_POOL_INCLUDE` routed through `ea_malloc`
  (SDRAM-backed), `LV_FONT_DEFAULT` corrected to `&lv_font_montserrat_32`
  (the only size actually enabled -- the stock default pointed at a
  disabled font, previously masked by the bug below).
- `lvgl/src/lv_conf_internal.h` -- forces an absolute `LV_CONF_PATH` so
  every translation unit resolves the exact same `lv_conf.h`, working
  around real, confirmed behavior (not an LVGL bug -- see below): the
  Arduino Giga core's own display-driver helper library,
  `Arduino_H7_Video`, ships its *own* bundled `lv_conf.h` inside its
  `src/` folder. Because that folder is unconditionally on the include
  path for the whole sketch build, `__has_include("lv_conf.h")` finds
  *that* file and wins -- before any user-supplied `lv_conf.h` is ever
  considered, regardless of where it's placed. Confirmed via
  `arm-none-eabi-gcc -E -H` (actual include-resolution trace) and
  `#pragma message` probes. An absolute `LV_CONF_PATH` is the correct
  fix specifically because it bypasses the `__has_include` search
  entirely. (An earlier version of this fix/writeup misdiagnosed this as
  an LVGL directory-layout bug and filed
  [lvgl/lvgl#10356](https://github.com/lvgl/lvgl/issues/10356) --
  that diagnosis was wrong and the issue was corrected/closed. This
  project's actual fix was right all along; only the stated reason was
  wrong.)
- `lvgl/src/core/lv_obj_class.c`, `lvgl/src/core/lv_obj_tree.c` --
  backported minimal NULL-checks for four unchecked `lv_realloc()` call
  sites that grow/shrink a parent object's children array and, on the
  grow paths, immediately index into the result. Under memory pressure
  that's a raw NULL-pointer write instead of a clean failure -- hit on
  every WiFi-scan-list row build (`lv_obj_create`) and every MQTT
  keyboard reparent (`lv_obj_set_parent`). This one *is* a real,
  confirmed upstream LVGL defect:
  [lvgl/lvgl#9794](https://github.com/lvgl/lvgl/issues/9794), fixed in
  [PR #9795](https://github.com/lvgl/lvgl/pull/9795) six weeks after our
  installed 9.5.0 shipped. Once LVGL is upgraded past that fix, this
  local patch can be dropped.

## How to reapply

Copy `arduino-libraries-folder/lv_conf.h` into `<sketchbook>/libraries/`
directly (next to, not inside, the `lvgl` folder). Copy everything under
`lvgl/` into the `lvgl` library folder itself, preserving the relative
path under `src/`.

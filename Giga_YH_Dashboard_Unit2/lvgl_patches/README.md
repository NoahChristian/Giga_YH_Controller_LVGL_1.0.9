# LVGL 9.5.0 local patches

The Arduino LVGL library (installed via Library Manager, normally at
`<sketchbook>/libraries/lvgl/`) is not part of this git repo and isn't
under version control on its own, so the fixes below live here instead
-- as full copies of the patched files, at the same relative paths they
occupy inside the `lvgl` library folder. If LVGL is ever reinstalled or
updated, these need to be reapplied (or re-verified as already fixed
upstream) before the dashboard hang fix from v1.0.56 is trustworthy
again.

Root-caused and fixed 2026-07-17. See the v1.0.56 changelog entry in
`Giga_YH_Dashboard_Unit2.ino` for the full investigation writeup, and
[lvgl/lvgl#10356](https://github.com/lvgl/lvgl/issues/10356) for the
upstream report on one of the two issues below.

## Files

- `lv_conf.h` (library root) -- `LV_MEM_SIZE` raised to 512KB,
  `LV_MEM_POOL_ALLOC`/`LV_MEM_POOL_INCLUDE` routed through `ea_malloc`
  (SDRAM-backed), `LV_FONT_DEFAULT` corrected to `&lv_font_montserrat_32`
  (the only size actually enabled -- the stock default pointed at a
  disabled font, previously masked by the bug below).
- `src/lv_conf_internal.h` -- forces an absolute `LV_CONF_PATH` so every
  translation unit resolves the exact same `lv_conf.h`, working around a
  real LVGL bug: this Arduino install's `lv_conf.h` sits one directory
  above `src/`, but `lv_conf_internal.h`'s own fallback assumes it sits
  two levels up (the upstream/PlatformIO layout), and the `__has_include`
  auto-detect ahead of that fallback doesn't reliably catch it either.
  Without this, library `.c` files (not just the sketch itself) silently
  fall back to LVGL's own built-in defaults -- in this project's case, a
  64KB internal-SRAM heap instead of the intended 512KB SDRAM-backed
  pool, confirmed via `#pragma message` compile probes and
  `lv_mem_monitor()` at runtime. Filed upstream as
  [lvgl/lvgl#10356](https://github.com/lvgl/lvgl/issues/10356).
- `src/core/lv_obj_class.c`, `src/core/lv_obj_tree.c` -- backported
  minimal NULL-checks for four unchecked `lv_realloc()` call sites that
  grow/shrink a parent object's children array and, on the grow paths,
  immediately index into the result. Under memory pressure that's a raw
  NULL-pointer write instead of a clean failure -- hit on every WiFi-
  scan-list row build (`lv_obj_create`) and every MQTT keyboard reparent
  (`lv_obj_set_parent`). This is the same defect as upstream
  [lvgl/lvgl#9794](https://github.com/lvgl/lvgl/issues/9794), fixed in
  [PR #9795](https://github.com/lvgl/lvgl/pull/9795) six weeks after our
  installed 9.5.0 shipped. Once LVGL is upgraded past that fix, this
  local patch can be dropped.

## How to reapply

Copy each file here over the corresponding file under
`<sketchbook>/libraries/lvgl/`, preserving the relative path (e.g.
`lv_conf.h` -> library root, `src/core/lv_obj_class.c` ->
`src/core/lv_obj_class.c`).

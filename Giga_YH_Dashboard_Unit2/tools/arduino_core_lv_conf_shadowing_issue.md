# Draft upstream report: Arduino_H7_Video's bundled `lv_conf.h` breaks LVGL 9.6.0 on GIGA

Status: **draft, not yet filed.** Target repo: [arduino/ArduinoCore-mbed](https://github.com/arduino/ArduinoCore-mbed).
Written 2026-10-01 from a reproduction on this machine. A secondary, unrelated LVGL PR
candidate is noted at the end.

Before filing, search the tracker again — related existing issues are listed under
[Prior art](#prior-art); this may belong as a comment on one of them rather than a new issue.

---

## Title

`Arduino_H7_Video`'s bundled `lv_conf.h` shadows user config and enables Arm-2D, breaking
every LVGL 9.6.0 build on GIGA

## Summary

`Arduino_H7_Video` ships `src/lv_conf.h`, which unconditionally shadows any user-supplied
`lv_conf.h` for every LVGL translation unit. The config it pulls in sets
`LV_USE_DRAW_ARM2D_SYNC 1`, but the core does not ship Arm-2D. LVGL 9.5.0 had no arm2d
sources so the flag was inert; LVGL 9.6.0 added the backend that honours it, so **any**
sketch using `Arduino_H7_Video` with LVGL 9.6.0 now fails to compile:

```
.../libraries/lvgl/src/draw/sw/blend/arm2d/lv_blend_arm2d.c:17:10: fatal error: arm_2d.h: No such file or directory
 #include <arm_2d.h>
          ^~~~~~~~~~
compilation terminated.
```

This is a latent core bug that LVGL 9.6.0 exposed, not an LVGL regression. A user config
setting `LV_USE_DRAW_ARM2D_SYNC 0` does not help, because that config is never read.

## Environment

| | |
|---|---|
| Board | Arduino GIGA R1 WiFi + GIGA Display Shield |
| FQBN | `arduino:mbed_giga:giga` |
| Core | `arduino:mbed_giga` 4.6.0 |
| `Arduino_H7_Video` | 1.0 (bundled in the core) |
| LVGL | 9.6.0 fails; 9.5.0 builds |
| arduino-cli | 1.5.1 |
| Host | Windows 11 |

## Reproduction

Ten lines, no user `lv_conf.h` required:

```c
#include <Arduino_H7_Video.h>
#include <lvgl.h>

Arduino_H7_Video Display(800, 480, GigaDisplayShield);

void setup() {
  Display.begin();
  lv_obj_t * label = lv_label_create(lv_screen_active());
  lv_label_set_text(label, "repro");
}
void loop() { lv_timer_handler(); delay(5); }
```

```
arduino-cli lib install lvgl@9.6.0
arduino-cli compile --fqbn arduino:mbed_giga:giga .
```

Fails as above. `arduino-cli lib install lvgl@9.5.0` then builds.

## Mechanism

`Arduino_H7_Video/src/lv_conf.h` is a six-line shim:

```c
#if __has_include("draw/sw/blend/neon/lv_blend_neon.h")
#include "lv_conf_9.h"
#else
#include "lv_conf_8.h"
#endif
```

Because a sketch using the display shield includes `Arduino_H7_Video.h`, that `src/` folder
is on the `-I` list for the whole build. LVGL's config resolution does:

```c
#ifdef __has_include
    #if __has_include("lv_conf.h")
        #ifndef LV_CONF_INCLUDE_SIMPLE
            #define LV_CONF_INCLUDE_SIMPLE
```

That probe finds the core's shim and succeeds, so the core's `lv_conf_9.h` is used and the
user's `lv_conf.h` is never consulted — wherever it is placed, including the location the
[official Arduino setup docs](https://lvgl.io/docs/open/integration/frameworks/arduino#configure-lvgl)
specify (beside the `lvgl` folder).

Verified with `arm-none-eabi-gcc -E -H` include-resolution traces and `#pragma message`
probes.

The relevant divergences between the core's `lv_conf_9.h` and a user config:

| Setting | Core `lv_conf_9.h` | Effect when it wins |
|---|---|---|
| `LV_USE_DRAW_ARM2D_SYNC 1` | requires `arm_2d.h`, not shipped | build failure on 9.6.0 |
| `LV_FONT_DEFAULT montserrat_14` | overrides the user's choice | silently wrong font sizes on 9.5.0 |
| `LV_MEM_SIZE (64 * 1024U)` | overrides the user's choice | memory pressure; any SDRAM pool config ignored |

The font and memory consequences are the quieter half of this bug: on 9.5.0 everything
builds and simply renders at the wrong size, which reads like a lost `lv_conf.h` edit.

## The commonly-cited workaround does not work

Several forum threads suggest including `lvgl.h` **before** `Arduino_H7_Video.h` so LVGL
resolves the user config first. Tested here on 9.6.0: **it still fails with the same
`arm_2d.h` error.**

`lv_blend_arm2d.c` is a library translation unit. Arduino compiles every source under
`lvgl/src/`, and each one resolves `lv_conf.h` against the full `-I` list, which contains
`Arduino_H7_Video/src` whenever the sketch uses that library. The order of `#include`
directives in the *sketch* cannot change how a *library* source file resolves its config.
Any workaround has to apply to every translation unit.

## Suggested fixes, in order of preference

1. **Set `LV_USE_DRAW_ARM2D_SYNC 0` in the bundled `lv_conf_9.h`.** One line, unblocks
   LVGL 9.6.0 immediately. The core cannot satisfy the flag it currently sets, so enabling
   it is not meaningful regardless of the shadowing question.
2. **Stop shadowing the user's config unconditionally.** Guard the bundled shim so it only
   applies when the user has not supplied one, or rename it (e.g. `arduino_h7_lv_conf.h`)
   and include it explicitly from the library's own sources rather than leaving a file named
   `lv_conf.h` on the global include path. The current arrangement means a GIGA user cannot
   configure LVGL at all through the documented mechanism.
3. **Ship Arm-2D**, if the acceleration is actually intended. Largest change, and only worth
   it if the performance is wanted.

Fix 1 is the minimal unblock; fix 2 addresses the underlying problem.

## Local workaround, for anyone hitting this

Define an absolute `LV_CONF_PATH` inside LVGL's config-resolution header, which bypasses
the `__has_include` search for every translation unit:

```c
#ifndef LV_CONF_PATH
    #define LV_CONF_PATH "/absolute/path/to/your/lv_conf.h"
#endif
```

Insert before the `#if !defined(LV_CONF_SKIP) || defined(LV_CONF_PATH)` line in:

- LVGL 9.6.0+: `lvgl/include/lvgl/config/lv_conf_internal.h`
- LVGL 9.5.x and earlier: `lvgl/src/lv_conf_internal.h`

This edits the installed library, so it must be reapplied after every LVGL install. This
project automates it in `lvgl_patches/Reapply-LvglPatches.ps1`.

## Prior art

Search before filing; this may be a comment on an existing thread.

- [arduino/ArduinoCore-mbed#952](https://github.com/arduino/ArduinoCore-mbed/issues/952) —
  "LVGL v9.2.0 not compatible with Arduino_H7_Video library"
- [arduino/ArduinoCore-mbed#834](https://github.com/arduino/ArduinoCore-mbed/issues/834) —
  "LVGL 9 release"
- [arduino/ArduinoCore-mbed#1120](https://github.com/arduino/ArduinoCore-mbed/issues/1120) —
  `Arduino_H7_Video::begin()` and LVGL include-path interaction under PlatformIO
- [Arduino Forum: "My 'lv_conf.h' being ignored"](https://forum.arduino.cc/t/my-lv-conf-h-being-ignored/1296110)
  — same shadowing, and the include-reorder workaround disproved above
- [lvgl/lvgl#10356](https://github.com/lvgl/lvgl/issues/10356) — filed from this project in
  July 2026 against LVGL on a wrong diagnosis, since closed. LVGL's resolution logic is
  behaving as designed; the problem is the core shipping a file named `lv_conf.h` on the
  global include path.

---

# Secondary, unrelated: LVGL PR candidate

Separate from the above, and against [lvgl/lvgl](https://github.com/lvgl/lvgl) rather than
the Arduino core.

LVGL 9.6.0 fixed the unchecked `lv_realloc()` results on the children-array paths
([#9794](https://github.com/lvgl/lvgl/issues/9794) /
[PR #9795](https://github.com/lvgl/lvgl/pull/9795)); `lv_obj_add_child` and
`lv_obj_remove_child` in `src/core/lv_obj.c` both check now. One instance of the same class
survives in `src/core/lv_obj_tree.c`:

```c
disp->screen_cnt--;
disp->screens = lv_realloc(disp->screens, disp->screen_cnt * sizeof(lv_obj_t *));
```

The result is assigned directly, so a failed shrink nulls `disp->screens` while screens
remain. `lv_obj.c` already documents the intended pattern two functions away — a shrink
"should never fail but just in case it's implemented as malloc + memcpy + free" — and
returns early on NULL. Suggested fix, matching that:

```c
disp->screen_cnt--;
lv_obj_t ** shrunk = lv_realloc(disp->screens, disp->screen_cnt * sizeof(lv_obj_t *));
if(shrunk != NULL || disp->screen_cnt == 0) {
    disp->screens = shrunk;
}
```

The `|| disp->screen_cnt == 0` arm keeps a legitimate `realloc(ptr, 0)` returning NULL from
being treated as failure. Low severity — it needs heap exhaustion during screen teardown —
but it is a one-line inconsistency with code LVGL has already fixed elsewhere.

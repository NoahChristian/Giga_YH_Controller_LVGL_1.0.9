<!--
Draft for a GitHub issue against lvgl/lvgl. Edit freely, then let Claude
know when it's ready to post (or post it yourself).
Suggested title is the first heading below.
-->

# lv_conf.h silently ignored in library .c files when installed via Arduino Library Manager (Arduino core: arduino:mbed_giga)

## Environment

- LVGL: 9.5.0, installed via Arduino Library Manager (`library.properties` in
  the library root: `name=lvgl`, `version=9.5.0`)
- Arduino core: `arduino:mbed_giga` 4.6.0 (Arduino Giga R1 WiFi)
- arduino-cli: 1.5.1
- Compiler: `arm-none-eabi-gcc` 7.2.1 (GNU Tools for Arm Embedded Processors
  7-2017-q4-major), bundled with the core
- OS: Windows 11
- Custom `lv_conf.h` placed directly in the library root:
  `<sketchbook>/libraries/lvgl/lv_conf.h` (i.e. one directory above `src/`)
  — this is where Arduino Library Manager actually puts a library's own
  files, and it's also the location most Arduino/LVGL setup guides tell
  users to drop a customized `lv_conf.h`.

## Summary

With `lv_conf.h` in that location, edits to it are honored inconsistently
depending on *which translation unit* is being compiled:

- The sketch's own `.ino` file eventually sees the custom `lv_conf.h` --
  but only because another library (`Arduino_H7_Video.h`, which pulls in
  the LVGL header chain internally) triggers `lv_conf_internal.h`'s
  resolution *before* the sketch's own explicit `#include "lv_conf.h"`
  line runs, and header guards mean whichever happens first wins.
- LVGL's own library `.c` files (e.g. `src/stdlib/builtin/lv_mem_core_builtin.c`,
  the file that actually creates the memory pool) **silently fall through
  to `lv_conf_internal.h`'s built-in defaults**, never seeing the custom
  `lv_conf.h` at all -- with no warning beyond the generic
  `#pragma message("Possible failure to include lv_conf.h...")` note,
  which is easy to miss among the hundreds of other build messages and
  gives no indication of *which* file it's warning about or that defaults
  are actually being used.

Header `lv_conf.h` set `LV_MEM_SIZE` to 512KB with a custom
`LV_MEM_POOL_ALLOC` backend (routing LVGL's heap to external SDRAM via a
`ea_malloc`-based allocator). Because `lv_mem_core_builtin.c` never saw
that config, `lv_mem_init()` silently fell back to `lv_conf_internal.h`'s
own default (`LV_MEM_SIZE (64 * 1024U)`, plain internal-SRAM static
array), regardless of what `lv_conf.h` said. This meant our application
was actually running on a 64KB SRAM heap for LVGL's entire object graph,
not the intended 512KB. Confirmed via a `#pragma message` compile-time
probe stringifying `LV_MEM_SIZE` inside that specific file, and via
`lv_mem_monitor()`'s `total_size` field at runtime (~59KB reported,
matching the 64KB default minus TLSF overhead). The compiled RAM usage
figure `arduino-cli compile` reports never changed no matter what value
we set `LV_MEM_SIZE` to in `lv_conf.h`, which in hindsight was the
biggest tell -- but nothing in the build output said *why*.

This caused a real failure during our own development/release effort,
not just a theoretical concern -- LVGL fell short here. It's related to
a second, already-known, already-fixed LVGL bug
([#9794](https://github.com/lvgl/lvgl/issues/9794)/[#9795](https://github.com/lvgl/lvgl/pull/9795),
not yet in the 9.5.0 release), and tracking it down took a somewhat
heavy lift of real-hardware debugging.

## Root cause

`src/lv_conf_internal.h`'s resolution logic:

```c
#ifdef __has_include
    #if __has_include("lv_conf.h")
        #ifndef LV_CONF_INCLUDE_SIMPLE
            #define LV_CONF_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if !defined(LV_CONF_SKIP) || defined(LV_CONF_PATH)
    #ifdef LV_CONF_PATH
        #include LV_CONF_PATH
    #elif defined(LV_CONF_INCLUDE_SIMPLE)
        #include "lv_conf.h"
    #else
        #include "../../lv_conf.h"   /* Else assume lv_conf.h is next to the lvgl folder. */
    #endif
    ...
```

Two things confound this issue:

1. **The fallback path assumes the wrong layout for this install method.**
   `../../lv_conf.h` (two directories up from `src/lv_conf_internal.h`)
   assumes `lv_conf.h` sits *next to* the `lvgl` folder (the
   clone-as-a-subdirectory / PlatformIO-style layout). Arduino Library
   Manager installs LVGL such that `lv_conf.h` sits *inside* the `lvgl`
   folder, one directory above `src/` -- one level short of what the
   fallback expects.

2. **Whether the `__has_include` auto-detect saves first is
   inconsistent per file.** It depends on whether that specific
   translation unit's compile command happens to have the library
   *root* (not just `src/`) on its include path -- which, empirically,
   differs between the sketch's own `.ino` (compiled with a broader
   include path) and LVGL's own `src/**/*.c` files (compiled, as best I
   can tell from `arduino-cli compile --verbose`, with the library's
   `src/` as the primary `-I` root, not the library root itself). The
   file's own comment nearby links to a
   [known GCC bug](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80753)
   where `__has_include` can silently misbehave, which may also be a
   contributing factor -- I haven't isolated that variable specifically.

So, some units find the real `lv_conf.h` (if something else's include chain resolves the `lv_conf_internal.h`
defaults first), and some never find it at all, silently using
`lv_conf_internal.h`'s built-in defaults with no error.

## A second, related and important symptom: stale incremental-build cache

This part may be more of an `arduino-cli` question than an LVGL one, but
it's downstream of the same root cause, so including it here in case
it's useful context (happy to file separately if that's more
appropriate).

Because affected library `.c` files never actually `#include` the real
`lv_conf.h` (the compiler never opens it for that translation unit), the
compiler's own dependency-file generation (`-MMD`) never lists
`lv_conf.h` as a dependency of that `.o`. `arduino-cli`'s incremental
build cache appears to rely on those dependency files (or an equivalent
mechanism) to decide whether a cached `.o` is still valid. Result: after
editing `lv_conf.h`, a non-`--clean` `arduino-cli compile` can silently
reuse a stale cached `.o` for an affected file, compiled under the *old*
config value, while the rest of the build (which did see the new value)
disagrees -- producing confusing errors (in our case, `undefined
reference` / `not declared in this scope` for a function that's
`#if`-gated on the very setting that just changed) that don't point at
the real cause and don't go away with `--clean` unless you also clear
`arduino-cli`'s separate sketch-build-cache directory
(`%LOCALAPPDATA%\arduino\sketches\<hash>\`).

## Minimal reproduction

Using the environment above, with a custom `lv_conf.h` at the library
root as described:

1. `arduino-cli compile --clean` with `LV_USE_LOG 0` in `lv_conf.h` --
   builds fine, establishes a cache.
2. Change `lv_conf.h`: `LV_USE_LOG 0` &rarr; `1`. Add a one-line call to
   `lv_log_register_print_cb(...)` somewhere in the sketch (this
   function's entire declaration/definition in LVGL is `#if LV_USE_LOG`-gated).
3. `arduino-cli compile` again, **without** `--clean`.

Frequent hangs occurred and clearly observed problems through many
iterations. Reproducible errors over multiple runs -- can share logs if
needed. `arduino-cli --verbose` logs `Using cached library
dependencies for file: .../lvgl/src/misc/lv_log.c` (i.e. it did not
recompile), and the build fails with:

```
<sketch>.ino:NNN:3: error: 'lv_log_register_print_cb' was not declared in this scope
```

-- even though `lv_conf.h` was just edited to enable exactly that
symbol. A `#pragma message` probe placed at the top of `lv_log.c`
stringifying `LV_USE_LOG` confirms the value it sees never changed
across the edit.

Reverse confirmed: after working around the root cause
locally (see below), the same two-step edit correctly triggers a fresh
recompile of `lv_log.c` and the build succeeds.

## Workaround used locally

Forcing an absolute `LV_CONF_PATH` at the top of `lv_conf_internal.h`
(before the `__has_include` probe) makes every translation unit resolve
the exact same file, unconditionally, sidestepping both the layout
mismatch and the per-file `__has_include` inconsistency:

```c
#ifndef LV_CONF_PATH
    #define LV_CONF_PATH "C:/full/absolute/path/to/lvgl/lv_conf.h"
#endif
```

## Upstream fixes (not prescriptive)

- Document, in the Arduino-specific setup instructions, that
  `LV_CONF_PATH` should be set explicitly (e.g. via a sketch-local
  `mbed_app.json`'s `macros`, or a build flag) rather than relying on
  the `lv_conf.h`-next-to-`lvgl` convention, specifically for the
  Arduino Library Manager install layout.
- Extend the fallback chain to also try one level up
  (`"../lv_conf.h"`) in addition to two, to match how Arduino actually
  lays libraries out.
- Consider whether the silent-default fallback should be noisier when
  it happens for *some* files but not others within the same build --
  e.g., a build-time static_assert comparing a canary value between
  `lv_conf.h` and a value baked into a core library file, so a
  mismatch fails loudly instead of silently picking different heap
  backends for different translation units.

Can provide the full `arduino-cli compile --verbose` logs for
either reproduction step, or test a candidate fix against this same
setup.

# Giga YH Controller LVGL

## Purpose

Firmware for an Arduino Giga R1 WiFi that arbitrates time-of-use home power
flow. It sits between a Home Assistant current monitor (which knows signed
grid power, i.e. import vs. export) and two Y-H grid-tied inverters (L1/L2 of
a US split-phase 220V system) that only accept an absolute-value output
command. The goal is to keep grid export close to zero by continuously
trimming inverter output based on MQTT setpoints from Home Assistant, while
independently reading back each inverter's actual local output over RS485 for
display/verification.

This is a single-file Arduino sketch (`Giga_YH_Controller_LVGL_1.0.9.ino`,
~865 lines) — there is no build system, package manifest, or test suite.
Treat the `.ino` file as the entire codebase. See `README.md` for the
project-level overview (both boards) and Unit 2's screens/screenshots.

## Hardware / topology

- **MCU**: Arduino Giga R1 WiFi + Giga Display Shield (800x480 touch, driven
  via LVGL) + `Arduino_GigaDisplayTouch`.
- **RS485↔TTL converters** on `Serial1` (L1) and `Serial2` (L2) at 4800 baud,
  talking to the Y-H current-loop feedback hardware using an 8-byte
  fixed-frame protocol (sync bytes `0x24 0x56`, big-endian power in bytes
  4–5, checksum in byte 7 as `264 - sum(bytes) mod 256`).
- **MQTT** (via `ArduinoMqttClient`) to a Home Assistant broker: subscribes
  to `V1.0/Home/PowerFeeder/Line{1,2}Set` for signed power deltas from HA's
  current monitor, and (unit 1 only) publishes current output to
  `V1.0/Home/PowerFeeder/Line{1,2}`.
- **NTP** over UDP for wall-clock display (no DST handling; fixed
  `timezone = -7`).
- Credentials (`SECRET_SSID`, `SECRET_PASS`, `HOME_ASSISTANT_IP`,
  `MQTT_USERNAME`, `MQTT_PASSWORD`) live in a gitignored `arduino_secrets.h`
  per board — see the header comment in each `.ino` for the expected
  `#define` list.

## Runtime model

Everything is a single-threaded, non-blocking `loop()` driven by `millis()`
comparisons — there is no RTOS/task scheduler. Per iteration it:

1. Byte-bangs incoming RS485 frames off `Serial1`/`Serial2` into `buffer1`/
   `buffer2`, validating the 2-byte sync pattern once 8 bytes have
   accumulated.
2. Runs a ~550ms watchdog per line (`timer485_L1`/`timer485_L2`) that forces
   an output re-send even without new input, so the inverter always sees a
   heartbeat.
3. Validates the RS485 checksum, extracts `Power1`/`Power2` (local measured
   output).
4. Sends the current `L1_Output`/`L2_Output` back out over RS485 when
   `bSendMessage{1,2}` is set (new valid input, or watchdog fire).
5. On new MQTT power data (`b_New_L{1,2}_Power`, set from `onMqttMessage`),
   adjusts `L{1,2}_Output` by accumulating the signed MQTT value into the
   existing output, clamped to `[0, MAX_POWER]`, gated to at most once per
   30 seconds per line, and gated entirely during the `STARTUP_DELAY`
   window after boot.
6. Refreshes the LVGL display roughly once a second, and unconditionally on
   any output/input change.
7. Publishes current output back to MQTT every 5 loop ticks (unit 1 only).
8. Calls `mqttClient.poll()` to service MQTT keepalives/callbacks.

`UNIT_NUMBER` distinguishes a primary controller (1, subscribes+publishes)
from secondary units (>1, subscribe-only) — only one instance is meant to
publish status back to HA.

## Key constants (top of file)

- `MAX_POWER` (900W) — per-inverter output ceiling.
- `STARTUP_DELAY` (seconds) — hold output at 0 after boot until the grid tie
  is confirmed active; currently `10` (confirmed intentional by the hardware
  owner, despite an earlier changelog entry describing a 5-minute intent).
- `L1_CorrectionFactor` / `L2_CorrectionFactor` (0.683966) — scales the
  Y-H current-loop reading to actual watts, compensating for the
  non-matching CT winding factor/diameter noted in the file header.
- `EMA_ALPHA` (0.35) — smoothing factor on the incoming MQTT power reading,
  derived from the measured real Refoss→HA report cadence (~15.8s) for an
  effective ~35s time constant. Part of the 2026-07-11 oscillation fix below.
- `MAX_STEP_PER_UPDATE` (100W) — caps how much a single 30s update may
  change `L{1,2}_Output` by. A conservative starting default grounded in the
  incident log, not a hardware-verified limit — revisit as real-world
  behavior is observed. Also part of the oscillation fix.

## Fixed oscillation incident (2026-07-11, v1.0.10)

The controller rang into sustained large-amplitude oscillation for ~90
minutes, root-caused against a Home Assistant recorder export: HA publishes
one whole-household signed reading (`total_power_from_grid`) identically to
*both* `Line1Set` and `Line2Set`, but each line's accumulator applied the
full value independently — roughly doubling the correction actually needed.
Combined with no filtering on the incoming value and no per-update step
cap, each inverter's own output swing measurably moved
`total_power_from_grid` again within seconds, closing a tight feedback loop
that rang instead of settling. Fixed by halving the per-line correction,
EMA-smoothing the input (`EMA_ALPHA`), and capping step size
(`MAX_STEP_PER_UPDATE`) — verified by replaying the actual logged sequence
through old vs. new accumulator logic (old: 62/719 points pinned ≥800W;
new: 0/719, stdev roughly halved).

**Note:** Unit 2's HIL testing later confirmed `Line1Set`/`Line2Set` are
actually *independent* signed per-line readings, not duplicates of one
value (see Unit 2 section below) — **this fix's halving logic needs
re-verification against that corrected understanding**, not yet done.

## Known rough edges still open

- **`L1_Power`/`L2_Power` cross-line "balance" logic was removed, not
  fixed** — it gated on two never-assigned dummy placeholders, so the
  condition was unconditionally true. Per the hardware owner: with the
  feeder running, `L1 + L2` should trend toward 0; the real cross-line
  coupling needs to be re-derived from logged data, not guessed. See the
  `TODO(L1/L2 balance)` comments in `loop()`.
- **No staleness/failsafe timeout on MQTT input** — intentional (hold last
  known output indefinitely if HA/broker/WiFi drops, rather than ramping to
  0). Worth revisiting if the deployment profile changes.
- WiFi/MQTT connect failures at boot still hang forever (`while(true);` /
  `while(1);`) with no reconnect/watchdog-reset path, and there's no WiFi
  reconnect logic if the link drops after `setup()`.
- `Arduino String` is still used in the MQTT/WiFi hot path — a heap
  fragmentation risk on a long-uptime device. Left as-is; a bigger
  behavioral change that wants real hardware verification before touching.

## Conventions / gotchas for future edits

- L1 and L2 are handled by fully duplicated, parallel blocks of global
  state and logic rather than a shared struct/function — when fixing a bug
  in one line's handling, check whether the mirrored L2 (or L1) block has
  the same bug.
- Display strings are built with `sprintf`/`strcat` into fixed `char[]`
  buffers and pushed to LVGL labels with `lv_label_set_text`; label color
  markup uses LVGL's `#RRGGBB text#` recolor syntax (recolor enabled via
  `lv_label_set_recolor`).
- `verbosity` (0–255) gates `Serial` logging verbosity; `trace` is a
  second, separate always-on/off debug flag used inconsistently alongside it.
- `VERSION_POWER` (shown on-screen) is kept in sync with the file's own
  changelog — bump both together so the on-screen version reflects the
  running build. Same pattern for Unit 2's `VERSION_DASHBOARD` below.

## Unit 2 — remote display dashboard (sibling sketch)

`Giga_YH_Dashboard_Unit2/Giga_YH_Dashboard_Unit2.ino` — second physical
Giga board + Display Shield, `UNIT_NUMBER 2`: subscribe-only, no RS485, no
publish, no control authority. Safe to iterate on freely without touching
Unit 1's control loop. Fully implemented (touch-navigable Home + five
detail screens) — see `README.md` for the current screens/interactions
with real screenshots; the `.ino`'s own header comment has full
scope/changelog history. `VERSION_DASHBOARD` tracks its own changelog
independently of `VERSION_POWER` above.

All real data, display-only, no control path: TOU rates/schedule (8
hardcoded 2026 holiday dates), NOAA tide predictions for La Jolla (station
9410230, `tide_data_2026.h`, regenerate via
`tools/tide_data/convert_tide_data.py` for a future year), real per-line
grid power via `Line1Set`/`Line2Set` MQTT topics, and weather/sun/moon on
the Almanac screen (and the Home quadrant's weather pill) -- see the
v1.0.58 paragraph below.

**`Line1Set`/`Line2Set` are independent signed per-line grid readings**,
not a duplicated whole-household value (confirmed via HIL testing
2026-07-13) — one line can import while the other exports; true net
household consumption is their sum. This contradicts the assumption behind
Unit 1's 2026-07-11 oscillation fix (see above) — **that halving logic
needs re-verification**, flagged but not yet done.

**LVGL 9.5.0 requires local patches — see
[`Giga_YH_Dashboard_Unit2/lvgl_patches/README.md`](Giga_YH_Dashboard_Unit2/lvgl_patches/README.md).**
Two bugs behind a board hang on repeated "Change WiFi"/"Change MQTT":
(1) an unchecked `lv_realloc()` upstream
([lvgl/lvgl#9794](https://github.com/lvgl/lvgl/issues/9794)) that can
corrupt memory instead of failing cleanly, and (2) `Arduino_H7_Video`
(the Giga core's own display helper) shipping a bundled `lv_conf.h` that
shadows any user-supplied one via `__has_include`, leaving LVGL silently
on a 64KB heap instead of the intended 512KB SDRAM-backed pool. **If
LVGL is ever reinstalled or updated, the patches in `lvgl_patches/` must
be reapplied** (or re-verified as fixed upstream). `logMemStatus()`
(calls `lv_mem_monitor()`) logs after every Change WiFi/Change MQTT flow
as an ongoing regression signal — watch `free_size`/`frag_pct` if hangs
ever recur.

Screen exporter tool (`tools/dump_screen.py` + `tools/capture_all.py`):
dumps the live LVGL framebuffer over serial (trigger byte `'D'`) and
reconstructs real PNGs on the PC side — use this to verify actual rendered
layout instead of guessing from source. `tools/sim_tap.py` / the `'P'`
serial command drive a second simulated-touch indev for scripted
regression testing without hands on the board. `tools/captures/` is a
running dev/debug archive (append-only, prune old PNGs periodically, keep
`capture_log.csv`); `Giga_YH_Dashboard_Unit2/docs/screenshots/` holds the
small, clean, currently-referenced-by-README set — don't dump debug
captures there.

**Before publishing any dashboard screenshot**: blur/redact WiFi SSIDs
(own and neighbors', e.g. in the WiFi scan-list screen) and never show a
real password — use a dummy like `YourPasswordHere`.

**Arduino auto-prototype-generation + custom types**: the build system
inserts function prototypes near the top of the translation unit, before
any custom struct/enum defined later in the file. Any function taking a
custom type as a parameter/return will fail to compile unless that type is
defined before the insertion point — keep all custom struct/enum
definitions (`TouTier`, `TouStatus`, `TidePoint`, etc.) in a block
immediately after the `#include`s, not inline near first use.

**LVGL default-theme padding**: a plain `lv_obj_create()` container gets
non-zero padding from the active theme unless explicitly zeroed with
`lv_obj_set_style_pad_all(obj, 0, 0)`. Unzeroed padding silently shrinks
the content area and shifts children — looks exactly like a sizing bug on
the *child* when the real cause is the *parent*. Zero padding explicitly
on every new container.

**Text clipping that matches the background color is invisible to simple
pixel-band scanning.** Verify new/resized text containers via zoomed
(4–6x) crops of the actual glyph shapes from a real capture, and confirm
real pixel clearance, not just an ink-vs-gap scan or a thumbnail eyeball.

**Almanac screen is fully real as of v1.0.58** — weather (Met.no), sun,
and moon (rise/set/phase/illumination) all come from a new HA pyscript
automation (`homeassistant/pyscript/almanac_data.py`, uses `skyfield` —
see its own header comment for the required `requirements.txt` and a
real pyscript import quirk it works around) publishing to
`V1.0/Home/Almanac/Data` every 6 hours. The moon-phase icon is a real
geometric rendering (`lv_canvas`, terminator-ellipse construction from
the actual phase angle), not one fixed picture. Externally-sourced
epochs (this topic's rise/set times) need `formatEpochTime()`'s
`+3600*timezone` adjustment before display — this board's RTC is set to
an already-local-shifted epoch (see `parseNtpPacket()`), not true UTC,
unlike what you'd assume from a genuinely-UTC source.

**Open tickets (parked, not yet implemented)**:
- Unit 1 oscillation-fix halving logic re-check (see above, twice-flagged).
- Occasional double-tap/slow physical touch input, unresolved — GitHub
  issue #1.

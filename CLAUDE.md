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
Treat the `.ino` file as the entire codebase.

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
  that is not checked in — see the header comment in the `.ino` for the
  expected `#define` list.

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
6. Refreshes the LVGL display (clock/status/power panels) roughly once a
   second, and unconditionally on any output/input change.
7. Publishes current output back to MQTT every 5 loop ticks (unit 1 only).
8. Calls `mqttClient.poll()` to service MQTT keepalives/callbacks.

`UNIT_NUMBER` distinguishes a primary controller (1, subscribes+publishes)
from secondary units (>1, subscribe-only) — only one instance is meant to
publish status back to HA.

## Key constants (top of file)

- `MAX_POWER` (900W) — per-inverter output ceiling.
- `STARTUP_DELAY` (seconds) — hold output at 0 after boot until the grid tie
  is confirmed active; currently `10`, though changelog v1.0.5 describes a
  5-minute (300s) intent — verify this value matches the deployed hardware's
  actual grid-tie activation time before relying on it.
- `L1_CorrectionFactor` / `L2_CorrectionFactor` (0.683966) — scales the
  Y-H current-loop reading to actual watts, compensating for the
  non-matching CT winding factor/diameter noted in the file header.
- `EMA_ALPHA` (0.35) — smoothing factor applied to the incoming MQTT power
  reading in `onMqttMessage`, derived from the measured real Refoss→HA
  report cadence (~15.8s between readings in logged data) for an effective
  ~35s smoothing time constant. Part of the 2026-07 oscillation fix, see
  below.
- `MAX_STEP_PER_UPDATE` (100W) — caps how much a single 30s update may
  change `L{1,2}_Output` by. A conservative starting default grounded in
  the incident log, not a hardware-verified limit — revisit once
  real-world behavior after this fix can be observed. Also part of the
  2026-07 oscillation fix.

## Fixed in the cleanup pass (2026-07)

The initial review found several concrete bugs and a pile of dead
declarations left over from earlier prototyping. These are now fixed:

- RS485 receive buffers (`buffer1`/`buffer2`) now reset `tail{1,2}` to 0
  after *every* fully-received 8-byte frame, valid or not. Previously the
  reset only happened on a successful send, so a run of checksum failures
  could walk `tail1`/`tail2` past the sync window toward the 12-byte array
  bound — a real out-of-bounds write risk.
- `onMqttMessage`'s `tbuf[256]` fill loop is now bounds-checked, with any
  excess bytes past capacity drained (not stored) so an oversized MQTT
  payload can't overflow the buffer or desync the next message.
- Fixed `Serial.print(' vs. ')` (a multi-char literal, prints garbage) →
  `Serial.print(" vs. ")`.
- Dropped duplicate `#define UNIT_NUMBER`, unused `COLLECTOR_TIME_CONSTANT`,
  and a long tail of declared-but-never-used globals (`lastSyncTime`,
  `lastClockTime`, `elapsedTime`, the inert `WiFiServer server(80)`,
  `interval`/`previousMillis`, `count`, `printInterval`/`printNow`, `num`,
  `receivedMessage`/`sendMessage`/`receivedMessage_Hex`, `buffsize{1,2}`,
  `head{1,2}`, `L1_LastPower`, `L1_CorrectedPower`/`L2_CorrectedPower`,
  `L1_State`/`L2_State`, `SetpointPower{1,2}`, `wifi_string`, `sz_time`) and
  the never-called `RTCset()`.
- `VERSION_POWER` now reads `"1.0.9"`, matching the file/header.
- Cosmetic: explicit parens around the checksum's `-`/`&` mix (was
  numerically fine but easy to misread), and `trunc(x/256)` replaced with
  `x >> 8`.

## Fixed oscillation incident (2026-07-11, v1.0.10)

The controller went into a sustained large-amplitude oscillation on
2026-07-11 (~18:24–19:59 Pacific), resolved at the time by a manual
hardware reset. Root-caused by analyzing a Home Assistant recorder export
(`total_power_from_grid`, `l1_feeder_current_consumption`,
`l2_feeder_current_consumption`) against the running code:

- `total_power_from_grid` is **one** whole-household signed reading, but
  Home Assistant publishes it identically to **both** `Line1Set` and
  `Line2Set` (confirmed by the hardware owner). Each line's accumulator
  was applying the *full* value independently
  (`tOutput = L1_Output + f_L1_Power`), so the two lines' combined
  response was roughly double the correction actually needed to zero out
  that one shared reading.
- No filtering existed on the incoming value — `f_L1_Power`/`f_L2_Power`
  were overwritten directly from the latest MQTT sample, so a single
  noisy/transient reading (the log shows single-minute windows swinging
  from about -800W to +1150W during the incident) went straight into the
  accumulator at full weight.
- No maximum-step-size limit existed — only a *minimum* time between
  updates (30s), not a cap on how large one update's change could be.
  Output was observed jumping ~400W in a single ~10-second step.

With effectively doubled gain, no filtering, and no step limit, each
inverter's own large output swing measurably moved
`total_power_from_grid` again within seconds, closing a tight physical
feedback loop that rang instead of settling.

Fixed in v1.0.10 (`onMqttMessage` and the `b_New_L{1,2}_Power` blocks in
`loop()`): the per-line correction is halved (`f_L{1,2}_Power / 2.0`),
the incoming value is EMA-smoothed (`EMA_ALPHA`) before use, and a single
update's output change is capped (`MAX_STEP_PER_UPDATE`). Verified by
replaying the actual logged `total_power_from_grid` sequence through both
the old and new accumulator logic in a standalone simulation: old logic
pins to `MAX_POWER` repeatedly during the incident window (62/719 sampled
points ≥800W, stdev ≈254W), new logic never approaches the cap on the
same input (0/719 ≥800W, stdev ≈126W).

## Planned dashboard redesign (design only — not yet implemented)

The current display (`setup()`'s 2x2 `lv_obj_create`/`lv_label_create`
grid, four static labels) is slated for a touch-navigable redesign:
a minimal glance Home screen plus five detail screens (Time & rates,
Connection, Battery, Grid flow, Almanac) reachable by tapping —
`Arduino_GigaDisplayTouch` is already initialized in `setup()` but
currently wired to nothing.

This was designed through an extended mockup/critique process *before*
any LVGL code was written, including corrections driven by real logged
data (`batterydata.csv`-derived charge/discharge curves, calendar
verification of weekday vs. weekend time-of-use schedules). The full
plan, screen-by-screen data-source status (what's free vs. buildable vs.
genuinely missing), LVGL widget mapping, and phased build order live in
[`design/lvgl_redesign_plan.md`](design/lvgl_redesign_plan.md).

A self-contained, clickable HTML prototype of every screen — the actual
visual reference, not just a description — is at
[`design/mockups/giga_dashboard_mockup.html`](design/mockups/giga_dashboard_mockup.html).
Open it directly in any browser; no build step or server needed.

Read the plan doc before starting implementation — it explicitly flags
that this touches the same file as the safety-relevant RS485/MQTT
control loop (see the 2026-07-11 oscillation incident above, itself a
timing/responsiveness bug), so new UI work needs to be verified not to
compete with that loop for cycle time the way the sibling
`ESP32_Fuel_Gauge` project's old blocking LED animation did.

## Known rough edges still open

- **`L1_Power`/`L2_Power` cross-line "balance" logic was removed, not
  fixed.** It gated on `L1_Power`/`L2_Power`, which were declared and never
  assigned (dummy placeholders from prototyping) — so the condition was
  unconditionally true rather than actually checking anything. Per the
  hardware owner: with the feeder running, L1 reads positive and L2 reads
  negative such that `L1 + L2` should trend toward 0, and the real
  cross-line coupling (plus P/I/D-style constants) needs to be re-derived
  from logged input/output data rather than guessed. See the
  `TODO(L1/L2 balance)` comments in the `b_New_L1_Power`/`b_New_L2_Power`
  blocks in `loop()`.
- **No staleness/failsafe timeout on MQTT input** — intentional per the
  hardware owner (hold last known output indefinitely if HA/broker/WiFi
  drops, rather than ramping to 0). Worth revisiting if the deployment
  profile changes.
- WiFi/MQTT connect failures at boot still hang forever (`while(true);` /
  `while(1);`) with no reconnect/watchdog-reset path, and there's no WiFi
  reconnect logic if the link drops after `setup()`.
- `Arduino String` is still used in the MQTT/WiFi hot path
  (`onMqttMessage`, `wifi_info`, topic publish concatenation) — a heap
  fragmentation risk on a long-uptime device. Left as-is in this pass since
  it's a bigger behavioral change that really wants compile/hardware
  verification (no `arduino-cli`/toolchain was available to build-check
  this pass).
- `STARTUP_DELAY` stays at `10` seconds (confirmed intentional by the
  hardware owner, despite the v1.0.5 changelog describing a 5-minute
  intent).

## Conventions / gotchas for future edits

- L1 and L2 are handled by fully duplicated, parallel blocks of global
  state and logic rather than a shared struct/function — when fixing a bug
  in one line's handling, check whether the mirrored L2 (or L1) block has
  the same bug.
- Display strings are built with `sprintf`/`strcat` into fixed `char[]`
  buffers and pushed to LVGL labels with `lv_label_set_text`; label color
  markup uses LVGL's `#RRGGBB text#` recolor syntax (recolor is enabled via
  `lv_label_set_recolor`).
- `verbosity` (0–255) gates `Serial` logging verbosity; `trace` is a second,
  separate always-on/off debug flag used inconsistently alongside it.
- `VERSION_POWER` (shown on-screen) is kept in sync with the file's own
  changelog (currently `"1.0.10"`) — bump both together going forward so
  the on-screen version reflects the running build.

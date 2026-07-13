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
- `COLLECTOR_TIME_CONSTANT` (30) — declared but not read anywhere in the
  current version; the effective per-line rate limit is the hardcoded
  `30000` ms literal in the L1/L2 update blocks.
- `L1_CorrectionFactor` / `L2_CorrectionFactor` (0.683966) — scales the
  Y-H current-loop reading to actual watts, compensating for the
  non-matching CT winding factor/diameter noted in the file header.

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
- `VERSION_POWER` (shown on-screen) is currently `"1.0.7"` while the
  filename/header comments say `1.0.9` — bump this together with the
  filename/header version going forward so the on-screen version reflects
  the running build.

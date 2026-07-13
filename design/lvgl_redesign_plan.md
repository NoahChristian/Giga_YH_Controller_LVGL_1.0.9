# Dashboard redesign plan

Companion to [`mockups/giga_dashboard_mockup.html`](mockups/giga_dashboard_mockup.html)
(open it directly in any browser — it's a self-contained, clickable
prototype of every screen described below) and to `CLAUDE.md`, which
this plan is linked from.

## Why

The current display is four static `lv_label`s with no touch
interaction (`Arduino_GigaDisplayTouch` is initialized but never wired
to anything). This plan replaces it with a touch-navigable dashboard: a
minimal glance screen plus detail screens reachable by tapping, designed
through an extended mockup/critique cycle (dark theme, ring/needle
gauges, a real TOU schedule, charts grounded against real logged data)
before any LVGL code gets written.

## Screen inventory

| Screen | Shows | Reached from |
|---|---|---|
| Home (top level) | Clock, weather teaser, TOU status pill, connection status, battery ring + charge state, net-flow needle gauge — 2x2 grid, minimal by design | boot |
| Time & rates | Full clock/date, 3-tier TOU schedule bar with "now" marker, rate + time-until-tier-change | tap Home's time panel |
| Connection | SSID, signal, IP, broker status, per-topic MQTT last-seen | tap Home's connection panel |
| Battery | Larger ring, charge/discharge magnitude, a day's SoC curve colored by charge (green, super off-peak) vs. discharge (amber) | tap Home's battery panel |
| Grid flow | Larger needle gauge, L1/L2 breakdown, today's savings, flow trend | tap Home's needle gauge panel |
| Almanac | Temperature/condition, sunrise/sunset, moonrise/moonset + phase, tide chart | tap the weather teaser specifically (its own link, not the whole time panel) |

## Visual system (already settled — see mockup for exact values)

- Dark theme: near-black screen (`#0b0c0e`), light/thin (400-weight)
  typography for values, muted grays for captions.
- Color carries fixed meaning everywhere, not per-screen: teal =
  battery/neutral, green = cheap/charging/good, amber =
  costly/discharging/caution, red = on-peak/most expensive, blue = the
  one tide-specific accent.
- Gauges: `lv_arc`-style ring for single percentages, a semicircular
  needle gauge for bidirectional values (net import/export is the only
  bidirectional reading in the set).
- Every screen (except Home) opens with a small back-to-Home affordance,
  top-left.

## Data source status

Not everything shown is live data yet — the mockup makes every field's
status explicit so this can't get lost in translation to code.

**Free — already computed or available in the current `.ino`, just not displayed this way:**
- `WiFi.SSID()`, `WiFi.RSSI()` → Connection screen
- `Power1`/`Power2` (RS485 readback), `L1_Output`/`L2_Output` → Grid flow screen's L1/L2 breakdown
- The TOU schedule itself is pure logic, no external dependency — see below.

**Buildable now, pure firmware logic, no new data source needed:**
- Time-of-use tier (super off-peak / off-peak / on-peak) and color, per this schedule confirmed against 2026-07-13's actual calendar day (Monday = weekday):
  - **Super off-peak (green)**: weekdays 12am-6am and 10am-2pm; weekends 12am-2pm (single block)
  - **On-peak (red)**: 4pm-9pm, every day
  - **Off-peak (orange)**: everything else
  - This is a fixed schedule approximation (stated as "good for summer daytime, a good approximation year-round" per the person who spec'd it) — a lookup table keyed on hour + weekday/weekend, no sensor needed.

**Needs a new MQTT subscription, but the data already exists elsewhere in this home automation setup:**
- Battery SoC/charge state: `V1.0/Home/Battery/SoC` and `V1.0/Home/Battery/Action` — already published by the sibling `ESP32_Fuel_Gauge` project's data source, this device would just need to subscribe.
- Per-topic MQTT freshness (the "Line1Set: 8s ago" style readout on the Connection screen): needs the firmware to track a last-received timestamp per subscribed topic — straightforward to add, not currently present.

**Genuinely new — no data source exists anywhere in scope yet:**
- Weather (temp/condition)
- Sunrise/sunset, moonrise/moonset, moon phase
- Tide chart
- Today's savings calculation
- All of these would realistically come from Home Assistant (weather/sun/tide integrations already exist there) republishing to new MQTT topics, rather than the Arduino computing any of this itself — ephemeris/tide math is not something to implement on an MCU.

## LVGL implementation mapping

| Mockup element | LVGL widget | Notes |
|---|---|---|
| Ring gauges (battery) | `lv_arc` | straightforward |
| Needle gauge (grid flow) | `lv_meter` / `lv_scale` | supports colored zone arcs + needle natively |
| Line charts | `lv_chart` | multi-color segments (the battery chart's green/amber split) need either overlaid series or per-point styling — not a single property, budget real time for this |
| Screen navigation | multiple `lv_scr_load()` screens, or one swappable container | mockup uses the latter (innerHTML swap); either maps fine to LVGL |
| Touch-to-navigate | `LV_EVENT_CLICKED` callbacks on each panel's root object | `Arduino_GigaDisplayTouch` is already initialized in `setup()`, currently wired to nothing |
| Icons (cloud, sunrise, arrows, chevrons) | custom icon font via LVGL's font converter, or `lv_img` bitmaps, or hand-drawn `lv_line`/`lv_arc` shapes (what the standalone mockup HTML does, to stay dependency-free) | no icon font is currently embedded in this firmware; budget setup time |
| Dark color theme | an LVGL style sheet (`lv_style_t`) built from the palette above | one-time setup, then reused across every screen |

## Phased build order

Phase 1 — prove the foundation on real hardware before adding anything visually rich:
1. Home screen + basic 2x2 grid layout, dark theme, static/placeholder values.
2. Wire touch input: tap navigation between Home and one detail screen (Connection — it's the simplest, all its data is already free).
3. Confirm this doesn't regress the RS485/MQTT control loop's timing — this is the same file as the safety-relevant power-control logic; UI work must not compete with it for loop-cycle time the way the old blocking LED animation did on the sibling fuel-gauge project.

Phase 2 — the rest of the "free" and "buildable now" data:
4. Grid flow screen (L1/L2 breakdown is free data).
5. Time & rates screen with the real TOU schedule logic.
6. Battery screen wired to the sibling project's existing `Battery/SoC`/`Battery/Action` topics.

Phase 3 — only after Phase 1-2 are solid on real hardware:
7. Chart widgets (`lv_chart` for the battery curve, colored by TOU tier).
8. Icon font/asset setup.
9. Per-topic MQTT freshness tracking on the Connection screen.

Phase 4 — explicitly deferred, needs new data sources decided first:
10. Almanac screen (weather/sun/moon/tide) — blocked on deciding where that data comes from (most likely: new Home Assistant integrations republishing to new MQTT topics).
11. Today's savings calculation — needs a defined formula, not just a data source.

## Verification

No `arduino-cli`/toolchain has been available in this environment for
any change made to this repo so far (see `CLAUDE.md`) — every phase
above needs real on-device testing, particularly Phase 1 step 3 (control
loop timing) given this project's history with the 2026-07-11 oscillation
incident that was itself a timing/responsiveness bug.

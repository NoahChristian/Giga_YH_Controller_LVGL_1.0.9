//Giga YH Dashboard - Unit 2 (remote display)
//Author: Noah Christian (C) 2026
//Spiral 1 + post-hardware fix batch
//
//Purpose: A touch-navigable status dashboard for the Giga_YH TOU-optimization
//  home automation system, running as UNIT_NUMBER 2 -- subscribe-only, never
//  publishes, never touches RS485, never writes an output command. It has no
//  control authority over anything; it only displays. This is deliberate: it
//  lets the dashboard UI (new, untested on real hardware) be proven out with
//  zero blast radius on the actual power controller (Giga_YH_Controller_LVGL_1.0.9,
//  UNIT_NUMBER 1), which has its own timing-sensitive RS485/MQTT control loop
//  that this sketch must never compete with -- because it's a fully separate
//  physical board, it structurally can't.
//
//Design reference: ../design/lvgl_redesign_plan.md and the mockups under
//  ../design/mockups/ (01_home.svg .. 06_almanac.svg) are the visual source
//  of truth this sketch is implementing. Read those before changing layout,
//  colors, or screen structure here.
//
//Spiral 1 scope (see plan doc for what's deliberately NOT here yet):
//  - All six screens exist and are reachable by touch: Home, Time & rates,
//    Connection, Battery, Grid flow, Almanac.
//  - Connection screen shows real data (SSID, RSSI, MQTT broker state) --
//    free, no MQTT subscription needed.
//  - Subscribed (not published -- this device has no publish authority,
//    ever) to the sibling ESP32_Fuel_Gauge project's own Battery/SoC and
//    Battery/Action topics, so the battery ring/state on both the Home
//    and Battery screens are real once a reading arrives.
//  - Also subscribed to Unit 1's own Line1/Line2 output-readback topics
//    (the feeder columns) AND to Line1Set/Line2Set (Home Assistant's real
//    per-line grid readings, independent signed values -- see
//    g_line1GridPower/g_line2GridPower below). The Grid flow screen's
//    right-hand ring is their sum, a genuine whole-household net-flow
//    reading, not a proxy.
//  - Real TOU rates/schedule (Home + Time & rates screens) and real NOAA
//    tide predictions (Almanac screen) are also wired in, display-only --
//    see the TOU/tide sections below. Weather is still a static
//    placeholder.
//  - No icon font, no lv_meter needle gauge, no lv_chart -- this sketch uses
//    LVGL's default font and simple lv_arc/lv_obj/lv_line primitives instead,
//    to stay on widgets that are stable across LVGL v7/v8/v9.
//
//Post-hardware fix batch (see plan doc "post-hardware fix batch" for the
//full list this addresses): centers/aligns text via LVGL alignment
//primitives instead of hand-guessed absolute coordinates (the root cause of
//the overlap/overrun bugs found once this ran on a real screen); recolors
//battery/grid state text to the green(Idle)/blue(Discharging)/red(Charging)
//muted scheme; wires the Grid panel's Consuming/Bypassing status from real
//L1+L2 data; replaces the flat placeholder chart bars with real lv_line
//curves reusing the mockups' own point data; adds two new Connection screen
//rows (gateway/local IP); adds simple sun/moon icon shapes to the Almanac
//screen; gives this sketch an explicit MQTT client ID as a precaution
//against ID collisions with Unit 1.
//
// arduino_secrets.h should contain the following definitions (same
// convention as the Unit 1 controller sketch):
// #define SECRET_SSID "YourWiFiSSID"
// #define SECRET_PASS "YourWiFiPassword"
// #define HOME_ASSISTANT_IP "YourHomeAssistantIP"
// #define MQTT_USERNAME "YourMQTTUsername"
// #define MQTT_PASSWORD "YourMQTTPassword"

#include "Arduino_H7_Video.h"
#include "lvgl.h"
#include "Arduino_GigaDisplayTouch.h"
#include "lv_conf.h"

#include "mbed.h"
#include <mbed_mktime.h>
#include <math.h>
#include "kvstore_global_api.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include "arduino_secrets.h"
#include <ArduinoMqttClient.h>

//Real NOAA CO-OPS tide predictions for La Jolla, CA (station 9410230),
//covering all of 2026. Generated once via tools/tide_data/
//convert_tide_data.py from a direct NOAA API fetch -- see that script
//for regenerating a future year. Defines TideEvent and TIDE_EVENTS_2026.
#include "tide_data_2026.h"

//Arduino's build system auto-generates function prototypes and inserts
//them near the top of the translation unit, BEFORE any type defined
//later in this file -- any function using a custom struct/enum as a
//parameter or return type breaks ("X has not been declared") unless
//that type is already defined by this point. Hence these being all the
//way up here instead of next to the functions that use them below.
enum TouTier { TOU_SUPER_OFF_PEAK, TOU_OFF_PEAK, TOU_ON_PEAK };
struct MonthDay { uint8_t month; uint8_t day; };
struct TouStatus {
  TouTier tier;
  double rate;
  bool weekendOrHoliday;
  int nextBoundaryHour;  //24-hour; the hour this tier ends (24 = midnight)
};
struct TouSegment { int startHour; int endHour; lv_color_t color; };
struct TouWindow { int startHour; int endHour; TouTier tier; };
struct TidePoint { int minuteOfDay; float heightFeet; bool isHigh; };
//see setConnStatusIndicator() -- drives the Home quadrant's and Connection
//screen's dot+label, replacing what used to be a build-time-only
//hardcoded green "Connected" dot.
enum WifiUiState { WIFI_UI_CONNECTING, WIFI_UI_CONNECTED, WIFI_UI_NOT_CONNECTED };
//orbit center/radius + tracked angle for the SoC ring's charge/discharge
//dot -- see updateOrbitDot() for why the angle is tracked as a float
//here instead of read back from LVGL.
struct OrbitDot {
  lv_obj_t* dot;
  int16_t cx, cy, r;
  float currentAngleDeg;
  int lastDirection;
  float lastPercent;
};

//These three are implemented in Arduino_H7_Video's own dsi.cpp (part of this
//same library, already linked in because Arduino_H7_Video.cpp itself calls
//them) but not declared in any header the sketch can see -- dsi.h lives in
//the library's src/ folder, not its public include path. Redeclaring them
//here with matching signatures lets the linker resolve them against the
//already-compiled library object. Used only by dumpFramebufferToSerial()
//below, a debug/dev feature: it lets me see what a screen actually looks
//like (resolution, real text width, real overlap) over the same serial
//port already used for logging, instead of relying on a hardware photo for
//every layout tweak.
uint32_t dsi_getActiveFrameBuffer(void);
uint32_t dsi_getDisplayXSize(void);
uint32_t dsi_getDisplayYSize(void);

#define UNIT_NUMBER 2
#define VERSION_DASHBOARD "1.0.48"
//version 1.0.0 - Spiral 1: all six screens built and touch-navigable,
//                Connection screen live (SSID/RSSI/broker state), everything
//                else placeholder. Did not compile (LVGL v8-style input
//                device driver API used against the actually-installed v9).
//version 1.0.1 - Fixed touch input driver for LVGL 9 (lv_indev_create/
//                lv_indev_set_read_cb replacing the removed lv_indev_drv_t
//                struct API). First clean compile against arduino:mbed_giga
//                4.6.0 + lvgl 9.5.0 + Arduino_GigaDisplayTouch 1.1.0.
//version 1.0.9 - Numbering corrected to continue from the prior Unit 2
//                lineage (Giga_LVGL_Tutorial_v1_0_8, inferred from that
//                project's folder name -- correct me if that's not the
//                right prior version) rather than restarting at 1.0.0.
//                Also added the boot-time Serial version banner below.
//                On real hardware: WiFi and MQTT broker connect confirmed
//                working; found and fixed a missing success-log message
//                that had looked like an indefinite hang in mqttClient.connect().
//version 1.0.10 - Subscribed (read-only) to Battery/SoC and Battery/Action --
//                 the same topics the sibling ESP32_Fuel_Gauge project
//                 publishes to. Battery ring/state on Home and Battery
//                 screens now reflect real data once a message arrives;
//                 the Battery screen's historical curve is still a
//                 placeholder. Still zero publish calls anywhere.
//                 Confirmed on real hardware: WiFi/MQTT connect, both
//                 subscriptions logged correctly by name.
//version 1.0.11 - Subscribed (read-only) to Unit 1's own Line1/Line2
//                 output-readback topics. Grid flow screen's L1/L2 numbers
//                 are now real. Still zero publish calls.
//version 1.0.12 - Post-hardware-review fix batch: replaced hand-guessed
//                 absolute label coordinates with LVGL alignment primitives
//                 and flex-row legend containers to fix the overlap/overrun
//                 bugs found on real hardware; added the green/blue/red
//                 muted battery-state color scheme (Idle/Discharging/
//                 Charging) to both the Battery and Grid panels via one
//                 shared helper; added isGridBypassed() to drive the Grid
//                 panel's Consuming/Bypassing status and watt total from
//                 real L1+L2 data; replaced the flat placeholder chart bars
//                 on the Battery and Almanac screens with real lv_line
//                 curves using the design mockups' own point data; added
//                 gateway/local IP rows to the Connection screen and split
//                 it into two columns; added simple sun/moon icon shapes to
//                 the Almanac screen; added an explicit MQTT client ID
//                 (mqttClient.setId) as a precaution -- not the expected fix
//                 for the missing Line1/Line2 data seen in testing, which is
//                 explained by Unit 1 not running during off-peak hours, not
//                 a subscription bug. Also added lightweight last-received
//                 timestamp bookkeeping per MQTT topic for future staleness
//                 detection. Confirmed on real hardware: boots, connects,
//                 subscribes, and parses Battery/SoC correctly with no
//                 regressions from this batch.
//version 1.0.13 - Widened the gap between the Home screen's weather pill and
//                 TOU-rate pill (Time quadrant) after a hardware photo showed
//                 their text visibly colliding -- the two pills' 40px
//                 top-to-top spacing wasn't enough clearance in practice.
//version 1.0.14 - Added a screen exporter (dumpFramebufferToSerial(),
//                 triggered by sending 'D' over Serial): dumps the live DSI
//                 framebuffer as raw RGB565 so a PC-side script
//                 (tools/dump_screen.py) can reconstruct a real PNG of
//                 whatever's on screen. Dev/debug only, not part of the
//                 dashboard's normal operation -- lets layout be checked
//                 against real rendered text metrics without needing a
//                 hardware photo for every tweak.
//version 1.0.15 - Added serial-triggered screen navigation ('H'/'T'/'C'/
//                 'B'/'G'/'M', dev/debug only) so every screen can be
//                 captured by the exporter without touching the board. Also
//                 fixed a real bug the exporter caught immediately: the
//                 Home screen's weather and TOU-rate pills were built with
//                 guessed fixed widths (190px/240px) narrower than their
//                 actual rendered text, so LVGL's default child-clipping
//                 was cutting text off both pills -- replaced with
//                 makeAutoPill(), which sizes the pill to its label's real
//                 content instead of a guess. Same fix applied to the Time
//                 & rates screen's larger TOU pill.
//version 1.0.16 - Used the exporter (tools/capture_all.py) to capture and
//                 inspect every screen for the first time and fixed
//                 everything it caught: two Unicode punctuation characters
//                 (en-dash, middle-dot) rendering as missing-glyph boxes on
//                 the default font, replaced with plain hyphens; the
//                 Connection screen's "MQTT broker"/"IP address" labels
//                 overlapping their own values (fixed with a new
//                 right-aligned fixed-width makeRowLabel() helper instead
//                 of a guessed gap); the Grid screen's "Saved today"
//                 caption clipped in a too-narrow stat column; the Home
//                 screen's battery-state placeholder text ("Waiting for
//                 data") clipped because it's deliberately left-justified
//                 with less room than centered text would have; and the
//                 Home screen's TOU pill overflowing its quadrant once the
//                 auto-pill fix let it size to its real (too-wide for this
//                 quadrant) content.
//version 1.0.17 - The 1.0.16 Connection-screen fix above introduced its own
//                 new bug, caught by re-capturing after that fix: the
//                 right-aligned label box was too narrow, so "MQTT broker"/
//                 "IP address"/"Firmware" wrapped onto a second line and
//                 collided with the row below instead of overlapping the
//                 value column. Widened the label box and switched
//                 makeRowLabel() to LV_LABEL_LONG_MODE_CLIP so a
//                 still-too-narrow label clips instead of wrapping and
//                 breaking row spacing. Screen-capture PNGs now get a
//                 timestamp in their filename (never overwritten) and are
//                 logged to tools/captures/capture_log.csv for an audit
//                 trail of what was captured and when.
//version 1.0.18 - The 1.0.17 fix above solved the wrapping but, by pushing
//                 rightValueX further right to make room, ran values like
//                 "Connected" and full IP addresses off the 800px screen
//                 edge instead -- caught by re-capturing again. Shifted the
//                 whole right column left instead of continuing to widen
//                 it rightward, so both the label box and the value column
//                 have real headroom on their own side.
//version 1.0.19 - 1.0.18's leftward shift also shrank rightLabelW from 210
//                 (already confirmed wide enough for "MQTT broker") down to
//                 180, silently clipping its leading "M" -- caught by
//                 re-capturing yet again. Kept the leftward shift but
//                 restored labelW to the already-confirmed-good 210. This
//                 three-iteration back-and-forth is exactly why the
//                 exporter exists: each fix got re-captured and checked
//                 instead of assumed correct.
//version 1.0.20 - Re-capturing the Grid screen after all the Connection
//                 screen churn above showed its "Saved today" column
//                 (widened to 180px back in 1.0.16) was STILL clipped --
//                 180px isn't enough for an 11-character string at this
//                 font, same lesson as "MQTT broker" needing 210px. Reused
//                 that confirmed 210px width instead of guessing a third
//                 number, and reflowed the L1/L2 columns to make room.
//version 1.0.21 - Widened the value-to-caption line spacing (26px -> 34px)
//                 in the Grid screen's L1/L2/Saved-today stat columns after
//                 you flagged it as visually tight, and applied the same
//                 30px gap to the Almanac screen's sunrise/sunset/moonrise/
//                 moonset columns, which used the same tight pattern.
//version 1.0.22 - Home screen's weather pill (Time quadrant) had its
//                 opaque background clipping into the descenders ('y' in
//                 "Monday"/"July") of the date label directly above it --
//                 pushed the pill (and the TOU pill below it, to keep their
//                 gap) down to clear it.
//version 1.0.23 - Found the real root cause of a worse bug: both TOU pills
//                 ("$0.38/kWh" on Home, the longer one on Time & rates) had
//                 their own text clipped at the top. Measured raw pixels
//                 from an exporter capture (not guesswork) and found the
//                 pill was both mispositioned (~20px off from its intended
//                 align offset) AND undersized (roughly half the expected
//                 content height) -- a known LVGL gotcha where
//                 LV_SIZE_CONTENT layout is deferred/batched, so reading a
//                 freshly created object's size (via lv_obj_align) right
//                 after creating it can see a stale, not-yet-resolved
//                 size. Fixed at the source in makeAutoPill() with
//                 lv_obj_update_layout(), which every pill on the dashboard
//                 uses, rather than papering over it with a bigger guessed
//                 padding value on just the two pills that happened to
//                 show it. Design rule going forward: never clip an
//                 ascender or descender; always verify at least 2px of
//                 clear spacing via the exporter, not by eye.
//version 1.0.24 - 1.0.23's lv_obj_update_layout() fix made no measurable
//                 difference (verified by re-measuring the same pixels --
//                 identical before/after), so the deferred-layout theory
//                 was wrong. Measured further and found the real
//                 discrepancy: two pills with identical padding computed
//                 very different LV_SIZE_CONTENT heights (~44px vs. ~26px),
//                 correlated with clickable vs. non-clickable state rather
//                 than anything about the text itself. Rather than keep
//                 chasing why, replaced LV_SIZE_CONTENT height with a
//                 fixed, generous height (40-44px) and lv_obj_center() for
//                 the label in makeAutoPill() -- a plain, unambiguous
//                 layout with no dependency on that quirk at all.
//version 1.0.25 - 1.0.24's explicit lv_obj_set_height() had NO effect
//                 either -- re-measured raw pixels and got byte-identical
//                 results to 1.0.23, confirming the call was silently
//                 ignored. Root cause: lv_obj_set_width(pill,
//                 LV_SIZE_CONTENT) appears to put the whole object in an
//                 auto-size mode that a later fixed lv_obj_set_height()
//                 doesn't override on its own. Fix: let the
//                 LV_SIZE_CONTENT auto-size resolve first via
//                 lv_obj_update_layout(), THEN call lv_obj_set_height() as
//                 the deliberately-last size-related operation.
//version 1.0.26 - 1.0.25's override-after-update_layout ALSO made no
//                 measurable difference (identical pixel measurements
//                 again), ruling out call-ordering entirely as the
//                 mechanism. Stopped trying to fix LV_SIZE_CONTENT on the
//                 pill container and instead measure a throwaway probe
//                 label's real width directly, then build the pill with a
//                 fully explicit (non-CONTENT) width/height from that
//                 measurement -- sidesteps whatever is actually wrong with
//                 CONTENT-sizing on this particular container/theme
//                 combination rather than continuing to fight it.
//version 1.0.27 - Found the REAL root cause after 1.0.26's fix also had
//                 zero visible effect, verified via a diagnostic build
//                 that changed the pill's own color (which DID show up,
//                 proving the code was running) while its measured pixel
//                 bounds stayed byte-identical regardless of any size
//                 change: q_time (and the other three Home quadrants, and
//                 every screen via makeScreenRoot()) never zeroed their
//                 OWN default padding, which the active theme sets to a
//                 non-zero value on plain lv_obj_create() containers. That
//                 silently shifted every absolute-position/aligned child
//                 on every screen by the same amount, AND shrank the
//                 quadrant's real usable content area -- so the TOU pill
//                 was being clipped by its own parent's real (smaller than
//                 assumed) boundary no matter what size the pill itself
//                 was given. Fixed with an explicit
//                 lv_obj_set_style_pad_all(obj, 0, 0) on makeScreenRoot()
//                 and all four Home quadrants.
//version 1.0.28 - Almanac screen: the sunrise/sunset/moonrise/moonset
//                 columns (95px tall) and the moon-phase row (30px tall)
//                 were both too short for their real content, clipping the
//                 BOTTOM of the caption line / the descenders on "Waxing
//                 gibbous"'s two g's. A same-color-as-background clip is
//                 invisible to a simple ink-vs-background pixel scan (it
//                 looks identical to the font just ending there) -- this
//                 one needed you pointing out specific glyph damage (a "u"
//                 losing its connecting bottom curve, reading almost like
//                 "ii") in a zoomed crop before it was traceable. Widened
//                 both containers and nudged the Tide label down to keep
//                 clearance from the (fixed-coordinate, reused-from-the-
//                 SVG-mockup) tide chart below it.
//version 1.0.29 - Same 30px-too-short-container bug hit the legend rows on
//                 the Battery screen ("Charging (super off-peak)"/
//                 "Discharging", clipped at bottom -- reported directly)
//                 and the Time & rates screen ("Super off-peak"/"Off-peak"/
//                 "On-peak", fixed proactively since it's the identical
//                 makeFlexRow+makeLegendItem pattern). Both widened from
//                 30 to 40px; the Battery one also nudged up slightly to
//                 stay within the 480px screen height.
//version 1.0.30 - Real data pass (display-only, no Unit 1/control changes):
//                 (1) real summer TOU rates/schedule (on-peak 4-9pm every
//                 day, super off-peak 12am-2pm weekends/holidays only,
//                 off-peak the rest; 2026 holiday dates computed exactly)
//                 now drive the Home/Time & rates pills and the Time
//                 screen's schedule bar every second, replacing the old
//                 static "$0.38/kWh"/flat green bar placeholders;
//                 (2) subscribed to Line1Set (same topic Unit 1 itself
//                 uses for Home Assistant's total_power_from_grid) for a
//                 real whole-household grid-consumption reading, now
//                 driving the Grid screen's big number/status (3-state:
//                 Consuming/Bypassing/Exporting, since the real signal
//                 can go negative unlike the old L1+L2-sum proxy);
//                 (3) real NOAA tide predictions for La Jolla (station
//                 9410230, all of 2026, embedded from a verified NOAA
//                 fetch -- see tide_data_2026.h) replace the Almanac
//                 screen's static single-curve mockup data, with a
//                 cosine-eased curve between today's actual high/low
//                 events and real High/Low time labels.
//version 1.0.31 - Fixed the weekday TOU schedule: 1.0.30 assumed weekdays
//                 had no super off-peak window at all (matching only the
//                 weekend/holiday numbers you'd given first), which you
//                 caught -- weekdays actually have TWO super off-peak
//                 windows, 12am-6am and 10am-2pm. Refactored the schedule
//                 into a single buildTouWindows() both the tier lookup
//                 (computeTouStatus) and the visual bar (buildTouSegments)
//                 now derive from, instead of two separately-maintained
//                 copies of the schedule shape that could (and did
//                 nearly) drift apart. Bar segment count bumped from 4 to
//                 6 (weekdays now need more segments than weekends).
//version 1.0.32 - Added two more pills below the Time & rates screen's
//                 main status pill, showing the other two rates (no
//                 "until" time, just tier + price), ordered by which one
//                 comes up next in the schedule -- findUpcomingTiers()
//                 walks buildTouWindows() forward from now to decide the
//                 order. Same flex-row-pair centering pattern as every
//                 other multi-item row on this dashboard.
//version 1.0.33 - 1.0.32's side-by-side pair ran off both edges of the
//                 screen -- "Super off-peak - $0.09/kWh" is long enough
//                 that two of them plus a gap don't fit in 800px.
//                 Restacked all three pills vertically (one per row)
//                 instead, per your correction.
//version 1.0.34 - Grid screen ("the consumption page") rework, per a
//                 detailed discussion: (1) two rings side by side --
//                 Battery (L1+L2 feeder sum, dynamic In/Out/Idle label,
//                 unchanged battery-state colors) and Grid (real
//                 Line1Set+Line2Set sum, static "Grid In/Out" label,
//                 blue/red by sign only); (2) corrected a real
//                 misunderstanding about Line1Set/Line2Set -- they're
//                 independent per-line signed readings, NOT a duplicated
//                 whole-home value as an earlier comment claimed (user
//                 confirmed via HIL testing), so both are now subscribed
//                 and summed for the true total; (3) relabeled L1/L2 to
//                 L1/L2 Feeder and added L1/L2 Grid columns showing the
//                 real per-line data; (4) Saved Today is now computed
//                 (time-averaged: elapsed on/off-peak hours today, purely
//                 from the clock + schedule, x last-known battery
//                 discharge power x their rates) instead of a hardcoded
//                 "$4.20"; (5) fixed the Almanac tide High/Low labels to
//                 sort by actual time of day (left=earlier) instead of a
//                 fixed High=left/Low=right that could disagree with the
//                 curve's own left-to-right time order; (6) corrected a
//                 header comment calling this a "power-arbitrage" system
//                 -- it's TOU optimization, not buying/selling power.
//                 Also flagged (not fixed here): Unit 1's own
//                 oscillation-fix halving logic assumed the now-corrected
//                 Line1Set/Line2Set-are-duplicates premise -- tracked
//                 separately.
//version 1.0.35 - 1.0.34's Battery ring shipped with a literal zero-length
//                 arc (angles 0,0 never updated), invisible next to the
//                 Grid ring's visible placeholder arc -- caught in the
//                 first real hardware capture. Turned it into an actual
//                 gauge instead of just fixing the placeholder angle: the
//                 user gave the real hardware limit (max battery
//                 discharge/output is 1800W), so the ring now shows
//                 |L1+L2 feeder power| / 1800W as a real 0-360 degree
//                 arc, same pattern as the SoC ring elsewhere.
//version 1.0.36 - Grid screen's two ring columns (185px tall) clipped the
//                 descending 'y' in "Battery" against their own bottom
//                 edge -- caught in the first real hardware capture of
//                 1.0.35. Widened to 200px and nudged the rows below down
//                 to keep clear gaps. Also added a test layout on the
//                 Home screen's bottom-right quadrant: the battery power
//                 ring next to the grid power ring, short "Batt"/"Grid"
//                 labels for now, purely to confirm two rings fit in that
//                 compact 396x236 space before refining further.
//version 1.0.37 - 1.0.36's Home quadrant two-ring test also clipped --
//                 zoomed pixel inspection showed "Batt"/"Grid" cut across
//                 their whole bottom edge, not just a descender: 130px
//                 was less than the bare-minimum ring+watts+label space
//                 needed with ZERO margin, not just a tight fit. Widened
//                 to 190px with real (10px+) gaps between every element
//                 this time instead of just enough to theoretically clear.
//version 1.0.38 - Real-hardware bug batch, caught after actual daily use:
//                 (1) the Home/Time date labels ("Monday, July 13") were
//                 build-time placeholder text that was never updated at
//                 runtime -- added getLocalDateStr() (same _rtc_localtime
//                 the clock already uses) and wired it into the existing
//                 1-second tick, same as the clock string; (2) the Grid
//                 ring's arc angle was never set in loop() at all (only
//                 its color was) -- it was permanently stuck at its
//                 build-time placeholder (60-120 degrees) regardless of
//                 real data, which is why it looked like the whole Home
//                 quadrant wasn't "loading"; now scaled as a real gauge
//                 like the Battery ring, against a new MAX_GRID_POWER
//                 (22kW, a ring-visual-only scale, never clamps the
//                 displayed number); (3) g_line1GridPower/g_line2GridPower
//                 were each silently constrained to +-900W (a leftover
//                 copy-paste from Line1/Line2's real 900W output ceiling)
//                 -- since these are independent real Home Assistant
//                 grid readings with no such ceiling, this capped
//                 combined consumption at 1800W exactly, which is what
//                 the user saw and is now removed entirely, per request,
//                 in favor of displaying the real unclamped value.
//                 MAX_BATTERY_POWER's comment clarified: it only scales
//                 the Battery ring's gauge and only applies to discharge
//                 -- charging was, and remains, uncapped in the displayed
//                 number.
//version 1.0.39 - Tapping the Home screen's bottom-right quadrant stopped
//                 navigating to the Grid screen once 1.0.36 added the
//                 Batt/Grid ring columns there. Root cause: makeColumn()
//                 builds on lv_obj_create(), which is CLICKABLE by
//                 default, and never cleared that flag (unlike makeRing,
//                 which explicitly does) -- the two columns cover almost
//                 the whole quadrant, so LVGL delivered the tap to the
//                 column (which has no handler) instead of bubbling it up
//                 to the quadrant's own navGrid callback. Every other
//                 makeColumn() call site was already non-interactive
//                 content, so clearing CLICKABLE there is a pure fix, not
//                 a behavior tradeoff.
//version 1.0.40 - Added a simulated-touch serial command ('P' + 3-digit x
//                 + 3-digit y, e.g. "P402242") per request, so tap
//                 regressions like 1.0.39's can be verified from a PC
//                 script instead of needing hands on the board. Registers
//                 a second LVGL pointer indev fed from serial instead of
//                 the real digitizer -- goes through real hit-testing/
//                 click-bubbling, not a fake shortcut -- auto-releasing
//                 ~80ms after the press so LVGL sees a normal click, not
//                 a long-press/drag. See tools/sim_tap.py. Initial version
//                 fed the indev already-logical (800x480) coordinates and
//                 silently never hit anything: LVGL applies its own
//                 lv_display_rotate_point() to every pointer indev's raw
//                 point (Arduino_H7_Video creates the LVGL display at the
//                 physical panel's native 480x800 portrait + ROTATION_270,
//                 and the real touch driver "just works" because the
//                 touch controller already reports raw panel-space
//                 coordinates for LVGL to rotate). Traced with a temporary
//                 LV_EVENT_ALL debug handler on the target object, which
//                 showed only periodic redraw events and zero PRESSED/
//                 RELEASED -- confirmed the point was landing nowhere.
//                 Fixed by inverting ROTATION_270's transform in
//                 simTouchReadCb() so the serial protocol can stay in
//                 familiar logical coordinates.
//version 1.0.41 - Saved Today was valuing a single instantaneous
//                 discharge-power sample (g_lastDischargePower, last
//                 reading while battery state was "Discharging") x
//                 elapsed on/off-peak hours -- per request, replaced
//                 with real state-of-charge: energy withdrawn today is
//                 now (1 - SoC) x the real 18kWh pack capacity
//                 (BATTERY_CAPACITY_KWH), so a fully charged battery
//                 always shows $0.00 and it rises as SoC drops. Still
//                 uses the same time-averaged on/off-peak elapsed-hours
//                 split to value that energy (uniform-consumption
//                 simplification, unchanged). g_lastDischargePower is
//                 gone -- it had no other use.
//version 1.0.42 - Saved Today's blended rate used hours ELAPSED SO FAR
//                 today, so before the first on-peak window starts (e.g.
//                 all morning) onPeakHours was 0 and the blend collapsed
//                 to a pure $0.43492 off-peak rate -- not wrong exactly
//                 (correctly reflects that no on-peak time has actually
//                 occurred yet), but per request changed to use the
//                 day's FIXED on-peak/off-peak hour totals instead (via
//                 computeElapsedTierHours(..., 24.0, ...), reusing the
//                 same function for the whole-day case), so the blended
//                 rate is stable all day and always strictly between
//                 $0.43492 and $0.65410.
//version 1.0.43 - Added an orbiting charge/discharge dot to the SoC
//                 ring (both the Home quadrant and the Battery detail
//                 screen), planned and mocked up interactively before
//                 writing any of this. The ring itself is unchanged
//                 (still teal, still starts east) except its track is
//                 now red instead of neutral gray, per request. The dot
//                 layers two more independent signals on top: color +
//                 spin direction from g_batteryState (yellow/clockwise
//                 while charging, amethyst/counter-clockwise while
//                 discharging, matching the physical ESP32 FastLED fuel
//                 gauge's own traveling-highlight colors exactly), and
//                 spin rate from real power (up to 1 full rotation/sec
//                 at 100%) -- feeder power (L1+L2) over MAX_BATTERY_POWER
//                 while discharging, grid total power via a two-point
//                 calibrated line (50W->10%, 7000W->100%) while charging
//                 as a temporary stand-in until a real charge-power MQTT
//                 topic exists. Idle never spins. Implemented as a plain
//                 lv_obj dot repositioned every animation tick via
//                 cos/sin from a tracked float angle (not an lv_arc
//                 rotation), so a mid-spin direction/rate change restarts
//                 from the dot's current position instead of snapping
//                 back to the east starting point. Also fixed the same
//                 click-through bug as makeColumn() (v1.0.39) in the
//                 pre-existing makeDot() helper -- lv_obj_create() is
//                 clickable by default and the orbiting dot moves around
//                 clickable quadrants/screens, so this needed to be
//                 correct even though the existing static legend dots
//                 never surfaced it in practice.
//version 1.0.44 - Real-hardware bug batch: (1) the SoC ring's Charging/
//                 Discharging/Idle state text could show a false "Idle"
//                 indefinitely -- g_batteryState defaults to 0 (Idle) at
//                 boot, there's no MQTT staleness timeout by design, and
//                 the Battery/Action topic's publisher likely doesn't
//                 retain it, so a freshly booted/reflashed Unit 2 has no
//                 way to learn the CURRENT state until the next real
//                 transition. Now gated on g_lastBatteryActionMs (only
//                 ever set inside the Action handler) so it shows
//                 nothing until a real reading has actually arrived,
//                 instead of a guess; (2) added seconds back to the
//                 clock (getLocaltime(), shared by Home and Time
//                 screens); (3) Home quadrant's "Batt" label spelled
//                 out to "Battery" -- fits fine in the 185px column.
//version 1.0.45 - Added a static "Feeder" caption below the Home
//                 quadrant's Battery label, centered, per request --
//                 clarifies that ring measures feeder power (same
//                 source as the Grid screen's L1/L2 Feeder columns).
//                 Plain text, no data dependency, so it shows
//                 regardless of whether Unit 1 is currently publishing.
//version 1.0.46 - First pieces of the planned WiFi setup flow (network
//                 select + on-screen keyboard UI comes in a later
//                 version): (1) mbed KVStore (QSPI-backed, physically
//                 separate from program flash so it survives a sketch
//                 reflash, not just a reboot) for persisting a
//                 manually-entered WiFi network across firmware
//                 updates -- saveWifiCredentials()/loadWifiCredentials(),
//                 verified round-trip AND reflash-survival on real
//                 hardware via a temporary 'K' serial test command;
//                 (2) connectToWiFi() no longer retries forever with a
//                 single credential source -- bounded 3x15s attempts
//                 against secrets.h, then 3x15s against stored KVStore
//                 credentials if that fails, returning false if both do
//                 (setup() currently just retries this pair indefinitely
//                 since the manual setup screens that should take over
//                 at that point don't exist yet -- next version); (3)
//                 ssid[]/password[] grew from secrets.h-exact-length to
//                 fixed 33/64-byte buffers (real WiFi SSID/WPA2 maximums)
//                 so a runtime-entered network can actually fit.
//version 1.0.47 - Manual WiFi setup flow completed and verified end-to-end
//                 on real hardware: network-scan list screen and
//                 lv_keyboard-based password entry screen, both wired into
//                 the fallback chain from 1.0.46. Also extracted the
//                 serial dev-command handling (screen dump/nav/sim-touch)
//                 into a standalone serviceDevCommands(), called from both
//                 loop() and setup()'s manual-setup wait loop -- previously
//                 those dev tools only worked from loop(), so they were
//                 unusable during exactly the blocking flow they're most
//                 needed for. Removed the temporary 'K' KVStore test
//                 command now that the real flow exercises save/load for
//                 real. Three real bugs found and fixed via screen-dump
//                 captures during testing: (1) scan-list rows were sized
//                 equal to their list container's *outer* width, overflowing
//                 past the container's own side padding -- combined with
//                 the "Secured"/"Open" label, this clipped text against the
//                 physical 800px screen edge (rendered as "Secure" with the
//                 trailing "d" cut off); rows now use lv_pct(100), and the
//                 label was shortened to "Lock"/"Open" and right-aligned
//                 via makeRowLabel() to fit the narrow gap next to the
//                 signal-strength bars; (2) hidden/non-broadcasting
//                 networks surfaced as scan results with an empty SSID,
//                 rendering as dead, unlabeled, still-tappable rows -- now
//                 skipped; (3) lv_keyboard_create()'s own constructor
//                 bottom-docks the keyboard via a *sticky* LV_ALIGN_BOTTOM_MID
//                 (LVGL re-applies it on later internal layout passes),
//                 which fired against a stale intermediate height and left
//                 the keyboard positioned at y=440 instead of y=220 --
//                 only its top ~40px row (of four) was visible above the
//                 480px screen bottom, making three of the four keyboard
//                 rows completely unusable. Fixed by re-asserting the
//                 bottom alignment as the last setup step instead of
//                 fighting it with a fixed lv_obj_set_pos. Confirmed on
//                 real hardware: tapped a real scanned network ("Oscar"),
//                 typed its real password via the on-screen keyboard,
//                 connected, and -- on a completely fresh reflash with
//                 secrets.h still pointed at a bogus test network -- the
//                 board fell through to the just-saved KVStore credentials
//                 and connected using those, proving the manual-entry ->
//                 KVStore-save -> KVStore-load round trip is real, not
//                 just the earlier synthetic test.
//version 1.0.48 - Replaced the Home quadrant's and Connection screen's
//                 hardcoded-green "Connected" dot/label (set once at
//                 screen-build time, never touched again) with a real
//                 three-state indicator (Connecting/Connected/Not
//                 connected) via setConnStatusIndicator(), called at
//                 every real WiFi state transition: before the first
//                 connect attempt, after it resolves (either source, or
//                 falling through to manual setup), after a successful
//                 manual/Reset-network round trip, and once per second
//                 from loop()'s existing update tick (this sketch has no
//                 active reconnect loop, so a runtime drop correctly
//                 shows "Not connected" instead of a stale "Connected").
//                 Also added the Connection screen's "Reset network"
//                 button -- clears the stored KVStore credentials and
//                 re-enters the same manual setup flow immediately,
//                 without touching secrets.h, for switching networks
//                 without a reflash. Along the way, found and fixed a
//                 second real bug: a single lv_timer_handler() call
//                 before a blocking WiFi call doesn't guarantee the
//                 frame has actually reached the panel by the time
//                 execution continues -- added forcePaint() (several
//                 ticks with a short real delay between them) and used
//                 it everywhere a state needs to be visibly painted
//                 before blocking. Verified the indicator itself is
//                 correct via a temporary debug print reading the live
//                 LVGL label text directly (confirmed "Connecting" during
//                 a forced-failure test) after a screen-dump capture
//                 taken at that same instant misleadingly still showed
//                 the previous "Connected" state -- traced to the
//                 screen-dump tool itself reading stale DRAM content
//                 that survives a soft reset during the display's own
//                 cold-boot window, not a firmware bug (three separate
//                 captures across different boot attempts, including one
//                 after the forcePaint fix, came back byte-for-byte
//                 identical, which a live redraw could never produce).

uint8_t verbosity = 255;
bool trace = true;

int timezone = -7; //GMT -7, Pacific Time -- same convention as Unit 1

//Fixed, generous buffers (33 = 32-char WiFi SSID max + null; 64 = 63-char
//WPA2 passphrase max + null) instead of char[]=SECRET_SSID's exact-fit
//sizing -- those two only needed to hold the compile-time secrets.h
//string, but a runtime-entered network (scanned SSID + typed password)
//can be longer than whatever's in secrets.h.
char ssid[33];
char password[64];

int wifiStatus = WL_IDLE_STATUS;

//---- WiFi credential persistence (mbed KVStore, QSPI-backed) ----
//Physically separate flash from the program flash the .ino gets written
//to, so this survives a sketch reflash, not just a reboot -- per
//request, so a manually-configured network doesn't need re-entering
//after every firmware update. Default mbed KVStore mount point is
//"/kv/"; keys can't contain '/' etc. per kv_set()'s own doc comment, so
//"wifi_ssid"/"wifi_pass" (no further slashes) are used directly under it.
#define KV_KEY_WIFI_SSID "/kv/wifi_ssid"
#define KV_KEY_WIFI_PASS "/kv/wifi_pass"

bool saveWifiCredentials(const char* s, const char* p) {
  int rc1 = kv_set(KV_KEY_WIFI_SSID, s, strlen(s) + 1, 0);
  int rc2 = kv_set(KV_KEY_WIFI_PASS, p, strlen(p) + 1, 0);
  return rc1 == 0 && rc2 == 0;
}

//Returns true only if BOTH keys were found and fit within the given
//buffers -- a partial save (e.g. power loss mid-write) should not be
//treated as a usable stored credential.
bool loadWifiCredentials(char* sOut, size_t sCap, char* pOut, size_t pCap) {
  size_t actualS = 0, actualP = 0;
  int rc1 = kv_get(KV_KEY_WIFI_SSID, sOut, sCap, &actualS);
  int rc2 = kv_get(KV_KEY_WIFI_PASS, pOut, pCap, &actualP);
  return rc1 == 0 && rc2 == 0 && actualS > 0 && actualP > 0;
}
WiFiUDP Udp;
unsigned int localPort = 2391; //different from Unit 1's 2390 in case both run on the same LAN segment during bring-up
constexpr auto timeServer{ "pool.ntp.org" };
const int NTP_PACKET_SIZE = 48;
byte packetBuffer[NTP_PACKET_SIZE];

Arduino_H7_Video Display(800, 480, GigaDisplayShield);
Arduino_GigaDisplayTouch TouchDetector;

WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);
const char broker[] = HOME_ASSISTANT_IP;
int port = 1883;

//Subscribe-only, per UNIT_NUMBER 2 -- this device never publishes anything,
//on any topic. These are the same Battery/SoC and Battery/Action topics the
//sibling ESP32_Fuel_Gauge project already publishes to; no new data source,
//just a second subscriber.
const char subtopicBatterySoC[] = "V1.0/Home/Battery/SoC";
const char subtopicBatteryAction[] = "V1.0/Home/Battery/Action";
//Same topics Giga_YH_Controller_LVGL_1.0.9 (Unit 1) itself publishes to
//(pubtopic1/pubtopic2 there) -- its own L1/L2 output readback, unsigned
//watts, only ever sent when that unit's UNIT_NUMBER < 2.
const char subtopicLine1[] = "V1.0/Home/PowerFeeder/Line1";
const char subtopicLine2[] = "V1.0/Home/PowerFeeder/Line2";
//Same topics Unit 1 itself subscribes to (its subtopic1/subtopic2) for
//Home Assistant's real grid readings. CORRECTED 2026-07-13 via the
//user's own HIL testing: these are NOT a duplicated whole-household
//value (an earlier comment here, and in Unit 1's own source, claimed
//that) -- they're INDEPENDENT signed per-line readings. One line can be
//positive (importing) while the other is negative (exporting), summing
//toward zero at balance. True whole-household net grid consumption is
//their SUM, not either one alone. See memory
//project_line1set_line2set_independent.md for the full context and the
//still-open question of whether this affects Unit 1's own oscillation-fix
//calibration (tracked separately, not touched here).
const char subtopicLine1Grid[] = "V1.0/Home/PowerFeeder/Line1Set";
const char subtopicLine2Grid[] = "V1.0/Home/PowerFeeder/Line2Set";

float g_batterySoC = -1.0; //-1 = no reading received yet
int g_batteryState = 0;    //1 = Charging, -1 = Discharging, 0 = Idle/unknown
float g_line1Power = -1.0; //-1 = no reading received yet; Unit 1's own feeder OUTPUT readback (Line1/Line2, not Set)
float g_line2Power = -1.0;
//Real signed per-line grid readings (Line1Set/Line2Set). Separate has-flags
//because a signed value can legitimately be negative, so unlike the floats
//above, "-1 = no data" doesn't work as a sentinel here.
float g_line1GridPower = 0;
bool g_hasLine1GridPower = false;
float g_line2GridPower = 0;
bool g_hasLine2GridPower = false;

//last-received timestamps (millis()) per topic -- not surfaced in the UI
//yet, just bookkeeping so a future "stale vs. never received" distinction
//is possible. Relevant because Unit 1 doesn't run 24/7 today (it will,
//eventually, once it has its own around-the-clock TOU schedule logic), so
//"no data" during its off hours is expected, not a fault to report on.
unsigned long g_lastBatterySoCMs = 0;
unsigned long g_lastBatteryActionMs = 0;
unsigned long g_lastLine1Ms = 0;
unsigned long g_lastLine2Ms = 0;
unsigned long g_lastLine1GridMs = 0;
unsigned long g_lastLine2GridMs = 0;

//---- color palette (matches design/mockups/*.svg exactly) ----
#define COLOR_BG          lv_color_hex(0x0b0c0e)
#define COLOR_TEXT        lv_color_hex(0xf2f2f0)
#define COLOR_TEXT_MUTED  lv_color_hex(0x8a8d92)
#define COLOR_TEXT_DIM    lv_color_hex(0x6b6e73)
#define COLOR_TEAL        lv_color_hex(0x3fd6c8)
#define COLOR_GREEN       lv_color_hex(0x4fbf7a)
#define COLOR_AMBER       lv_color_hex(0xe8a53d)
#define COLOR_RED         lv_color_hex(0xe2574c)
#define COLOR_STATUS_OK   lv_color_hex(0x39d98a)
#define COLOR_TRACK       lv_color_hex(0x242629)
#define COLOR_PILL_AMBER  lv_color_hex(0x3a1f1c)
#define COLOR_BLUE_TIDE   lv_color_hex(0x5dc8e8)
#define COLOR_MOON_GRAY   lv_color_hex(0xc9cace)

//Orbiting charge/discharge dot colors -- matched exactly to the physical
//ESP32 FastLED fuel gauge strip's own traveling-highlight colors
//(CRGB::Yellow / CRGB::Amethyst in ESP32_Fuel_Gauge.ino), so the two
//devices read consistently if you're looking at both.
#define COLOR_ORBIT_CHARGING     lv_color_hex(0xFFFF00)
#define COLOR_ORBIT_DISCHARGING  lv_color_hex(0x9966CC)

//battery-state color mapping (Idle=green, Discharging=blue, Charging=red --
//deliberately different from the mockups' original green/amber scheme, per
//explicit direction). Shared by the Battery panel and the Grid panel so the
//two can't drift out of sync with each other.
#define COLOR_STATE_IDLE         COLOR_GREEN
#define COLOR_STATE_DISCHARGING  COLOR_BLUE_TIDE
#define COLOR_STATE_CHARGING     COLOR_RED

lv_color_t batteryStateColor(int state) {
  return state == 1 ? COLOR_STATE_CHARGING : state == -1 ? COLOR_STATE_DISCHARGING : COLOR_STATE_IDLE;
}

const char* batteryStateText(int state) {
  return state == 1 ? "Charging" : state == -1 ? "Discharging" : "Idle";
}

//---- Time-of-use rates and schedule (summer; display-only) ----
//Real rates and hours as given 2026-07-13. Unit 2 has no control
//authority over anything -- this only drives what's *displayed* here,
//never what Unit 1 actually does with the battery.
#define RATE_SUPER_OFF_PEAK 0.09469
#define RATE_OFF_PEAK       0.43492
#define RATE_ON_PEAK        0.65410

//Real hardware limit, per the user: max battery DISCHARGE/output is
//1800W -- charging has no such limit (battery can charge as fast as it
//wants). This constant is ONLY a visual scale for the Battery ring's
//gauge (0-1800W -> 0-360 degrees); it never caps the displayed watts
//number itself, which always shows the real, unclamped value.
#define MAX_BATTERY_POWER 1800.0

//Real hardware limit, per the user: 22kW is the practical ceiling for
//the Grid ring's gauge scale, not an actual cap -- the displayed grid
//watts number is never clamped, only the ring's arc angle is scaled
//against this so the gauge saturates at a sensible visual max instead
//of the arc wrapping/looking broken for a real number this large.
#define MAX_GRID_POWER 22000.0

//Real hardware limit, per the user: the battery pack's usable capacity
//is 18kWh. Used for the Saved Today calc below -- NOT a ring-visual
//scale like the two constants above, this one directly represents real
//energy (state-of-charge x capacity), not a gauge saturation point.
#define BATTERY_CAPACITY_KWH 18.0

//2026 TOU holiday dates (month, day), computed exactly with Python (not
//by hand -- see conversation). None fall on a Sunday this year, so the
//"shift to the following Monday" rule needs no code this pass.
static const MonthDay TOU_HOLIDAYS_2026[] = {
  { 1, 1 },    //New Year's Day
  { 1, 19 },   //MLK Day
  { 2, 16 },   //Presidents' Day
  { 5, 25 },   //Memorial Day
  { 7, 4 },    //Independence Day
  { 9, 7 },    //Labor Day
  { 11, 26 },  //Thanksgiving
  { 12, 25 },  //Christmas Day
};

bool isTouHoliday(int month, int day) {
  for (size_t i = 0; i < sizeof(TOU_HOLIDAYS_2026) / sizeof(TOU_HOLIDAYS_2026[0]); i++) {
    if (TOU_HOLIDAYS_2026[i].month == month && TOU_HOLIDAYS_2026[i].day == day) return true;
  }
  return false;
}

//Single source of truth for the day's schedule shape, shared by both the
//tier lookup (computeTouStatus) and the visual schedule bar
//(buildTouSegments) so they can't drift out of sync with each other --
//a real risk now that the shape is more complex than "one block per
//tier". On-peak is 4pm-9pm every day. Weekday super off-peak is TWO
//separate windows (12am-6am AND 10am-2pm) -- corrected 2026-07-13 after
//an earlier version assumed weekdays had no super off-peak window at
//all, which was wrong. Weekend/holiday super off-peak is a single
//12am-2pm window. Off-peak fills whatever's left.
int buildTouWindows(bool weekendOrHoliday, TouWindow* out) {
  int n = 0;
  if (weekendOrHoliday) {
    out[n++] = { 0, 14, TOU_SUPER_OFF_PEAK };
    out[n++] = { 14, 16, TOU_OFF_PEAK };
    out[n++] = { 16, 21, TOU_ON_PEAK };
    out[n++] = { 21, 24, TOU_OFF_PEAK };
  } else {
    out[n++] = { 0, 6, TOU_SUPER_OFF_PEAK };
    out[n++] = { 6, 10, TOU_OFF_PEAK };
    out[n++] = { 10, 14, TOU_SUPER_OFF_PEAK };
    out[n++] = { 14, 16, TOU_OFF_PEAK };
    out[n++] = { 16, 21, TOU_ON_PEAK };
    out[n++] = { 21, 24, TOU_OFF_PEAK };
  }
  return n;
}

TouStatus computeTouStatus(int hour, int wday, int month, int day) {
  TouStatus s;
  s.weekendOrHoliday = (wday == 0 || wday == 6) || isTouHoliday(month, day);
  TouWindow windows[6];
  int n = buildTouWindows(s.weekendOrHoliday, windows);
  for (int i = 0; i < n; i++) {
    if (hour >= windows[i].startHour && hour < windows[i].endHour) {
      s.tier = windows[i].tier;
      s.nextBoundaryHour = windows[i].endHour;
      break;
    }
  }
  s.rate = touRate(s.tier);
  return s;
}

double touRate(TouTier tier) {
  return tier == TOU_ON_PEAK ? RATE_ON_PEAK : tier == TOU_SUPER_OFF_PEAK ? RATE_SUPER_OFF_PEAK : RATE_OFF_PEAK;
}

//Of the two tiers OTHER than the current one, which appears next going
//forward in today's schedule, and which appears after that. Scans
//buildTouWindows() forward from the current hour's window, treating the
//day as a repeating cycle of its own windows (a reasonable simplification
//since a tier change more than one day out isn't meaningfully "upcoming"
//anyway). With exactly 3 possible tiers this always finds both within a
//handful of steps.
void findUpcomingTiers(TouTier current, bool weekendOrHoliday, int currentHour, TouTier* outNext, TouTier* outThird) {
  TouWindow windows[6];
  int n = buildTouWindows(weekendOrHoliday, windows);
  int curIdx = 0;
  for (int i = 0; i < n; i++) {
    if (currentHour >= windows[i].startHour && currentHour < windows[i].endHour) {
      curIdx = i;
      break;
    }
  }
  TouTier next = current, third = current;
  bool foundNext = false;
  for (int step = 1; step <= n; step++) {
    TouTier t = windows[(curIdx + step) % n].tier;
    if (t == current) continue;
    if (!foundNext) {
      next = t;
      foundNext = true;
    } else if (t != next) {
      third = t;
      break;
    }
  }
  *outNext = next;
  *outThird = third;
}

//Sums how many hours of on-peak and off-peak fall within [0, uptoHour)
//of today's schedule (fractional, e.g. 2.5 hours), clipping the window
//containing uptoHour to just its portion before that point. Super
//off-peak time is intentionally not returned/accumulated anywhere --
//that's charging time, not savings. Pass the current fractional hour to
//get hours elapsed SO FAR today, or 24.0 to get the day's fixed total
//on/off-peak hours regardless of the time of day (used by the Saved
//Today rate blend below, which wants a stable blended rate all day, not
//one that's 100% off-peak before the first on-peak window has started).
//Purely a function of the clock + the schedule (both already known), so
//it needs no persisted state and is automatically correct after a
//reboot, at the cost of the "assume uniform consumption" simplification
//the user asked for -- it doesn't track how discharge power actually
//varied through the day, just tier hours within the window asked for.
void computeElapsedTierHours(bool weekendOrHoliday, float uptoHour, float* outOnPeakHours, float* outOffPeakHours) {
  TouWindow windows[6];
  int n = buildTouWindows(weekendOrHoliday, windows);
  float onPeak = 0, offPeak = 0;
  for (int i = 0; i < n; i++) {
    if (uptoHour <= windows[i].startHour) continue;  //hasn't started yet today
    float elapsedEnd = (uptoHour < windows[i].endHour) ? uptoHour : windows[i].endHour;
    float elapsed = elapsedEnd - windows[i].startHour;
    if (elapsed <= 0) continue;
    if (windows[i].tier == TOU_ON_PEAK) onPeak += elapsed;
    else if (windows[i].tier == TOU_OFF_PEAK) offPeak += elapsed;
  }
  *outOnPeakHours = onPeak;
  *outOffPeakHours = offPeak;
}

const char* touTierName(TouTier tier) {
  return tier == TOU_ON_PEAK ? "On-peak" : tier == TOU_SUPER_OFF_PEAK ? "Super off-peak" : "Off-peak";
}

lv_color_t touTierColor(TouTier tier) {
  return tier == TOU_ON_PEAK ? COLOR_RED : tier == TOU_SUPER_OFF_PEAK ? COLOR_GREEN : COLOR_AMBER;
}

//Formats an hour-of-day (0-24, 24 meaning midnight) as "H:00 AM/PM".
void formatHourLabel(int hour24, char* buf, size_t bufSize) {
  int h = hour24 % 24;
  int displayHour = h % 12;
  if (displayHour == 0) displayHour = 12;
  const char* ampm = (h < 12) ? "AM" : "PM";
  snprintf(buf, bufSize, "%d:00 %s", displayHour, ampm);
}

//Fills up to 6 (start,end,color) segments describing today's schedule
//across 24 hours, for the Time & rates screen's schedule bar -- just
//buildTouWindows() above with each window's tier mapped to its color, so
//this can't disagree with what computeTouStatus() actually decides.
//Returns the segment count (6 on weekdays, 4 on weekends/holidays).
int buildTouSegments(bool weekendOrHoliday, TouSegment* out) {
  TouWindow windows[6];
  int n = buildTouWindows(weekendOrHoliday, windows);
  for (int i = 0; i < n; i++) {
    out[i].startHour = windows[i].startHour;
    out[i].endHour = windows[i].endHour;
    out[i].color = touTierColor(windows[i].tier);
  }
  return n;
}

//Grid ring color: blue while pulling power from the grid (>=0), red while
//giving power away (<0) -- "we don't want to give power away" per
//request. The label itself ("Grid In/Out") stays static regardless of
//sign; only the ring/number color changes. Replaces the old 3-state
//Consuming/Bypassing/Exporting text, dropped per request for now.
lv_color_t gridFlowColor(float totalGridPower) {
  return totalGridPower >= 0 ? COLOR_BLUE_TIDE : COLOR_RED;
}

//Battery ring label: unlike the Grid ring, this DOES change text with
//state -- "In" while charging, "Out" while discharging, "Idle" otherwise.
//Color reuses the existing, unchanged battery-state palette (Idle=green,
//Discharging=blue, Charging=red).
const char* batteryFlowLabel(int batteryState) {
  return batteryState == 1 ? "Battery In" : batteryState == -1 ? "Battery Out" : "Battery Idle";
}

//---- screens (built once in setup(), switched with lv_scr_load) ----
lv_obj_t* scr_home;
lv_obj_t* scr_time;
lv_obj_t* scr_connection;
lv_obj_t* scr_battery;
lv_obj_t* scr_grid;
lv_obj_t* scr_almanac;
lv_obj_t* scr_wifi_scan;
lv_obj_t* scr_wifi_password;

//---- WiFi setup flow state ----
#define MAX_WIFI_SCAN_RESULTS 20
char g_wifiScanSsid[MAX_WIFI_SCAN_RESULTS][33];
bool g_wifiScanSecure[MAX_WIFI_SCAN_RESULTS];
int32_t g_wifiScanRssi[MAX_WIFI_SCAN_RESULTS];
int g_wifiScanCount = 0;

char g_selectedWifiSsid[33] = "";
bool g_selectedWifiSecure = false;

//set true by setup() when it's blocked waiting on the manual setup flow
//(both bounded connect attempts failed) -- the wait loop that checks
//this lives in setup() itself, right where the fallback triggers, not
//in the normal loop() tick.
bool g_awaitingManualWifiSetup = false;

lv_obj_t* wifiScanList;
lv_obj_t* wifiScanStatusLbl;
lv_obj_t* wifiPasswordSsidLbl;
lv_obj_t* wifiPasswordTextarea;
lv_obj_t* wifiKeyboard;
lv_obj_t* wifiConnectStatusLbl;

//labels/widgets that need periodic/live updates after screen build
lv_obj_t* lbl_home_clock;
lv_obj_t* lbl_home_date;
lv_obj_t* lbl_home_ssid;
//connection status dot+label pair, once each on the Home quadrant and the
//Connection screen -- both used to be built once with a hardcoded green
//dot and "Connected" text (never touched again after screen build), so
//the UI claimed a live connection even while connectToWiFi() was still
//mid-retry or had already given up. See setConnStatusIndicator().
lv_obj_t* dot_home_conn;
lv_obj_t* lbl_home_connState;
lv_obj_t* dot_scr_conn;
lv_obj_t* lbl_scr_connState;
lv_obj_t* lbl_conn_ssid;
lv_obj_t* lbl_conn_rssi;
lv_obj_t* lbl_conn_broker;
lv_obj_t* lbl_conn_gateway;
lv_obj_t* lbl_conn_ip;
lv_obj_t* lbl_time_clock;
lv_obj_t* lbl_time_date;
lv_obj_t* ring_home_battery;
lv_obj_t* dot_home_battery;
lv_obj_t* lbl_home_battery_pct;
lv_obj_t* lbl_home_battery_state;
lv_obj_t* ring_batt_screen;
lv_obj_t* dot_batt_screen;
lv_obj_t* lbl_batt_pct;
lv_obj_t* lbl_batt_state;
lv_obj_t* ring_home_grid;
lv_obj_t* lbl_home_grid_watts;
lv_obj_t* lbl_home_grid_status;  //text set once ("Grid In/Out"), only color updates now
//Home quadrant test: battery power ring next to the grid ring, short
//"Batt"/"Grid" labels for now just to confirm they fit in the compact
//396x236 quadrant space, per request.
lv_obj_t* ring_home_batt_power;
lv_obj_t* lbl_home_batt_watts;
lv_obj_t* lbl_home_batt_label;
//Grid screen: two rings side by side -- battery (left, L1+L2 feeder sum)
//and grid (right, Line1Grid+Line2Grid sum), per request.
lv_obj_t* ring_grid_battery;
lv_obj_t* lbl_grid_battery_watts;
lv_obj_t* lbl_grid_battery_label;  //dynamic: "Battery In"/"Battery Out"/"Battery Idle"
lv_obj_t* ring_grid_grid;
lv_obj_t* lbl_grid_grid_watts;
lv_obj_t* lbl_grid_grid_label;  //text set once ("Grid In/Out"), only color updates now
lv_obj_t* lbl_grid_l1_feeder;
lv_obj_t* lbl_grid_l2_feeder;
lv_obj_t* lbl_grid_l1_grid;
lv_obj_t* lbl_grid_l2_grid;
lv_obj_t* lbl_grid_saved;
lv_obj_t* pill_home_tou;
lv_obj_t* pill_time_tou;
lv_obj_t* pill_time_next;   //next-upcoming of the two other rates
lv_obj_t* pill_time_third;  //the one after that
lv_obj_t* schedule_bar_seg[6];  //6, not 4 -- weekdays now have more segments than weekends (two super off-peak windows)
lv_obj_t* tide_line_obj;
lv_obj_t* lbl_tide_left;   //positional (not content-fixed) -- see rebuildTideCurve()
lv_obj_t* lbl_tide_right;

//---- NTP / clock (same approach as Unit 1's getLocaltime/setNtpTime) ----
char* getLocaltime(char buffer[]) {
  tm t;
  _rtc_localtime(time(NULL), &t, RTC_FULL_LEAP_YEAR_SUPPORT);
  strftime(buffer, 32, "%I:%M:%S %p", &t);
  return buffer;
}

//Same _rtc_localtime() call as above, but exposes the full tm struct
//(hour/weekday/month/day/day-of-year) instead of just a formatted time
//string -- used for TOU tier computation and tide-table lookup.
void getLocalTm(tm& t) {
  _rtc_localtime(time(NULL), &t, RTC_FULL_LEAP_YEAR_SUPPORT);
}

//"Tuesday, July 14" -- real weekday/date, replacing the old build-time
//placeholder ("Monday, July 13") that was never actually updated at
//runtime. %A/%B are in newlib's built-in (non-locale-file) name tables,
//same library this sketch already relies on for getLocaltime()'s %I/%p.
char* getLocalDateStr(char buffer[]) {
  tm t;
  _rtc_localtime(time(NULL), &t, RTC_FULL_LEAP_YEAR_SUPPORT);
  strftime(buffer, 32, "%A, %B %d", &t);
  return buffer;
}

unsigned long sendNTPpacket(const char* address) {
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011;
  packetBuffer[1] = 0;
  packetBuffer[2] = 6;
  packetBuffer[3] = 0xEC;
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  Udp.beginPacket(address, 123);
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

unsigned long parseNtpPacket() {
  if (!Udp.parsePacket()) return 0;
  Udp.read(packetBuffer, NTP_PACKET_SIZE);
  const unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
  const unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
  const unsigned long secsSince1900 = highWord << 16 | lowWord;
  constexpr unsigned long seventyYears = 2208988800UL;
  const unsigned long epoch = secsSince1900 - seventyYears;
  const unsigned long new_epoch = epoch + (3600 * timezone);
  set_time(new_epoch);
  return epoch;
}

void setNtpTime() {
  Udp.begin(localPort);
  sendNTPpacket(timeServer);
  delay(1000);
  parseNtpPacket();
}

//---- WiFi ----
//Bounded attempt helper -- tries WiFi.begin() up to maxAttempts times,
//attemptDelayMs apart, returns as soon as one succeeds. Replaces the
//old unconditional infinite retry loop, which had no way to ever fall
//through to a fallback credential source.
bool tryConnectWiFi(const char* s, const char* p, int maxAttempts, unsigned long attemptDelayMs) {
  for (int attempt = 1; attempt <= maxAttempts; attempt++) {
    if (verbosity > 0) {
      Serial.print("Attempting to connect to SSID: ");
      Serial.print(s);
      Serial.print(" (attempt ");
      Serial.print(attempt);
      Serial.print("/");
      Serial.print(maxAttempts);
      Serial.println(")");
    }
    wifiStatus = WiFi.begin(s, p);
    if (wifiStatus == WL_CONNECTED) return true;
    delay(attemptDelayMs);
    if (wifiStatus == WL_CONNECTED) return true;  //WiFi.begin() itself may block long enough to settle during the call above; re-check before waiting out a full extra delay
  }
  return false;
}

//Boot-time connect: secrets.h baseline first (3x15s), then the
//KVStore-stored credentials from a previous manual setup if that fails
//(3x15s) -- per request, so a manually-configured network survives a
//cold boot or reflash without needing re-entry, while secrets.h stays
//the primary default. Returns false if both fail; the manual
//network-selection/keyboard setup flow (not yet wired in here) is what
//should run next when that happens.
bool connectToWiFi(void) {
  if (WiFi.status() == WL_NO_MODULE) {
    if (verbosity > 0) Serial.println("Communication with WiFi module failed!");
    while (true);
  }
  if (WiFi.status() == WL_NO_SHIELD) {
    if (verbosity > 0) Serial.println("Communication with WiFi module failed!");
    while (true);
  }

  strncpy(ssid, SECRET_SSID, sizeof(ssid) - 1);
  ssid[sizeof(ssid) - 1] = '\0';
  strncpy(password, SECRET_PASS, sizeof(password) - 1);
  password[sizeof(password) - 1] = '\0';
  if (tryConnectWiFi(ssid, password, 3, 15000)) {
    setNtpTime();
    if (verbosity > 0) Serial.println("Connected to WiFi (secrets.h)");
    return true;
  }

  char storedSsid[33], storedPass[64];
  if (loadWifiCredentials(storedSsid, sizeof(storedSsid), storedPass, sizeof(storedPass))) {
    if (verbosity > 0) Serial.println("secrets.h network not reachable -- trying stored credentials");
    if (tryConnectWiFi(storedSsid, storedPass, 3, 15000)) {
      strncpy(ssid, storedSsid, sizeof(ssid) - 1);
      ssid[sizeof(ssid) - 1] = '\0';
      strncpy(password, storedPass, sizeof(password) - 1);
      password[sizeof(password) - 1] = '\0';
      setNtpTime();
      if (verbosity > 0) Serial.println("Connected to WiFi (stored credentials)");
      return true;
    }
  } else if (verbosity > 0) {
    Serial.println("secrets.h network not reachable, no stored credentials to fall back to");
  }

  return false;
}

//---- touch input driver ----
//Arduino_GigaDisplayTouch::getTouchPoints() is the documented API for this
//library at time of writing -- verify against the installed library version
//if touch doesn't register on real hardware, this is the first thing to check.
//LVGL 9 API (the old lv_indev_drv_t struct-based registration from v8 was
//removed -- confirmed by an actual compile against the installed lvgl 9.5.0).
void touchpad_read(lv_indev_t* indev, lv_indev_data_t* data) {
  GDTpoint_t points[5];
  uint8_t contacts = TouchDetector.getTouchPoints(points);
  if (contacts > 0) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = points[0].x;
    data->point.y = points[0].y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

//---- simulated touch input (dev/debug only) ----
//A second, independent LVGL pointer indev, fed from a serial command
//instead of the real digitizer -- lets a PC-side script exercise real
//taps (real LVGL hit-testing/click-bubbling, the same code path a
//physical finger drives) for regression testing things like the
//makeColumn() click-through bug, without needing hands on the board.
//Registering a second POINTER indev alongside the real one is supported
//by LVGL -- both feed the same active screen's input processing.
volatile bool g_simTouchDown = false;
volatile int16_t g_simTouchX = 0;
volatile int16_t g_simTouchY = 0;
unsigned long g_simTouchReleaseAt = 0;

//g_simTouchX/Y are in the sketch's familiar logical 800x480 landscape
//space (the same space every lv_obj_set_pos() call and the screen
//exporter's PNGs use) -- but LVGL expects every pointer indev's raw
//point in the DISPLAY's un-rotated base resolution and applies its own
//lv_display_rotate_point() to every indev read (see lv_indev.c's
//indev_pointer_proc()). Arduino_H7_Video creates the LVGL display at
//the physical panel's native portrait 480x800 (Display(800,480) with
//width>=height triggers _rotated=true -> lv_display_create(height(),
//width()) = lv_display_create(480,800)) plus ROTATION_270. The real
//touch driver (touchpad_read() above) works "for free" because the
//touch controller already reports raw panel-space coordinates, which
//LVGL's own rotation then converts. Feeding it already-logical
//coordinates (as an earlier version of this function did) makes LVGL
//rotate them a SECOND time, landing on the wrong point entirely --
//this is why simulated taps parsed correctly but never hit any
//on-screen object. Inverting ROTATION_270's transform by hand here
//(raw_x = 479 - logical_y, raw_y = logical_x) makes the serial
//protocol/PC script deal only in logical coordinates, same as
//everything else in this sketch.
void simTouchReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
  data->point.x = 479 - g_simTouchY;
  data->point.y = g_simTouchX;
  data->state = g_simTouchDown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

//---- screen exporter (dev/debug only) ----
//Dumps the live DSI framebuffer over Serial as raw RGB565 so a PC-side
//script can reconstruct a real PNG of whatever's on screen right now --
//lets layout be checked against the real rendered resolution/font metrics
//without needing a hardware photo for every tweak. Triggered by sending the
//single byte 'D' over the same Serial port already used for logging.
//
//The framebuffer here is in the *physical panel's native orientation*
//(the Giga Display Shield's panel is physically portrait; this library
//applies a 270 degree LVGL-side rotation so our 800x480 landscape logical
//screen renders correctly on it -- see Arduino_H7_Video.cpp's _rotated
//handling). dsi_getDisplayXSize()/YSize() report that native (rotated)
//size, not our logical 800x480 -- the PC-side script un-rotates it back.
void dumpFramebufferToSerial() {
  uint32_t w = dsi_getDisplayXSize();
  uint32_t h = dsi_getDisplayYSize();
  const uint16_t* fb = (const uint16_t*)dsi_getActiveFrameBuffer();

  //Flush any pending writes and force a fresh CPU read of the framebuffer
  //memory (DMA2D writes it, so a stale D-cache line could otherwise be
  //served back instead of what's actually on screen right now).
  SCB_CleanInvalidateDCache();

  Serial.println();
  Serial.println("FBDUMP");
  Serial.print(w);
  Serial.print('x');
  Serial.println(h);
  Serial.println("RGB565");

  uint32_t pixelCount = w * h;
  Serial.write((const uint8_t*)fb, pixelCount * 2);

  Serial.println();
  Serial.println("FBEND");
}

//---- MQTT message handler (subscribe-only -- this never publishes) ----
void onMqttMessage(int messageSize) {
  char tbuf[256] = "";
  int size = 0;
  String topic = mqttClient.messageTopic();

  if (topic.equals(subtopicBatterySoC)) {
    while (mqttClient.available() && size < (int)sizeof(tbuf) - 1) {
      tbuf[size] = (char)mqttClient.read();
      size++;
    }
    while (mqttClient.available()) mqttClient.read(); //discard anything beyond tbuf's capacity so the next message stays in sync
    tbuf[size] = '\0';
    //clamped the same way the fuel-gauge project's own SoC handler is, since
    //an out-of-range value here would otherwise feed straight into an arc
    //angle calculation with no bound of its own
    g_batterySoC = constrain(atof(tbuf), 0.0, 100.0);
    g_lastBatterySoCMs = millis();
    if (trace) { Serial.print("Battery SoC = "); Serial.println(g_batterySoC, 1); }

  } else if (topic.equals(subtopicBatteryAction)) {
    while (mqttClient.available() && size < (int)sizeof(tbuf) - 1) {
      tbuf[size] = (char)mqttClient.read();
      size++;
    }
    while (mqttClient.available()) mqttClient.read();
    tbuf[size] = '\0';
    if (!strcmp(tbuf, "Charging")) g_batteryState = 1;
    else if (!strcmp(tbuf, "Discharging")) g_batteryState = -1;
    else if (!strcmp(tbuf, "Idle")) g_batteryState = 0;
    g_lastBatteryActionMs = millis();
    if (trace) { Serial.print("Battery Action = "); Serial.println(tbuf); }

  } else if (topic.equals(subtopicLine1)) {
    while (mqttClient.available() && size < (int)sizeof(tbuf) - 1) {
      tbuf[size] = (char)mqttClient.read();
      size++;
    }
    while (mqttClient.available()) mqttClient.read();
    tbuf[size] = '\0';
    g_line1Power = constrain(atof(tbuf), 0.0, 900.0); //900 = Unit 1's MAX_POWER ceiling
    g_lastLine1Ms = millis();
    if (trace) { Serial.print("Line1 = "); Serial.println(g_line1Power, 0); }

  } else if (topic.equals(subtopicLine2)) {
    while (mqttClient.available() && size < (int)sizeof(tbuf) - 1) {
      tbuf[size] = (char)mqttClient.read();
      size++;
    }
    while (mqttClient.available()) mqttClient.read();
    tbuf[size] = '\0';
    g_line2Power = constrain(atof(tbuf), 0.0, 900.0);
    g_lastLine2Ms = millis();
    if (trace) { Serial.print("Line2 = "); Serial.println(g_line2Power, 0); }

  } else if (topic.equals(subtopicLine1Grid)) {
    while (mqttClient.available() && size < (int)sizeof(tbuf) - 1) {
      tbuf[size] = (char)mqttClient.read();
      size++;
    }
    while (mqttClient.available()) mqttClient.read();
    tbuf[size] = '\0';
    //signed -- unlike Line1/Line2 (feeder output) above, negative here is
    //a real reading (this line net-exporting), not out-of-range noise.
    //Deliberately NOT constrained like Line1/Line2 above -- this is a real
    //Home Assistant grid reading, not bounded by Unit 1's own 900W output
    //ceiling, and per request should display whatever it actually is
    //(the old +-900 constrain here silently capped combined L1+L2 grid
    //consumption at 1800W, which is what surfaced this bug).
    g_line1GridPower = atof(tbuf);
    g_hasLine1GridPower = true;
    g_lastLine1GridMs = millis();
    if (trace) { Serial.print("Line1 grid = "); Serial.println(g_line1GridPower, 0); }

  } else if (topic.equals(subtopicLine2Grid)) {
    while (mqttClient.available() && size < (int)sizeof(tbuf) - 1) {
      tbuf[size] = (char)mqttClient.read();
      size++;
    }
    while (mqttClient.available()) mqttClient.read();
    tbuf[size] = '\0';
    g_line2GridPower = atof(tbuf);
    g_hasLine2GridPower = true;
    g_lastLine2GridMs = millis();
    if (trace) { Serial.print("Line2 grid = "); Serial.println(g_line2GridPower, 0); }
  }
}

//Drives the Home quadrant's and Connection screen's dot+label pair. Both
//used to be built once at screen-construction time with a hardcoded
//green dot and "Connected" text, so the UI claimed a live connection even
//during connectToWiFi()'s multi-attempt retries (which run with scr_home
//already loaded and visible, per setup()'s own flow) or after a
//connection was never established at all. Called at every real state
//transition instead -- before the first connect attempt, after each
//attempt resolves, after a successful manual setup or Reset-network
//round trip, and once per second from loop()'s existing periodic update
//tick (this sketch has no active WiFi reconnect loop, so a runtime drop
//correctly stays "Not connected" until the user acts, rather than being
//silently missed).
//A single lv_timer_handler() call queues a redraw but doesn't guarantee
//it's actually reached the panel by the time execution continues into a
//blocking call right after it -- confirmed on real hardware via a
//screen-dump capture taken the instant a serial log line proved the
//Arduino-side code had already executed past the state-change call: the
//physical screen still showed the previous state. Several ticks with a
//short real delay between them gives the display pipeline (this sketch
//renders through a software 270-degree rotation, per dump_screen.py's
//own docstring) actual wall-clock time to finish, not just queue, the
//frame. Used anywhere a state needs to be visibly painted before a
//blocking WiFi call -- "Connecting"/"Scanning..." etc.
void forcePaint() {
  for (int i = 0; i < 5; i++) {
    lv_timer_handler();
    delay(15);
  }
}

void setConnStatusIndicator(WifiUiState state) {
  lv_color_t color = state == WIFI_UI_CONNECTED ? COLOR_STATUS_OK : state == WIFI_UI_CONNECTING ? COLOR_AMBER : COLOR_RED;
  const char* text = state == WIFI_UI_CONNECTED ? "Connected" : state == WIFI_UI_CONNECTING ? "Connecting" : "Not connected";
  lv_obj_set_style_bg_color(dot_home_conn, color, 0);
  lv_label_set_text(lbl_home_connState, text);
  lv_obj_set_style_bg_color(dot_scr_conn, color, 0);
  lv_label_set_text(lbl_scr_connState, text);
}

//---- small widget helpers ----
lv_obj_t* makeScreenRoot() {
  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_set_style_radius(scr, 0, 0);
  //ROOT CAUSE of a real bug found through hours of exporter measurement:
  //plain lv_obj_create() instances get the active theme's default padding
  //(non-zero) unless explicitly zeroed. Every absolute-position and
  //LV_ALIGN_TOP_MID child throughout this whole file assumed this screen's
  //content area starts at literal (0,0) -- it didn't, it started ~20px
  //down/in from that, silently shifting every child on every screen. Worse,
  //because it also shrinks the effective content area, children near the
  //bottom edge (like the Home screen's TOU pill) could get clipped by the
  //screen's own real (padded, smaller-than-assumed) content boundary no
  //matter what their OWN size was set to -- which is why changing that
  //pill's height kept having zero visible effect until this was found.
  lv_obj_set_style_pad_all(scr, 0, 0);
  return scr;
}

lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, lv_color_t color) {
  lv_obj_t* lbl = lv_label_create(parent);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, color, 0);
  return lbl;
}

lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, lv_color_t color, lv_coord_t x, lv_coord_t y) {
  lv_obj_t* lbl = makeLabel(parent, text, color);
  lv_obj_set_pos(lbl, x, y);
  return lbl;
}

//right-aligned label in a fixed-width box ending at labelRight -- a
//screen-exporter capture caught a "label at X, value at X+160" layout
//overlapping because the real label text (e.g. "MQTT broker") was wider
//than the guessed gap. Right-aligning within a known-width box means the
//value column's start position is safe regardless of how wide any given
//label's text actually renders, PROVIDED labelW is wide enough that it
//doesn't wrap -- a first attempt at this used too narrow a labelW and the
//exporter caught the opposite failure, text wrapping onto a second line
//and colliding with the row below. CLIP mode makes that failure mode a
//single-line clip instead of a row-height-breaking wrap if labelW is ever
//still too narrow for some future label text.
lv_obj_t* makeRowLabel(lv_obj_t* parent, const char* text, lv_coord_t labelRight, lv_coord_t y, lv_coord_t labelW) {
  lv_obj_t* lbl = makeLabel(parent, text, COLOR_TEXT_MUTED);
  lv_obj_set_width(lbl, labelW);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_CLIP);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(lbl, labelRight - labelW, y);
  return lbl;
}

lv_obj_t* makeDot(lv_obj_t* parent, lv_color_t color, lv_coord_t x, lv_coord_t y, lv_coord_t d) {
  lv_obj_t* dot = lv_obj_create(parent);
  lv_obj_set_size(dot, d, d);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, color, 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_set_pos(dot, x, y);
  //Same click-through bug as makeColumn() (see its comment): lv_obj_create()
  //is clickable by default. Harmless in practice for the existing static
  //legend/status dots (tiny hit targets), but the new orbiting dot below
  //moves around a clickable quadrant/screen, so this needs to be correct.
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  return dot;
}

//Pill with content-sized WIDTH (auto, always measured correctly in every
//capture so far) but a FIXED, generous height with the label centered
//inside it -- deliberately NOT using LV_SIZE_CONTENT for height.
//
//Root cause found by measuring raw pixels from exporter captures across
//several iterations: LV_SIZE_CONTENT height computed inconsistently
//between pills using the exact same padding -- one (the clickable weather
//pill) came out ~44px tall and rendered cleanly, another (a non-clickable
//TOU pill) came out ~26px tall and clipped its own label's top and bottom
//by several pixels. This was NOT a deferred-layout timing issue (forcing
//an immediate lv_obj_update_layout() made no measurable difference); the
//two pills' clickable state seems to be the actual differentiator via
//whatever the active theme applies to interactive vs. plain lv_obj
//instances, but rather than depend on understanding (or on being able to
//keep depending on) that theme quirk, this sidesteps LV_SIZE_CONTENT
//height entirely. A fixed height plus lv_obj_center() on the label is a
//completely ordinary, well-defined LVGL layout with no ambiguity about
//when its size is "real" -- it always is.
lv_obj_t* makeAutoPill(lv_obj_t* parent, lv_color_t bg, lv_color_t textColor, const char* text, lv_coord_t padX, lv_coord_t height) {
  //Never puts LV_SIZE_CONTENT on the pill itself -- three straight attempts
  //at that (plain height override, update_layout-then-override, different
  //call orderings) all measured byte-identical on real hardware, so
  //whatever's happening is deeper than call ordering. Instead: measure a
  //throwaway label's own natural width directly (a plain, ordinary
  //lv_label operation, not the container content-sizing logic that's been
  //the actual problem), then build the real pill with a fully explicit
  //fixed width/height computed from that measurement. No LV_SIZE_CONTENT
  //anywhere on the container at all.
  lv_obj_t* probe = makeLabel(parent, text, textColor);
  lv_obj_update_layout(probe);
  lv_coord_t labelW = lv_obj_get_width(probe);
  lv_obj_del(probe);

  lv_obj_t* pill = lv_obj_create(parent);
  lv_obj_set_size(pill, labelW + 2 * padX, height);
  lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(pill, bg, 0);
  lv_obj_set_style_border_width(pill, 0, 0);
  lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* lbl = makeLabel(pill, text, textColor);
  lv_obj_center(lbl);
  return pill;
}

lv_obj_t* makeRing(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t d, int16_t start_angle, int16_t end_angle, lv_color_t color) {
  lv_obj_t* arc = lv_arc_create(parent);
  lv_obj_set_size(arc, d, d);
  lv_obj_set_pos(arc, x, y);
  lv_arc_set_bg_angles(arc, 0, 360);
  lv_arc_set_angles(arc, start_angle, end_angle);
  lv_obj_set_style_arc_color(arc, COLOR_TRACK, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
  //arcs are draggable sliders by default with a visible knob; make this a
  //read-only display ring instead
  lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_border_width(arc, 0, LV_PART_KNOB);
  lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  return arc;
}

//---- orbiting charge/discharge indicator dot ----
//A small dot that orbits the SoC ring (ring_home_battery / ring_batt_screen),
//layered on top without touching the ring's own SoC-driven arc length or
//color -- the ring stays exactly as it was (teal, starts at the east/
//3-o'clock position). The dot alone carries two additional, independent
//pieces of information the ring doesn't: which direction power is
//currently flowing (color + spin direction) and how fast (spin rate).
//Angle is tracked as a float here rather than read back from LVGL, so a
//mid-spin direction/rate change can restart smoothly from wherever the
//dot currently is instead of snapping back to the east starting point
//every time real data updates.
//lastDirection: -2 = uninitialized (forces the first update through),
//0 = idle, 1 = charging (CW), -1 = discharging (CCW).
OrbitDot orbitHomeBattery = { nullptr, 0, 0, 0, 0.0f, -2, -1.0f };
OrbitDot orbitBattScreen = { nullptr, 0, 0, 0, 0.0f, -2, -1.0f };

void orbitDotExecCb(void* var, int32_t angleTenths) {
  OrbitDot* o = (OrbitDot*)var;
  o->currentAngleDeg = angleTenths / 10.0f;
  float rad = o->currentAngleDeg * (float)M_PI / 180.0f;
  lv_coord_t diam = lv_obj_get_width(o->dot);
  lv_obj_set_pos(o->dot, o->cx + (int)(o->r * cosf(rad)) - diam / 2,
                  o->cy + (int)(o->r * sinf(rad)) - diam / 2);
}

//direction: 1 = charging (yellow, clockwise), -1 = discharging (amethyst,
//counter-clockwise), 0 = idle (hidden, not spinning). percent: 0-100,
//maps to spin rate, 100% = one full rotation/second. Only touches the
//running animation when direction or percent actually changed, so the
//1-second tick doesn't restart (and visually stutter) a spin that's
//already correct.
void updateOrbitDot(OrbitDot* o, int direction, float percent) {
  bool directionChanged = (direction != o->lastDirection);
  bool percentChanged = fabs(percent - o->lastPercent) > 1.0;
  if (!directionChanged && !percentChanged) return;
  o->lastDirection = direction;
  o->lastPercent = percent;

  lv_anim_delete(o, orbitDotExecCb);

  if (direction == 0 || percent <= 0) {
    lv_obj_add_flag(o->dot, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(o->dot, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_color(o->dot, direction == 1 ? COLOR_ORBIT_CHARGING : COLOR_ORBIT_DISCHARGING, 0);

  float hz = constrain(percent, 0.0, 100.0) / 100.0;
  float fromAngle = o->currentAngleDeg;
  float toAngle = fromAngle + (direction == 1 ? 360.0f : -360.0f);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_exec_cb(&a, orbitDotExecCb);
  lv_anim_set_values(&a, (int32_t)(fromAngle * 10), (int32_t)(toAngle * 10));
  lv_anim_set_duration(&a, (uint32_t)(1000.0 / hz));
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_linear);
  lv_anim_start(&a);
}

//plain container for group layout -- no background/border of its own, used
//as an alignment anchor so children can be centered with lv_obj_align
//regardless of their actual rendered text width, instead of guessing X.
lv_obj_t* makeColumn(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
  lv_obj_t* col = lv_obj_create(parent);
  lv_obj_set_size(col, w, h);
  lv_obj_set_pos(col, x, y);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 0, 0);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  //lv_obj_create() is clickable by default -- this is a plain alignment
  //container, never meant to be interactive itself. Left clickable, a
  //column placed over a clickable ancestor (e.g. the Home screen's q_grid
  //quadrant, which navigates on tap) silently swallows the tap instead of
  //letting it bubble up: LVGL only sends the click to the actual object
  //hit, not automatically up to parent handlers. This is why tapping the
  //Home quadrant's Batt/Grid ring columns stopped navigating to the Grid
  //screen once those columns were added.
  lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
  return col;
}

//horizontal flex row for legend-style dot+label groups (auto-spaced by
//LVGL's flex layout, so items can't overlap regardless of rendered text
//width -- the root cause of the legend overlap bugs found on real hardware)
lv_obj_t* makeFlexRow(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_coord_t gap) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_size(row, w, h);
  lv_obj_set_pos(row, x, y);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, gap, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  return row;
}

//one dot+label pair sized to its own content, meant as a child of makeFlexRow
void makeLegendItem(lv_obj_t* row, lv_color_t dotColor, const char* text) {
  lv_obj_t* item = lv_obj_create(row);
  lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(item, 0, 0);
  lv_obj_set_style_pad_all(item, 0, 0);
  lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(item, 8, 0);
  lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* dot = lv_obj_create(item);
  lv_obj_set_size(dot, 10, 10);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, dotColor, 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  makeLabel(item, text, COLOR_TEXT_MUTED);
}

//sun/moon icon shapes, reusing the exact geometry from
//design/mockups/06_almanac.svg (circle + 4 tick marks for sun, two
//overlapping circles for a moon crescent silhouette). Positioned by
//top-left corner of a (d x d) bounding box, like the other make* helpers.
void makeSunIcon(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t d) {
  lv_coord_t r = d / 2;
  lv_coord_t cx = x + r, cy = y + r;
  makeDot(parent, COLOR_AMBER, x, y, d);
  lv_coord_t tick = r / 2; //tick length, scaled off the icon radius
  makeDot(parent, COLOR_AMBER, cx - 1, y - tick - 2, 2);          //top
  makeDot(parent, COLOR_AMBER, cx - 1, y + d + 2, 2);             //bottom
  makeDot(parent, COLOR_AMBER, x - tick - 2, cy - 1, 2);          //left
  makeDot(parent, COLOR_AMBER, x + d + 2, cy - 1, 2);             //right
}

void makeMoonIcon(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t d) {
  makeDot(parent, COLOR_MOON_GRAY, x, y, d);
  //offset circle in the background color carves out the crescent shape
  makeDot(parent, COLOR_BG, x + d / 4, y - d / 10, d);
}

//navigate by loading a different pre-built screen; called from touch event callbacks
void navHome(lv_event_t* e) { lv_scr_load(scr_home); }
void navTime(lv_event_t* e) { lv_scr_load(scr_time); }
void navConnection(lv_event_t* e) { lv_scr_load(scr_connection); }
void navBattery(lv_event_t* e) { lv_scr_load(scr_battery); }
void navGrid(lv_event_t* e) { lv_scr_load(scr_grid); }
void navAlmanac(lv_event_t* e) { lv_scr_load(scr_almanac); }

void makeBackButton(lv_obj_t* parent) {
  lv_obj_t* back = lv_label_create(parent);
  lv_label_set_text(back, "< Home");
  lv_obj_set_style_text_color(back, COLOR_TEXT_MUTED, 0);
  lv_obj_set_pos(back, 40, 30);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, navHome, LV_EVENT_CLICKED, NULL);
}

//---- WiFi manual setup: network scan + password/keyboard ----
//Reached only when both bounded connectToWiFi() attempts (secrets.h,
//then stored KVStore credentials) fail, or via the Connection screen's
//"Reset network" button. Blocking connect attempts here (tryConnectWiFi
//inside attemptWifiConnectFromSetup) are deliberate -- this is a rare,
//user-attended setup flow, not a hot path, and every other WiFi attempt
//in this sketch already blocks for the same reason.

void navWifiScanRescan(lv_event_t* e) {
  populateWifiScanList();
  lv_scr_load(scr_wifi_scan);
}

//Connection screen's "Reset network" button. Clears the stored KVStore
//credentials and re-enters this same manual setup flow right away --
//doesn't touch secrets.h, so if that's still a valid network a later
//reboot/reflash will just reconnect to it immediately; this button exists
//for switching to a DIFFERENT network without one. Reuses the identical
//blocking wait-loop pattern setup() falls into when both credential
//sources fail (see there for why blocking here is fine): attemptWifi-
//ConnectFromSetup() flips g_awaitingManualWifiSetup back to false and
//navigates to scr_home itself once a new connection succeeds.
void onResetNetworkClicked(lv_event_t* e) {
  kv_remove(KV_KEY_WIFI_SSID);
  kv_remove(KV_KEY_WIFI_PASS);
  WiFi.disconnect();
  setConnStatusIndicator(WIFI_UI_NOT_CONNECTED);
  g_awaitingManualWifiSetup = true;
  populateWifiScanList();
  lv_scr_load(scr_wifi_scan);
  while (g_awaitingManualWifiSetup) {
    lv_timer_handler();
    serviceDevCommands();
    delay(5);
  }
  setConnStatusIndicator(WIFI_UI_CONNECTED);
}

void attemptWifiConnectFromSetup(const char* s, const char* p) {
  lv_obj_clear_flag(wifiConnectStatusLbl, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(wifiConnectStatusLbl, "Connecting...");
  lv_obj_set_style_text_color(wifiConnectStatusLbl, COLOR_AMBER, 0);
  lv_scr_load(scr_wifi_password);  //open-network path can call this straight from the scan screen -- show the status somewhere it'll be seen
  forcePaint();                     //paint "Connecting..." before the blocking attempt below

  if (tryConnectWiFi(s, p, 3, 15000)) {
    strncpy(ssid, s, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    strncpy(password, p, sizeof(password) - 1);
    password[sizeof(password) - 1] = '\0';
    saveWifiCredentials(s, p);
    setNtpTime();
    g_awaitingManualWifiSetup = false;
    lv_scr_load(scr_home);
  } else {
    lv_label_set_text(wifiConnectStatusLbl, "Couldn't connect. Check the password and try again.");
    lv_obj_set_style_text_color(wifiConnectStatusLbl, COLOR_RED, 0);
  }
}

void onWifiPasswordSubmit(lv_event_t* e) {
  const char* pw = lv_textarea_get_text(wifiPasswordTextarea);
  attemptWifiConnectFromSetup(g_selectedWifiSsid, pw);
}

void onWifiNetworkSelected(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  strncpy(g_selectedWifiSsid, g_wifiScanSsid[idx], sizeof(g_selectedWifiSsid) - 1);
  g_selectedWifiSsid[sizeof(g_selectedWifiSsid) - 1] = '\0';
  g_selectedWifiSecure = g_wifiScanSecure[idx];

  if (g_selectedWifiSecure) {
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "Enter password for %s", g_selectedWifiSsid);
    lv_label_set_text(wifiPasswordSsidLbl, hdr);
    lv_textarea_set_text(wifiPasswordTextarea, "");
    lv_obj_add_flag(wifiConnectStatusLbl, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(scr_wifi_password);
  } else {
    attemptWifiConnectFromSetup(g_selectedWifiSsid, "");
  }
}

//Clears and rebuilds the scrollable row list from a fresh
//WiFi.scanNetworks() -- called on entry to the scan screen and every
//time the user backs into it, since results go stale (networks
//appear/disappear, signal changes) and there's no reason to cache them
//across a rare, user-attended setup flow.
void populateWifiScanList() {
  lv_obj_clean(wifiScanList);
  lv_obj_clear_flag(wifiScanStatusLbl, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(wifiScanStatusLbl, "Scanning...");
  lv_obj_set_style_text_color(wifiScanStatusLbl, COLOR_TEXT_MUTED, 0);
  forcePaint();  //paint "Scanning..." before the blocking scan below

  int n = WiFi.scanNetworks();
  g_wifiScanCount = (n > 0) ? min(n, MAX_WIFI_SCAN_RESULTS) : 0;

  if (g_wifiScanCount == 0) {
    lv_label_set_text(wifiScanStatusLbl, "No networks found. Tap Cancel and back in to rescan.");
    return;
  }
  lv_obj_add_flag(wifiScanStatusLbl, LV_OBJ_FLAG_HIDDEN);

  for (int i = 0; i < g_wifiScanCount; i++) {
    String s = WiFi.SSID(i);
    //hidden/non-broadcasting networks surface as a scan result with an
    //empty SSID -- skip building a row for them, since there's nothing
    //meaningful to tap (this manual-setup flow doesn't support typing an
    //SSID by hand for a hidden network). g_wifiScanSsid[i] stays
    //zeroed/unused for this slot, so it can never be selected.
    if (s.length() == 0) continue;
    s.toCharArray(g_wifiScanSsid[i], sizeof(g_wifiScanSsid[i]));
    g_wifiScanSecure[i] = WiFi.encryptionType(i) != ENC_TYPE_NONE;
    g_wifiScanRssi[i] = WiFi.RSSI(i);

    lv_obj_t* row = lv_obj_create(wifiScanList);
    //width lv_pct(100) of the list's own content area, not a hardcoded
    //720 -- a hardcoded row width equal to the *list's outer* width
    //overflowed past the list's 4px side padding, which combined with the
    //"Secured"/"Open" label below pushed text past the physical 800px
    //screen edge (caught by a real screen-dump capture, text rendered as
    //"Secure" with the trailing "d" clipped off).
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 60);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x17191c), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, onWifiNetworkSelected, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    makeLabel(row, g_wifiScanSsid[i], COLOR_TEXT, 20, 18);

    //signal bars: 4 small rects, increasing height, lit up to the
    //RSSI-derived level -- matches the mockup's design, built from plain
    //lv_obj rects rather than an icon font, same convention as the rest
    //of this sketch.
    int level = g_wifiScanRssi[i] > -50 ? 4 : g_wifiScanRssi[i] > -60 ? 3 : g_wifiScanRssi[i] > -70 ? 2 : 1;
    for (int b = 0; b < 4; b++) {
      lv_coord_t barH = 6 + b * 3;
      lv_obj_t* bar = lv_obj_create(row);
      lv_obj_set_size(bar, 5, barH);
      lv_obj_set_pos(bar, 560 + b * 9, 30 - barH);
      lv_obj_set_style_bg_color(bar, b < level ? COLOR_TEAL : COLOR_TRACK, 0);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(bar, 0, 0);
      lv_obj_set_style_radius(bar, 1, 0);
      lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }

    //"Secured"/"Open" shortened to "Lock"/"Open" -- both words need to fit
    //in the narrow gap between the signal bars (ending at row-relative
    //x=592) and the row's own right edge (~712 at this row width), and
    //"Secured" (7 chars) doesn't fit that gap at this sketch's font size
    //(confirmed: it clipped). makeRowLabel's right-aligned fixed-width box
    //(same pattern used on the Connection screen) keeps the text's right
    //edge pinned regardless of exact glyph width.
    makeRowLabel(row, g_wifiScanSecure[i] ? "Lock" : "Open", 702, 18, 95);
  }
}

void buildWifiScanScreen() {
  scr_wifi_scan = makeScreenRoot();
  lv_obj_t* back = lv_label_create(scr_wifi_scan);
  lv_label_set_text(back, "< Cancel");
  lv_obj_set_style_text_color(back, COLOR_TEXT_MUTED, 0);
  lv_obj_set_pos(back, 40, 30);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, navConnection, LV_EVENT_CLICKED, NULL);

  makeLabel(scr_wifi_scan, "Select WiFi network", COLOR_TEXT_MUTED, 40, 70);

  wifiScanStatusLbl = makeLabel(scr_wifi_scan, "", COLOR_TEXT_MUTED, 40, 110);
  lv_obj_add_flag(wifiScanStatusLbl, LV_OBJ_FLAG_HIDDEN);

  wifiScanList = lv_obj_create(scr_wifi_scan);
  lv_obj_set_pos(wifiScanList, 40, 110);
  lv_obj_set_size(wifiScanList, 720, 340);
  lv_obj_set_style_bg_opa(wifiScanList, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(wifiScanList, 0, 0);
  lv_obj_set_style_pad_all(wifiScanList, 4, 0);
  lv_obj_set_style_pad_row(wifiScanList, 10, 0);
  lv_obj_set_flex_flow(wifiScanList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(wifiScanList, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
}

void buildWifiPasswordScreen() {
  scr_wifi_password = makeScreenRoot();
  lv_obj_t* back = lv_label_create(scr_wifi_password);
  lv_label_set_text(back, "< Back");
  lv_obj_set_style_text_color(back, COLOR_TEXT_MUTED, 0);
  lv_obj_set_pos(back, 40, 18);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, navWifiScanRescan, LV_EVENT_CLICKED, NULL);

  wifiPasswordSsidLbl = makeLabel(scr_wifi_password, "Enter password for", COLOR_TEXT_MUTED, 40, 50);

  wifiPasswordTextarea = lv_textarea_create(scr_wifi_password);
  lv_obj_set_size(wifiPasswordTextarea, 720, 46);
  lv_obj_set_pos(wifiPasswordTextarea, 40, 82);
  lv_textarea_set_password_mode(wifiPasswordTextarea, true);
  lv_textarea_set_one_line(wifiPasswordTextarea, true);
  lv_textarea_set_max_length(wifiPasswordTextarea, 63);
  //fires on the keyboard's OK/checkmark key AND on Enter typed directly
  //(lv_keyboard.c sends LV_EVENT_READY to the bound textarea in both
  //cases) -- one attachment point covers both.
  lv_obj_add_event_cb(wifiPasswordTextarea, onWifiPasswordSubmit, LV_EVENT_READY, NULL);

  wifiConnectStatusLbl = makeLabel(scr_wifi_password, "", COLOR_RED, 40, 137);
  lv_obj_add_flag(wifiConnectStatusLbl, LV_OBJ_FLAG_HIDDEN);

  wifiKeyboard = lv_keyboard_create(scr_wifi_password);
  lv_obj_set_size(wifiKeyboard, 800, 260);
  lv_keyboard_set_textarea(wifiKeyboard, wifiPasswordTextarea);
  //lv_keyboard_create()'s own constructor bottom-docks the keyboard via
  //lv_obj_align(BOTTOM_MID) against its *default* 50%-height size, before
  //this call ever resizes it to 260px. That alignment is sticky (LVGL
  //re-applies it on later internal layout passes, e.g. inside
  //lv_keyboard_set_textarea()'s own row/font recalculation) and was
  //observed re-firing against a stale intermediate height, landing the
  //keyboard at y=440 instead of y=220 -- only its first ~40px row was
  //then visible above the physical 480px screen bottom, with the other
  //three rows rendered off-screen (confirmed via lv_obj_get_y() reading
  //440 despite lv_obj_get_height() correctly reading 260, and via a
  //screen-dump capture showing only one keyboard row). Re-asserting the
  //bottom alignment here, after every size/textarea call that could have
  //perturbed it, forces one final recalculation against the real 260px
  //height instead of fighting the widget's own docking behavior with a
  //fixed lv_obj_set_pos.
  lv_obj_align(wifiKeyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
}

//---- Home screen ----
void buildHomeScreen() {
  scr_home = makeScreenRoot();

  //top-left quadrant: time / weather / TOU -- tap navigates to Time screen
  lv_obj_t* q_time = lv_obj_create(scr_home);
  lv_obj_set_size(q_time, 396, 236);
  lv_obj_set_pos(q_time, 2, 2);
  lv_obj_set_style_bg_opa(q_time, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(q_time, 0, 0);
  //same default-padding issue as makeScreenRoot() -- see that function's
  //comment. This one mattered most: it's where the TOU pill clipping was
  //actually traced to.
  lv_obj_set_style_pad_all(q_time, 0, 0);
  lv_obj_add_flag(q_time, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(q_time, navTime, LV_EVENT_CLICKED, NULL);

  lbl_home_clock = makeLabel(q_time, "6:07 PM", COLOR_TEXT);
  lv_obj_align(lbl_home_clock, LV_ALIGN_TOP_MID, 0, 55);
  lbl_home_date = makeLabel(q_time, "Monday, July 13", COLOR_TEXT_MUTED);
  lv_obj_align(lbl_home_date, LV_ALIGN_TOP_MID, 0, 92);

  //pushed down from 122 -- the date label above has descenders (the 'y' in
  //"Monday"/"July") that this pill's opaque background was clipping into
  lv_obj_t* weather_pill = makeAutoPill(q_time, lv_color_hex(0x17191c), COLOR_TEXT, "Cloudy, 68F >", 16, 40);
  lv_obj_align(weather_pill, LV_ALIGN_TOP_MID, 0, 134);
  lv_obj_add_flag(weather_pill, LV_OBJ_FLAG_CLICKABLE);
  //LVGL doesn't bubble click events to parents unless LV_OBJ_FLAG_EVENT_BUBBLE
  //is set (it isn't, here), so this fires on its own without also triggering
  //q_time's navTime handler -- matches the mockup's "weather links to Almanac,
  //not Time" behavior with no extra plumbing needed.
  lv_obj_add_event_cb(weather_pill, navAlmanac, LV_EVENT_CLICKED, NULL);

  //widened the gap from the weather pill above (was 40px pill-top-to-pill-top,
  //not enough clearance on real hardware -- the two pills' text visibly
  //collided in a hardware photo) to a clearer ~56px
  //shorter than the Time detail screen's version on purpose -- a
  //screen-exporter capture showed the full "On-peak - $0.38/kWh" text
  //overflowing past this quadrant's 396px width even after the auto-pill
  //fix (the pill itself no longer clips its own text, but nothing clips
  //the pill to the quadrant, so an oversized pill spills into the
  //neighboring quadrant/screen edge instead). The rate color already
  //conveys "peak" without spelling it out at this compact scale; the full
  //phrase still appears on the dedicated Time & rates screen, which has
  //room for it.
  //Background switched from the old fixed COLOR_PILL_AMBER to the same
  //neutral dark shade as the weather pill above it -- now that the tier
  //(and its text color) is real and can be green/amber/red, a fixed
  //amber-ish "warning" background behind green text would look wrong.
  //Real text/color set every second in loop(); this placeholder is just
  //sized correctly ($0.XX/kWh is the same length for all three rates).
  pill_home_tou = makeAutoPill(q_time, lv_color_hex(0x17191c), COLOR_RED, "$0.65/kWh", 16, 40);
  lv_obj_align(pill_home_tou, LV_ALIGN_TOP_MID, 0, 190);

  //top-right quadrant: connection -- tap navigates to Connection screen
  lv_obj_t* q_conn = lv_obj_create(scr_home);
  lv_obj_set_size(q_conn, 396, 236);
  lv_obj_set_pos(q_conn, 402, 2);
  lv_obj_set_style_bg_opa(q_conn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(q_conn, 0, 0);
  lv_obj_set_style_pad_all(q_conn, 0, 0);
  lv_obj_add_flag(q_conn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(q_conn, navConnection, LV_EVENT_CLICKED, NULL);

  lv_obj_t* conn_row = makeFlexRow(q_conn, 0, 78, 396, 30, 10);
  lv_obj_set_style_bg_opa(conn_row, LV_OPA_TRANSP, 0);
  dot_home_conn = makeDot(conn_row, COLOR_STATUS_OK, 0, 0, 10);
  lbl_home_connState = makeLabel(conn_row, "Connected", COLOR_TEXT);

  lbl_home_ssid = makeLabel(q_conn, "---", COLOR_TEXT_MUTED);
  lv_obj_align(lbl_home_ssid, LV_ALIGN_TOP_MID, 0, 115);

  //bottom-left quadrant: battery -- tap navigates to Battery screen
  lv_obj_t* q_batt = lv_obj_create(scr_home);
  lv_obj_set_size(q_batt, 396, 236);
  lv_obj_set_pos(q_batt, 2, 242);
  lv_obj_set_style_bg_opa(q_batt, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(q_batt, 0, 0);
  lv_obj_set_style_pad_all(q_batt, 0, 0);
  lv_obj_add_flag(q_batt, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(q_batt, navBattery, LV_EVENT_CLICKED, NULL);

  ring_home_battery = makeRing(q_batt, 90, 55, 84, 0, 0, COLOR_TEAL); //angle set live once a reading arrives
  //Track color per request -- red instead of the default neutral dark
  //gray, purely cosmetic, doesn't affect the teal SoC arc drawn on top.
  lv_obj_set_style_arc_color(ring_home_battery, COLOR_RED, LV_PART_MAIN);
  //Orbiting charge/discharge dot, hidden until real state data arrives.
  //Orbit center/radius derived from the ring's own x/y/diameter above
  //(90,55,84): center = (90+42, 55+42), radius = 42 - half the ring's
  //8px arc width, so the dot rides the arc's own centerline.
  dot_home_battery = makeDot(q_batt, COLOR_ORBIT_CHARGING, 0, 0, 10);
  lv_obj_add_flag(dot_home_battery, LV_OBJ_FLAG_HIDDEN);
  orbitHomeBattery.dot = dot_home_battery;
  orbitHomeBattery.cx = 132;
  orbitHomeBattery.cy = 97;
  orbitHomeBattery.r = 38;
  //pct and state share the same X (left-justified at "the 100% position"),
  //per feedback -- these are NOT centered, unlike the connection/grid text.
  lbl_home_battery_pct = makeLabel(q_batt, "--", COLOR_TEXT, 195, 75);
  //shorter than "Waiting for data" on purpose -- a screen-exporter capture
  //showed that string clipped (q_batt is only 396 wide, and this label is
  //deliberately left-justified at x=195, not centered, leaving just ~200px
  //before the quadrant's edge)
  lbl_home_battery_state = makeLabel(q_batt, "No data yet", COLOR_TEXT_MUTED, 195, 105);

  //bottom-right quadrant: grid flow -- tap navigates to Grid flow screen
  lv_obj_t* q_grid = lv_obj_create(scr_home);
  lv_obj_set_size(q_grid, 396, 236);
  lv_obj_set_pos(q_grid, 402, 242);
  lv_obj_set_style_bg_opa(q_grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(q_grid, 0, 0);
  lv_obj_set_style_pad_all(q_grid, 0, 0);
  lv_obj_add_flag(q_grid, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(q_grid, navGrid, LV_EVENT_CLICKED, NULL);

  //Two small rings side by side -- battery power next to grid power --
  //to test whether they fit in this compact quadrant. Short "Batt"/"Grid"
  //labels for now, per request, purely to check the fit before refining.
  //Column height (and the offsets within it) got a real hardware capture
  //showing genuine clipping at 130px -- that was actually LESS than the
  //bare-minimum space ring+watts+label needs with zero margin, not just a
  //tight fit. Widened to 190px with real gaps between each element this
  //time, not just enough to theoretically clear.
  const lv_coord_t hColW = 185, hRingD = 65;
  lv_obj_t* col_home_batt = makeColumn(q_grid, 10, 15, hColW, 190);
  ring_home_batt_power = makeRing(col_home_batt, (hColW - hRingD) / 2, 0, hRingD, 0, 0, COLOR_TEAL);  //angle set live once battery data arrives
  lbl_home_batt_watts = makeLabel(col_home_batt, "-- W", COLOR_TEXT);
  lv_obj_align(lbl_home_batt_watts, LV_ALIGN_TOP_MID, 0, 78);
  lbl_home_batt_label = makeLabel(col_home_batt, "Battery", COLOR_TEXT_MUTED);
  lv_obj_align(lbl_home_batt_label, LV_ALIGN_TOP_MID, 0, 118);
  //Static caption clarifying what this ring measures (feeder power, same
  //source as the Grid screen's L1/L2 Feeder columns) -- plain text, no
  //data dependency, so it always shows regardless of whether Unit 1 is
  //currently publishing. Same +40px rhythm as the ring->watts->label
  //spacing above it.
  lv_obj_t* feederCaption = makeLabel(col_home_batt, "Feeder", COLOR_TEXT_DIM);
  lv_obj_align(feederCaption, LV_ALIGN_TOP_MID, 0, 158);

  lv_obj_t* col_home_grid = makeColumn(q_grid, 200, 15, hColW, 190);
  ring_home_grid = makeRing(col_home_grid, (hColW - hRingD) / 2, 0, hRingD, 60, 120, COLOR_BLUE_TIDE); //placeholder angle; real needle gauge is a later spiral
  lbl_home_grid_watts = makeLabel(col_home_grid, "-- W", COLOR_TEXT);
  lv_obj_align(lbl_home_grid_watts, LV_ALIGN_TOP_MID, 0, 78);
  //text set ONCE here, never updated -- same static label idea as the
  //full Grid screen's right-hand ring, only the color changes now that
  //the 3-state Consuming/Bypassing/Exporting text is retired.
  lbl_home_grid_status = makeLabel(col_home_grid, "Grid", COLOR_TEXT_MUTED);
  lv_obj_align(lbl_home_grid_status, LV_ALIGN_TOP_MID, 0, 118);
}

//---- Time & rates screen ----
void buildTimeScreen() {
  scr_time = makeScreenRoot();
  makeBackButton(scr_time);

  lbl_time_clock = makeLabel(scr_time, "6:07 PM", COLOR_TEXT);
  lv_obj_align(lbl_time_clock, LV_ALIGN_TOP_MID, 0, 95);
  lbl_time_date = makeLabel(scr_time, "Monday, July 13", COLOR_TEXT_MUTED);
  lv_obj_align(lbl_time_date, LV_ALIGN_TOP_MID, 0, 148);

  //Real schedule bar: 6 pre-created segments (weekends/holidays only use
  //4 of them, the rest hidden), each repositioned/recolored every second
  //in loop() from buildTouSegments(). Bar geometry unchanged: x=100,
  //600px wide over 24 hours = 25px/hour. Segments get a small uniform
  //corner radius rather than only-the-outer-edges rounding -- a minor,
  //acceptable cosmetic simplification versus the old single fully-rounded
  //bar.
  for (int i = 0; i < 6; i++) {
    schedule_bar_seg[i] = lv_obj_create(scr_time);
    lv_obj_set_size(schedule_bar_seg[i], 1, 16);
    lv_obj_set_pos(schedule_bar_seg[i], 100, 210);
    lv_obj_set_style_radius(schedule_bar_seg[i], 4, 0);
    lv_obj_set_style_bg_color(schedule_bar_seg[i], COLOR_GREEN, 0);
    lv_obj_set_style_border_width(schedule_bar_seg[i], 0, 0);
  }

  //height widened from 30 to 40 -- same too-short-container bug as the
  //Battery/Almanac legend rows, fixed proactively here since it's the
  //identical makeFlexRow+makeLegendItem pattern rather than waiting for
  //it to be separately reported.
  lv_obj_t* legend = makeFlexRow(scr_time, 0, 248, 800, 40, 40);
  makeLegendItem(legend, COLOR_GREEN, "Super off-peak");
  makeLegendItem(legend, COLOR_AMBER, "Off-peak");
  makeLegendItem(legend, COLOR_RED, "On-peak");

  //Placeholder text is deliberately the WORST-CASE length ("Super
  //off-peak" is the longest tier name, "12:00 AM" the longest boundary
  //time) so the pill's build-time fixed width (see makeAutoPill's
  //fixed-size design, 1.0.26) is correctly sized from the start --
  //real text set every second in loop() could otherwise be longer than
  //a shorter placeholder and overflow the pill.
  pill_time_tou = makeAutoPill(scr_time, lv_color_hex(0x17191c), COLOR_RED, "Super off-peak - $0.09/kWh - until 12:00 AM", 20, 44);
  lv_obj_align(pill_time_tou, LV_ALIGN_TOP_MID, 0, 295);

  //The other two rates, no "until" time -- ordered by whichever comes
  //next in the schedule first, per request. Stacked one per row (not
  //side by side -- a side-by-side pair ran off both edges of the
  //screen, since "Super off-peak - $0.09/kWh" is long enough that two
  //of them plus a gap don't fit in 800px). Each is its own
  //individually-centered pill, same pattern as the main pill above.
  //Placeholder text is again the worst-case length so build-time sizing
  //is correct no matter which tier ends up in which pill at runtime.
  pill_time_next = makeAutoPill(scr_time, lv_color_hex(0x17191c), COLOR_AMBER, "Super off-peak - $0.09/kWh", 20, 40);
  lv_obj_align(pill_time_next, LV_ALIGN_TOP_MID, 0, 350);
  pill_time_third = makeAutoPill(scr_time, lv_color_hex(0x17191c), COLOR_GREEN, "Super off-peak - $0.09/kWh", 20, 40);
  lv_obj_align(pill_time_third, LV_ALIGN_TOP_MID, 0, 401);
}

//---- Connection screen (fully live this spiral) ----
void buildConnectionScreen() {
  scr_connection = makeScreenRoot();
  makeBackButton(scr_connection);

  lv_obj_t* conn_row = makeFlexRow(scr_connection, 0, 82, 800, 30, 10);
  dot_scr_conn = makeDot(conn_row, COLOR_STATUS_OK, 0, 0, 12);
  lbl_scr_connState = makeLabel(conn_row, "Connected", COLOR_TEXT);

  //two side-by-side column regions instead of one cramped vertical list --
  //left column: what network we're on; right column: broker/addressing.
  //Labels are right-aligned in a fixed-width box ending at *LabelRight (see
  //makeRowLabel) rather than placed at a guessed X, after a screen-exporter
  //capture showed "MQTT broker"/"IP address" overlapping their own values
  //at the gap this used to use.
  lv_coord_t leftLabelRight = 240, leftLabelW = 140, leftValueX = 260;
  //rightLabelW widened from an initial 140 (the exporter caught "MQTT
  //broker"/"IP address" wrapping onto a second line at that width) but the
  //first widening pushed rightValueX far enough right that values like
  //"Connected"/a full IP address then ran off the 800px screen edge --
  //caught by re-capturing. The attempted fix for THAT then shrank
  //rightLabelW back down to 180, which was one more re-capture away from
  //being caught too: 180 isn't enough for "MQTT broker" either, and
  //LV_LABEL_LONG_MODE_CLIP on a right-aligned label clips from the left,
  //so it silently ate the leading "M" instead of visibly wrapping.
  //rightLabelW=210 was already confirmed to fit "MQTT broker" on one line
  //(that's the value this started at); this only shifts the column,
  //keeping that same confirmed-good width instead of re-guessing it.
  lv_coord_t rightLabelRight = 580, rightLabelW = 210, rightValueX = 600;

  makeRowLabel(scr_connection, "SSID", leftLabelRight, 170, leftLabelW);
  lbl_conn_ssid = makeLabel(scr_connection, "---", COLOR_TEXT, leftValueX, 170);

  makeRowLabel(scr_connection, "Signal", leftLabelRight, 216, leftLabelW);
  lbl_conn_rssi = makeLabel(scr_connection, "---", COLOR_TEXT, leftValueX, 216);

  makeRowLabel(scr_connection, "MQTT broker", rightLabelRight, 170, rightLabelW);
  lbl_conn_broker = makeLabel(scr_connection, "---", COLOR_TEXT_MUTED, rightValueX, 170);

  makeRowLabel(scr_connection, "Router", rightLabelRight, 216, rightLabelW);
  lbl_conn_gateway = makeLabel(scr_connection, "---", COLOR_TEXT, rightValueX, 216);

  makeRowLabel(scr_connection, "IP address", rightLabelRight, 262, rightLabelW);
  lbl_conn_ip = makeLabel(scr_connection, "---", COLOR_TEXT, rightValueX, 262);

  makeRowLabel(scr_connection, "Firmware", rightLabelRight, 308, rightLabelW);
  makeLabel(scr_connection, "V" VERSION_DASHBOARD, COLOR_TEXT, rightValueX, 308);

  //Clears the stored KVStore credentials and re-enters the manual
  //network-select flow immediately -- for switching to a different
  //network without a full reflash. Doesn't touch secrets.h, so if that's
  //still a valid network it'll just reconnect to it on the next boot.
  lv_obj_t* resetPill = makeAutoPill(scr_connection, lv_color_hex(0x17191c), COLOR_TEXT_MUTED, "Reset network", 20, 40);
  lv_obj_align(resetPill, LV_ALIGN_BOTTOM_RIGHT, -40, -30);
  lv_obj_add_flag(resetPill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(resetPill, onResetNetworkClicked, LV_EVENT_CLICKED, NULL);
}

//---- Battery screen ----
//chart point data reused verbatim (same 800x480 canvas as this display) from
//design/mockups/04_battery.svg's two polylines, just recolored to the
//Charging=red/Discharging=blue scheme instead of the mockup's original
//green/amber.
static const lv_point_precise_t BATT_CHART_CHARGING[] = {
  { 100, 395 }, { 113, 394 }, { 125, 359 }, { 137, 313 }, { 150, 282 },
  { 162, 264 }, { 175, 268 }, { 188, 268 }, { 200, 268 }, { 213, 268 },
  { 225, 268 }, { 237, 268 }, { 250, 268 }, { 263, 269 }, { 275, 269 },
  { 287, 269 }, { 300, 269 }, { 312, 269 }, { 325, 269 }, { 338, 269 },
  { 350, 269 }, { 363, 269 }, { 375, 269 }, { 387, 269 }, { 400, 269 },
  { 413, 269 }, { 425, 269 }, { 437, 269 }, { 450, 270 }
};
static const lv_point_precise_t BATT_CHART_DISCHARGING[] = {
  { 450, 270 }, { 462, 281 }, { 475, 285 }, { 488, 288 }, { 500, 291 },
  { 513, 295 }, { 525, 298 }, { 537, 304 }, { 550, 310 }, { 553, 311 }
};

void buildBatteryScreen() {
  scr_battery = makeScreenRoot();
  makeBackButton(scr_battery);

  ring_batt_screen = makeRing(scr_battery, 260, 60, 120, 0, 0, COLOR_TEAL); //angle set live once a reading arrives
  //Track color per request -- red instead of the default neutral dark
  //gray, purely cosmetic, doesn't affect the teal SoC arc drawn on top.
  lv_obj_set_style_arc_color(ring_batt_screen, COLOR_RED, LV_PART_MAIN);
  //Orbiting charge/discharge dot, hidden until real state data arrives.
  //Orbit center/radius derived from the ring's own x/y/diameter above
  //(260,60,120): center = (260+60, 60+60), radius = 60 - half the
  //ring's 8px arc width, so the dot rides the arc's own centerline.
  dot_batt_screen = makeDot(scr_battery, COLOR_ORBIT_CHARGING, 0, 0, 14);
  lv_obj_add_flag(dot_batt_screen, LV_OBJ_FLAG_HIDDEN);
  orbitBattScreen.dot = dot_batt_screen;
  orbitBattScreen.cx = 320;
  orbitBattScreen.cy = 120;
  orbitBattScreen.r = 56;
  //pct and state share the same X (left-justified), per feedback
  lbl_batt_pct = makeLabel(scr_battery, "--", COLOR_TEXT, 400, 95);
  lbl_batt_state = makeLabel(scr_battery, "Waiting for data", COLOR_TEXT_MUTED, 400, 125);

  //plain ASCII hyphen, not a Unicode en-dash -- a screen-exporter capture
  //showed the en-dash rendering as a missing-glyph box, since the default
  //LVGL font here doesn't include it
  lv_obj_t* caption = makeLabel(scr_battery, "Yesterday (Sun, Jul 12)\n12 AM - 6:07 PM", COLOR_TEXT_DIM);
  lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 178);

  lv_obj_t* chart_charging = lv_line_create(scr_battery);
  lv_line_set_points(chart_charging, BATT_CHART_CHARGING, sizeof(BATT_CHART_CHARGING) / sizeof(BATT_CHART_CHARGING[0]));
  lv_obj_set_style_line_color(chart_charging, COLOR_STATE_CHARGING, 0);
  lv_obj_set_style_line_width(chart_charging, 3, 0);
  lv_obj_set_style_line_rounded(chart_charging, true, 0);

  lv_obj_t* chart_discharging = lv_line_create(scr_battery);
  lv_line_set_points(chart_discharging, BATT_CHART_DISCHARGING, sizeof(BATT_CHART_DISCHARGING) / sizeof(BATT_CHART_DISCHARGING[0]));
  lv_obj_set_style_line_color(chart_discharging, COLOR_STATE_DISCHARGING, 0);
  lv_obj_set_style_line_width(chart_discharging, 3, 0);
  lv_obj_set_style_line_rounded(chart_discharging, true, 0);

  makeDot(scr_battery, COLOR_TEXT, 548, 306, 10); //"now" marker, matches mockup

  makeLabel(scr_battery, "12 AM", COLOR_TEXT_DIM, 100, 405);
  lv_obj_t* now_lbl = makeLabel(scr_battery, "Now", COLOR_TEXT_DIM);
  lv_obj_set_pos(now_lbl, 660, 405);

  //height widened from 30 to 40 and nudged up from y=445 to keep it within
  //the 480px screen -- same class of bug as the Almanac fixes above: 30px
  //was clipping the bottom of "Charging (super off-peak)"/"Discharging".
  lv_obj_t* legend = makeFlexRow(scr_battery, 0, 436, 800, 40, 40);
  makeLegendItem(legend, COLOR_STATE_CHARGING, "Charging (super off-peak)");
  makeLegendItem(legend, COLOR_STATE_DISCHARGING, "Discharging");
}

//---- Grid flow screen ("the consumption page") ----
//Two rings side by side, per request: Battery (left, L1+L2 feeder sum --
//how much power is moving into/out of the battery) and Grid (right,
//Line1Grid+Line2Grid sum -- the real whole-household net grid flow).
//Each ring uses a column container as its centering anchor, same pattern
//as every other multi-item row on this dashboard, so the ring+number+
//label group centers within its half of the screen regardless of actual
//rendered text width.
void buildGridScreen() {
  scr_grid = makeScreenRoot();
  makeBackButton(scr_grid);

  //column height widened from 185 to 200 -- "Battery" has a descending
  //'y', and 185 clipped it against the column's own bottom edge (same
  //too-short-container bug caught several times earlier today).
  const lv_coord_t colW = 380, ringD = 110;
  lv_obj_t* col_battery = makeColumn(scr_grid, 20, 55, colW, 200);
  ring_grid_battery = makeRing(col_battery, (colW - ringD) / 2, 0, ringD, 0, 0, COLOR_TEAL);  //angle set live once battery data arrives
  lbl_grid_battery_watts = makeLabel(col_battery, "-- W", COLOR_TEXT);
  lv_obj_align(lbl_grid_battery_watts, LV_ALIGN_TOP_MID, 0, 118);
  lbl_grid_battery_label = makeLabel(col_battery, "Battery Idle", COLOR_TEXT_MUTED);
  lv_obj_align(lbl_grid_battery_label, LV_ALIGN_TOP_MID, 0, 152);

  lv_obj_t* col_grid = makeColumn(scr_grid, 400, 55, colW, 200);  //matches col_battery's height for symmetry
  ring_grid_grid = makeRing(col_grid, (colW - ringD) / 2, 0, ringD, 60, 120, COLOR_BLUE_TIDE);  //placeholder angle; real needle gauge is a later spiral
  lbl_grid_grid_watts = makeLabel(col_grid, "-- W", COLOR_TEXT);
  lv_obj_align(lbl_grid_grid_watts, LV_ALIGN_TOP_MID, 0, 118);
  //text set ONCE here, never updated -- per request, no per-sign relabeling,
  //only the ring/number/label COLOR changes (see gridFlowColor())
  lbl_grid_grid_label = makeLabel(col_grid, "Grid In/Out", COLOR_TEXT_MUTED);
  lv_obj_align(lbl_grid_grid_label, LV_ALIGN_TOP_MID, 0, 152);

  //Four stat columns: L1/L2 Feeder (Unit 1's own output readback, renamed
  //from the old plain "L1"/"L2" for clarity that it's feeder output, not
  //a grid reading) and L1/L2 Grid (the real independent per-line grid
  //readings feeding the ring above). 180px width matches the
  //already-confirmed-safe width from the Connection/Saved-today columns
  //earlier this session, rather than re-guessing a smaller one for these
  //shorter-but-not-that-short labels.
  //row y nudged from 255 to 265 -- the ring columns above grew taller
  //(185->200, to fix the "Battery" descender clip) and now end exactly
  //at 255 with zero gap otherwise.
  lv_obj_t* col_l1f = makeColumn(scr_grid, 10, 265, 180, 68);
  lbl_grid_l1_feeder = makeLabel(col_l1f, "--", COLOR_TEXT);
  lv_obj_align(lbl_grid_l1_feeder, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_t* l1f_lbl = makeLabel(col_l1f, "L1 Feeder", COLOR_TEXT_DIM);
  lv_obj_align(l1f_lbl, LV_ALIGN_TOP_MID, 0, 34);

  lv_obj_t* col_l2f = makeColumn(scr_grid, 210, 265, 180, 68);
  lbl_grid_l2_feeder = makeLabel(col_l2f, "--", COLOR_TEXT);
  lv_obj_align(lbl_grid_l2_feeder, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_t* l2f_lbl = makeLabel(col_l2f, "L2 Feeder", COLOR_TEXT_DIM);
  lv_obj_align(l2f_lbl, LV_ALIGN_TOP_MID, 0, 34);

  lv_obj_t* col_l1g = makeColumn(scr_grid, 410, 265, 180, 68);
  lbl_grid_l1_grid = makeLabel(col_l1g, "--", COLOR_TEXT);
  lv_obj_align(lbl_grid_l1_grid, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_t* l1g_lbl = makeLabel(col_l1g, "L1 Grid", COLOR_TEXT_DIM);
  lv_obj_align(l1g_lbl, LV_ALIGN_TOP_MID, 0, 34);

  lv_obj_t* col_l2g = makeColumn(scr_grid, 610, 265, 180, 68);
  lbl_grid_l2_grid = makeLabel(col_l2g, "--", COLOR_TEXT);
  lv_obj_align(lbl_grid_l2_grid, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_t* l2g_lbl = makeLabel(col_l2g, "L2 Grid", COLOR_TEXT_DIM);
  lv_obj_align(l2g_lbl, LV_ALIGN_TOP_MID, 0, 34);

  //Saved Today -- now a real computed value (time-averaged discharge
  //power x elapsed on/off-peak hours today x their rates), not the old
  //hardcoded "$4.20". See the Saved Today calc in loop().
  lv_obj_t* col_saved = makeColumn(scr_grid, 250, 350, 300, 68);  //nudged from 340 to keep a clear gap from the stat row above
  lbl_grid_saved = makeLabel(col_saved, "$0.00", COLOR_STATUS_OK);
  lv_obj_align(lbl_grid_saved, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_t* saved_lbl = makeLabel(col_saved, "Saved today", COLOR_TEXT_DIM);
  lv_obj_align(saved_lbl, LV_ALIGN_TOP_MID, 0, 34);
}

//---- Almanac screen ----
//Real tide curve/High-Low labels, computed from TIDE_EVENTS_2026 (see
//tide_data_2026.h) instead of the old static SVG-mockup polyline.
//(TidePoint itself is declared at the top of the file, alongside the
//other custom types -- see the comment there for why.)

//Linear-searches TIDE_EVENTS_2026 (sorted by day, from NOAA) for today's
//high/low events. Returns the count found (typically 3-4 for this mixed
//semidiurnal-tide station).
int getTodaysTideEvents(int dayOfYear, TidePoint* out, int maxOut) {
  int count = 0;
  for (int i = 0; i < TIDE_EVENT_COUNT && count < maxOut; i++) {
    if (TIDE_EVENTS_2026[i].dayOfYear == dayOfYear) {
      out[count].minuteOfDay = TIDE_EVENTS_2026[i].minuteOfDay;
      out[count].heightFeet = TIDE_EVENTS_2026[i].heightTenths / 10.0;
      out[count].isHigh = TIDE_EVENTS_2026[i].isHigh;
      count++;
    } else if (TIDE_EVENTS_2026[i].dayOfYear > dayOfYear) {
      break;  //table is sorted by day; no more matches possible
    }
  }
  return count;
}

//Formats a minute-of-day (0-1439) as "H:MM AM/PM".
void formatMinuteOfDay(int minuteOfDay, char* buf, size_t bufSize) {
  int hour = (minuteOfDay / 60) % 24;
  int minute = minuteOfDay % 60;
  int displayHour = hour % 12;
  if (displayHour == 0) displayHour = 12;
  const char* ampm = (hour < 12) ? "AM" : "PM";
  snprintf(buf, bufSize, "%d:%02d %s", displayHour, minute, ampm);
}

//Chart geometry matches the space the old SVG-derived polyline used:
//x 150-650 spans the 24-hour day, y 370-410 is the plotted height range
//(inverted -- LVGL/SVG y grows downward, so a higher tide is a smaller y).
#define TIDE_CHART_X0 150
#define TIDE_CHART_X1 650
#define TIDE_CHART_Y_HIGH 370
#define TIDE_CHART_Y_LOW 410
#define TIDE_CURVE_MAX_POINTS 48
static lv_point_precise_t g_tideCurvePoints[TIDE_CURVE_MAX_POINTS];
int g_tideCurvePointCount = 0;

//Rebuilds today's tide curve (cosine-eased interpolation between
//consecutive real high/low events -- a standard, reasonable
//approximation of a real tide curve's shape) and the High/Low labels.
//Only needs to run once a day; loop() calls it periodically to catch a
//midnight rollover, not on every tick.
void rebuildTideCurve() {
  tm t;
  getLocalTm(t);
  int dayOfYear = t.tm_yday + 1;

  TidePoint events[6];
  int n = getTodaysTideEvents(dayOfYear, events, 6);
  if (n == 0) {
    g_tideCurvePointCount = 0;
    return;
  }

  float minH = events[0].heightFeet, maxH = events[0].heightFeet;
  int highIdx = events[0].isHigh ? 0 : -1;
  int lowIdx = events[0].isHigh ? -1 : 0;
  for (int i = 1; i < n; i++) {
    if (events[i].heightFeet < minH) minH = events[i].heightFeet;
    if (events[i].heightFeet > maxH) maxH = events[i].heightFeet;
    if (events[i].isHigh) {
      if (highIdx < 0 || events[i].heightFeet > events[highIdx].heightFeet) highIdx = i;
    } else {
      if (lowIdx < 0 || events[i].heightFeet < events[lowIdx].heightFeet) lowIdx = i;
    }
  }
  if (maxH - minH < 0.1) maxH = minH + 0.1;  //avoid a divide-by-zero on a near-flat day

  int idx = 0;
  const int STEPS = 6;
  //flat lead-in from midnight to the first event
  for (int s = 0; s <= STEPS && idx < TIDE_CURVE_MAX_POINTS; s++) {
    float minuteOfDay = events[0].minuteOfDay * (float)s / STEPS;
    float frac = (events[0].heightFeet - minH) / (maxH - minH);
    g_tideCurvePoints[idx].x = TIDE_CHART_X0 + (int)(minuteOfDay / 1440.0 * (TIDE_CHART_X1 - TIDE_CHART_X0));
    g_tideCurvePoints[idx].y = TIDE_CHART_Y_LOW - (int)(frac * (TIDE_CHART_Y_LOW - TIDE_CHART_Y_HIGH));
    idx++;
  }
  //cosine-eased interpolation between each consecutive pair of events
  for (int i = 0; i < n - 1 && idx < TIDE_CURVE_MAX_POINTS; i++) {
    for (int s = 1; s <= STEPS && idx < TIDE_CURVE_MAX_POINTS; s++) {
      float frac_t = (float)s / STEPS;
      float ease = (1.0 - cos(frac_t * PI)) / 2.0;
      float minuteOfDay = events[i].minuteOfDay + (events[i + 1].minuteOfDay - events[i].minuteOfDay) * frac_t;
      float height = events[i].heightFeet + (events[i + 1].heightFeet - events[i].heightFeet) * ease;
      float frac = (height - minH) / (maxH - minH);
      g_tideCurvePoints[idx].x = TIDE_CHART_X0 + (int)(minuteOfDay / 1440.0 * (TIDE_CHART_X1 - TIDE_CHART_X0));
      g_tideCurvePoints[idx].y = TIDE_CHART_Y_LOW - (int)(frac * (TIDE_CHART_Y_LOW - TIDE_CHART_Y_HIGH));
      idx++;
    }
  }
  //flat lead-out from the last event to midnight
  float mLast = events[n - 1].minuteOfDay;
  for (int s = 1; s <= STEPS && idx < TIDE_CURVE_MAX_POINTS; s++) {
    float minuteOfDay = mLast + (1440 - mLast) * (float)s / STEPS;
    float frac = (events[n - 1].heightFeet - minH) / (maxH - minH);
    g_tideCurvePoints[idx].x = TIDE_CHART_X0 + (int)(minuteOfDay / 1440.0 * (TIDE_CHART_X1 - TIDE_CHART_X0));
    g_tideCurvePoints[idx].y = TIDE_CHART_Y_LOW - (int)(frac * (TIDE_CHART_Y_LOW - TIDE_CHART_Y_HIGH));
    idx++;
  }
  g_tideCurvePointCount = idx;
  lv_line_set_points(tide_line_obj, g_tideCurvePoints, g_tideCurvePointCount);

  //Chronological, not fixed High=left/Low=right -- caught by the user:
  //the curve above runs left-to-right by time of day, so whichever
  //event (high or low) happens EARLIER belongs on the left to match,
  //regardless of which one is "High" vs "Low".
  char timeBuf[12];
  char highBuf[24] = "", lowBuf[24] = "";
  if (highIdx >= 0) {
    formatMinuteOfDay(events[highIdx].minuteOfDay, timeBuf, sizeof(timeBuf));
    snprintf(highBuf, sizeof(highBuf), "High %s", timeBuf);
  }
  if (lowIdx >= 0) {
    formatMinuteOfDay(events[lowIdx].minuteOfDay, timeBuf, sizeof(timeBuf));
    snprintf(lowBuf, sizeof(lowBuf), "Low %s", timeBuf);
  }
  if (highIdx >= 0 && lowIdx >= 0) {
    bool highIsEarlier = events[highIdx].minuteOfDay <= events[lowIdx].minuteOfDay;
    lv_label_set_text(lbl_tide_left, highIsEarlier ? highBuf : lowBuf);
    lv_label_set_text(lbl_tide_right, highIsEarlier ? lowBuf : highBuf);
  } else if (highIdx >= 0) {
    lv_label_set_text(lbl_tide_left, highBuf);
  } else if (lowIdx >= 0) {
    lv_label_set_text(lbl_tide_left, lowBuf);
  }
}

//one sunrise/sunset/moonrise/moonset column: icon, then value, then caption,
//all centered on cx via a fixed-width column container so text-anchor=middle
//from the mockup survives regardless of actual rendered text width.
void buildAlmanacTimeColumn(lv_obj_t* parent, lv_coord_t cx, bool isSun, const char* value, const char* caption) {
  const lv_coord_t w = 150;
  //value-to-caption gap widened from an initial 23px to 30px, matching the
  //Grid screen's stat columns -- both used the same too-tight pattern,
  //caught when you flagged the Grid one as visually cramped.
  //
  //Column height widened from 95 to 115 -- 95 wasn't enough room for
  //icon(28) + value(~28) + caption starting at 70, so the caption's own
  //bottom (and, for words with descenders, the descenders first) was
  //being clipped by the column's own height boundary. Caught by you
  //pointing out specific glyph damage ("u" missing its bottom curve,
  //reading almost like "ii") that a simple ink-vs-background pixel-band
  //scan couldn't distinguish from a font just ending there naturally --
  //the clipping color and the surrounding background are identical.
  lv_obj_t* col = makeColumn(parent, cx - w / 2, 178, w, 115);
  if (isSun) makeSunIcon(col, w / 2 - 10, 8, 20);
  else makeMoonIcon(col, w / 2 - 10, 8, 20);
  lv_obj_t* val = makeLabel(col, value, lv_color_hex(0xd6d7d9));
  lv_obj_align(val, LV_ALIGN_TOP_MID, 0, 40);
  lv_obj_t* cap = makeLabel(col, caption, COLOR_TEXT_DIM);
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 74);
}

void buildAlmanacScreen() {
  scr_almanac = makeScreenRoot();
  makeBackButton(scr_almanac);

  makeLabel(scr_almanac, "68F  Cloudy", COLOR_TEXT, 350, 95);

  //pushed down from the original layout (which collided with the moon-phase
  //row below it once rendered on real hardware) and widened into columns
  //that center regardless of text width.
  buildAlmanacTimeColumn(scr_almanac, 160, true, "5:58 AM", "Sunrise");
  buildAlmanacTimeColumn(scr_almanac, 320, true, "8:41 PM", "Sunset");
  buildAlmanacTimeColumn(scr_almanac, 480, false, "9:14 PM", "Moonrise");
  buildAlmanacTimeColumn(scr_almanac, 640, false, "6:37 AM", "Moonset");

  //height widened from 30 to 40 -- 30 was clipping the descenders on the
  //two g's in "Waxing gibbous" against the row's own bottom edge (you
  //caught this by pointing out specific glyph damage in a zoomed crop:
  //a "u" missing its bottom curve elsewhere on this screen, the same
  //class of bug). y nudged up slightly so the extra height doesn't eat
  //too far into the gap before the Tide label/chart below.
  lv_obj_t* phase_row = makeFlexRow(scr_almanac, 0, 298, 800, 40, 10);
  makeMoonIcon(phase_row, 0, 0, 18);
  //plain hyphen instead of a Unicode middle-dot -- same missing-glyph issue
  //as the battery caption's en-dash above
  makeLabel(phase_row, "Waxing gibbous - 72% lit", COLOR_MOON_GRAY);

  lv_obj_t* tide_lbl = makeLabel(scr_almanac, "Tide", COLOR_BLUE_TIDE);
  lv_obj_align(tide_lbl, LV_ALIGN_TOP_MID, 0, 344);

  //Built with zero points initially -- rebuildTideCurve() fills this in
  //once the clock is NTP-synced (real dates need real time), called once
  //from setup() and periodically from loop() to catch a midnight rollover.
  tide_line_obj = lv_line_create(scr_almanac);
  lv_obj_set_style_line_color(tide_line_obj, COLOR_BLUE_TIDE, 0);
  lv_obj_set_style_line_width(tide_line_obj, 3, 0);
  lv_obj_set_style_line_rounded(tide_line_obj, true, 0);

  lbl_tide_left = makeLabel(scr_almanac, "High --:-- --", COLOR_TEXT, 150, 430);
  lbl_tide_right = makeLabel(scr_almanac, "Low --:-- --", COLOR_TEXT);
  lv_obj_align(lbl_tide_right, LV_ALIGN_TOP_RIGHT, -150, 430);
}

void setup() {
  Serial.begin(115200);
  delay(3000);

  Serial.print("Giga YH Dashboard - Unit 2 (remote display) - V");
  Serial.println(VERSION_DASHBOARD);

  Display.begin();
  TouchDetector.begin();

  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchpad_read);

  lv_indev_t* simIndev = lv_indev_create();
  lv_indev_set_type(simIndev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(simIndev, simTouchReadCb);

  buildHomeScreen();
  buildTimeScreen();
  buildConnectionScreen();
  buildBatteryScreen();
  buildGridScreen();
  buildAlmanacScreen();
  buildWifiScanScreen();
  buildWifiPasswordScreen();
  lv_scr_load(scr_home);
  lv_timer_handler(); //don't leave the screen dark while WiFi connects
  //Home is already visible at this point, so the dot/label would still
  //show whatever build-time text/color the screen was constructed with
  //(previously a hardcoded green "Connected", even though nothing has
  //connected yet) for the entire multi-attempt retry window below unless
  //set explicitly here first.
  setConnStatusIndicator(WIFI_UI_CONNECTING);
  forcePaint(); //paint "Connecting" before the blocking attempts below

  //Manual setup flow: shown only if both bounded attempts above (secrets.h,
  //then stored KVStore credentials) fail. attemptWifiConnectFromSetup()
  //(fired from a network-row tap or the keyboard's OK key) sets
  //g_awaitingManualWifiSetup = false and lv_scr_load(scr_home) on success --
  //this loop just keeps the UI alive and responsive to touch until then.
  if (!connectToWiFi()) {
    if (verbosity > 0) Serial.println("Both secrets.h and stored WiFi credentials failed -- showing manual setup");
    setConnStatusIndicator(WIFI_UI_NOT_CONNECTED);
    g_awaitingManualWifiSetup = true;
    populateWifiScanList();
    lv_scr_load(scr_wifi_scan);
    while (g_awaitingManualWifiSetup) {
      lv_timer_handler();
      serviceDevCommands();  //keep 'D' screen-dump / 'P' sim-touch working during setup too
      delay(5);
    }
  }
  //Reachable only once actually connected -- either connectToWiFi() above
  //succeeded directly, or the wait loop only exits via a successful
  //attemptWifiConnectFromSetup().
  setConnStatusIndicator(WIFI_UI_CONNECTED);

  //Real dates need real (NTP-synced) time, so this has to happen after
  //connectToWiFi() above, not alongside the other screen-building calls.
  rebuildTideCurve();

  if (verbosity > 0) {
    Serial.print("Attempting to connect to the MQTT broker: ");
    Serial.println(broker);
  }
  //Explicit client ID so this can never collide with whatever default ID
  //Unit 1 (or anything else on the broker) is using -- cheap precaution,
  //not a confirmed fix for anything observed so far.
  mqttClient.setId("GigaYH_Unit2");
  mqttClient.setUsernamePassword(MQTT_USERNAME, MQTT_PASSWORD);
  if (!mqttClient.connect(broker, port)) {
    if (verbosity > 0) {
      Serial.print("MQTT connection failed! Error code = ");
      Serial.println(mqttClient.connectError());
    }
    //Unlike Unit 1, don't hang forever here -- a remote display losing its
    //broker connection at boot shouldn't brick the screen. Fall through and
    //let the Connection screen show "Not connected"; retry logic is a later
    //spiral (same gap exists on Unit 1 today, tracked in its CLAUDE.md).
  } else {
    if (verbosity > 0) Serial.println("Connected to the MQTT broker.");

    mqttClient.onMessage(onMqttMessage);

    if (verbosity > 0) { Serial.print("Subscribing to topic: "); Serial.println(subtopicBatterySoC); }
    mqttClient.subscribe(subtopicBatterySoC);

    if (verbosity > 0) { Serial.print("Subscribing to topic: "); Serial.println(subtopicBatteryAction); }
    mqttClient.subscribe(subtopicBatteryAction);

    if (verbosity > 0) { Serial.print("Subscribing to topic: "); Serial.println(subtopicLine1); }
    mqttClient.subscribe(subtopicLine1);

    if (verbosity > 0) { Serial.print("Subscribing to topic: "); Serial.println(subtopicLine2); }
    mqttClient.subscribe(subtopicLine2);

    if (verbosity > 0) { Serial.print("Subscribing to topic: "); Serial.println(subtopicLine1Grid); }
    mqttClient.subscribe(subtopicLine1Grid);

    if (verbosity > 0) { Serial.print("Subscribing to topic: "); Serial.println(subtopicLine2Grid); }
    mqttClient.subscribe(subtopicLine2Grid);
    //Subscribe-only -- no mqttClient.beginMessage()/publish anywhere in this
    //sketch, on any topic. Unit 2 has no publish authority, by design.
  }
}

//Serial dev/debug commands: 'D' dumps the current screen (see
//dumpFramebufferToSerial() above); 'H'/'T'/'C'/'B'/'G'/'M' force-navigate
//to Home/Time/Connection/Battery/Grid/alManac without touching the
//screen, so every screen can be captured by the exporter from a PC
//script without needing physical access to the board; 'P' + 6 ASCII
//digits (3-digit x, 3-digit y, zero-padded -- e.g. "P402242" for
//x=402,y=242) simulates a real tap at that point via the sim touch
//indev above, auto-releasing ~80ms later so LVGL sees a normal
//press-then-release click cycle.
//
//Factored out of loop() and also called from setup()'s manual WiFi
//setup wait loop -- the screen-dump/sim-touch dev tools need to keep
//working during that flow too (that's exactly the flow they're most
//useful for testing), not just once the dashboard's normal loop() is
//running.
void serviceDevCommands() {
  if (Serial.available()) {
    switch (Serial.read()) {
      case 'D': dumpFramebufferToSerial(); break;
      case 'H': lv_scr_load(scr_home); break;
      case 'T': lv_scr_load(scr_time); break;
      case 'C': lv_scr_load(scr_connection); break;
      case 'B': lv_scr_load(scr_battery); break;
      case 'G': lv_scr_load(scr_grid); break;
      case 'M': lv_scr_load(scr_almanac); break;
      case 'P': {
        char coordBuf[6];
        if (Serial.readBytes(coordBuf, 6) == 6) {
          char xBuf[4] = { coordBuf[0], coordBuf[1], coordBuf[2], '\0' };
          char yBuf[4] = { coordBuf[3], coordBuf[4], coordBuf[5], '\0' };
          g_simTouchX = atoi(xBuf);
          g_simTouchY = atoi(yBuf);
          g_simTouchDown = true;
          g_simTouchReleaseAt = millis() + 80;
        }
        break;
      }
      default: break;
    }
  }

  //Release the simulated tap ~80ms after it started -- checked every
  //call (not gated behind any tick) so the press-release cycle stays
  //short enough for LVGL to register it as a click, not a long-press
  //or drag.
  if (g_simTouchDown && millis() >= g_simTouchReleaseAt) {
    g_simTouchDown = false;
  }
}

unsigned long lastClockUpdate = 0;
unsigned long lastTideUpdate = 0;

void loop() {
  lv_timer_handler();
  mqttClient.poll();

  //Tide data only changes once a day; re-checking every minute (not
  //every second, like the clock/TOU tick below) is plenty to catch a
  //midnight rollover without doing this relatively heavier computation
  //needlessly often.
  unsigned long nowTide = millis();
  if (nowTide - lastTideUpdate >= 60000) {
    lastTideUpdate = nowTide;
    rebuildTideCurve();
  }

  serviceDevCommands();

  unsigned long now = millis();
  if (now - lastClockUpdate >= 1000) {
    lastClockUpdate = now;
    char buf[16];
    getLocaltime(buf);
    lv_label_set_text(lbl_home_clock, buf);
    lv_label_set_text(lbl_time_clock, buf);

    char dateBuf[32];
    getLocalDateStr(dateBuf);
    lv_label_set_text(lbl_home_date, dateBuf);
    lv_label_set_text(lbl_time_date, dateBuf);

    //Real TOU tier/rate, once per second -- same cadence as the clock.
    //Display-only: this never touches Unit 1 or its RS485/battery
    //control, it only decides what these two pills/the schedule bar show.
    tm localTm;
    getLocalTm(localTm);
    TouStatus tou = computeTouStatus(localTm.tm_hour, localTm.tm_wday, localTm.tm_mon + 1, localTm.tm_mday);
    lv_color_t touColor = touTierColor(tou.tier);

    char homeTouBuf[16];
    sprintf(homeTouBuf, "$%.2f/kWh", tou.rate);
    lv_obj_t* homeTouLbl = lv_obj_get_child(pill_home_tou, 0);
    lv_label_set_text(homeTouLbl, homeTouBuf);
    lv_obj_set_style_text_color(homeTouLbl, touColor, 0);

    char untilBuf[12];
    formatHourLabel(tou.nextBoundaryHour, untilBuf, sizeof(untilBuf));
    char timeTouBuf[48];
    sprintf(timeTouBuf, "%s - $%.2f/kWh - until %s", touTierName(tou.tier), tou.rate, untilBuf);
    lv_obj_t* timeTouLbl = lv_obj_get_child(pill_time_tou, 0);
    lv_label_set_text(timeTouLbl, timeTouBuf);
    lv_obj_set_style_text_color(timeTouLbl, touColor, 0);

    TouTier nextTier, thirdTier;
    findUpcomingTiers(tou.tier, tou.weekendOrHoliday, localTm.tm_hour, &nextTier, &thirdTier);

    char nextBuf[32];
    sprintf(nextBuf, "%s - $%.2f/kWh", touTierName(nextTier), touRate(nextTier));
    lv_obj_t* nextLbl = lv_obj_get_child(pill_time_next, 0);
    lv_label_set_text(nextLbl, nextBuf);
    lv_obj_set_style_text_color(nextLbl, touTierColor(nextTier), 0);

    char thirdBuf[32];
    sprintf(thirdBuf, "%s - $%.2f/kWh", touTierName(thirdTier), touRate(thirdTier));
    lv_obj_t* thirdLbl = lv_obj_get_child(pill_time_third, 0);
    lv_label_set_text(thirdLbl, thirdBuf);
    lv_obj_set_style_text_color(thirdLbl, touTierColor(thirdTier), 0);

    TouSegment segs[6];
    int segCount = buildTouSegments(tou.weekendOrHoliday, segs);
    for (int i = 0; i < 6; i++) {
      if (i < segCount) {
        lv_coord_t x = 100 + segs[i].startHour * 25;
        lv_coord_t w = (segs[i].endHour - segs[i].startHour) * 25;
        lv_obj_set_pos(schedule_bar_seg[i], x, 210);
        lv_obj_set_size(schedule_bar_seg[i], w, 16);
        lv_obj_set_style_bg_color(schedule_bar_seg[i], segs[i].color, 0);
        lv_obj_clear_flag(schedule_bar_seg[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(schedule_bar_seg[i], LV_OBJ_FLAG_HIDDEN);
      }
    }

    lv_label_set_text(lbl_home_ssid, WiFi.SSID());
    lv_label_set_text(lbl_conn_ssid, WiFi.SSID());
    //This sketch has no active WiFi reconnect loop, so a real runtime drop
    //(router reboot, moved out of range) has nothing retrying it -- WiFi.
    //status() will just sit at whatever it dropped to. Reflecting that
    //here means the dot/label correctly show "Not connected" instead of a
    //stale "Connected" left over from setup(), the same class of bug this
    //whole indicator was added to fix.
    setConnStatusIndicator(WiFi.status() == WL_CONNECTED ? WIFI_UI_CONNECTED : WIFI_UI_NOT_CONNECTED);

    char rssiBuf[16];
    sprintf(rssiBuf, "%ld dBm", WiFi.RSSI());
    lv_label_set_text(lbl_conn_rssi, rssiBuf);

    bool connected = mqttClient.connected();
    lv_label_set_text(lbl_conn_broker, connected ? "Connected" : "Not connected");
    lv_obj_set_style_text_color(lbl_conn_broker, connected ? COLOR_STATUS_OK : COLOR_RED, 0);

    lv_label_set_text(lbl_conn_gateway, WiFi.gatewayIP().toString().c_str());
    lv_label_set_text(lbl_conn_ip, WiFi.localIP().toString().c_str());

    if (g_batterySoC >= 0) {
      char pctBuf[8];
      sprintf(pctBuf, "%.0f%%", g_batterySoC);
      lv_label_set_text(lbl_home_battery_pct, pctBuf);
      lv_label_set_text(lbl_batt_pct, pctBuf);

      int16_t angle = (int16_t)(g_batterySoC / 100.0 * 360);
      lv_arc_set_angles(ring_home_battery, 0, angle);
      lv_arc_set_angles(ring_batt_screen, 0, angle);

      //g_batteryState defaults to 0 (Idle) at boot and there's no MQTT
      //staleness/failsafe timeout on this device by design (see
      //CLAUDE.md) -- so if the Battery/Action topic isn't retained by
      //its publisher, a freshly booted/reflashed Unit 2 has no way to
      //learn the CURRENT state until the next real transition, and
      //shows a false "Idle" the whole time. g_lastBatteryActionMs (only
      //ever set inside the Action MQTT handler) doubles as a has-data
      //flag here, same "-1/0 means never received" convention used
      //elsewhere in this file, so it shows an honest "waiting for
      //data" instead of a guess.
      if (g_lastBatteryActionMs > 0) {
        const char* stateText = batteryStateText(g_batteryState);
        lv_color_t stateColor = batteryStateColor(g_batteryState);
        lv_label_set_text(lbl_home_battery_state, stateText);
        lv_obj_set_style_text_color(lbl_home_battery_state, stateColor, 0);
        lv_label_set_text(lbl_batt_state, stateText);
        lv_obj_set_style_text_color(lbl_batt_state, stateColor, 0);
      }
    }

    //L1/L2 Feeder -- Unit 1's own output readback (unchanged source,
    //renamed labels for clarity that this isn't a grid reading).
    if (g_line1Power >= 0) {
      char l1Buf[12];
      sprintf(l1Buf, "%.0f W", g_line1Power);
      lv_label_set_text(lbl_grid_l1_feeder, l1Buf);
    }
    if (g_line2Power >= 0) {
      char l2Buf[12];
      sprintf(l2Buf, "%.0f W", g_line2Power);
      lv_label_set_text(lbl_grid_l2_feeder, l2Buf);
    }

    //L1/L2 Grid -- the real independent per-line grid readings. Each
    //shown as soon as ITS OWN reading has arrived, independent of the
    //other line (unlike the ring below, which needs both).
    if (g_hasLine1GridPower) {
      char buf[12];
      sprintf(buf, "%.0f W", g_line1GridPower);
      lv_label_set_text(lbl_grid_l1_grid, buf);
    }
    if (g_hasLine2GridPower) {
      char buf[12];
      sprintf(buf, "%.0f W", g_line2GridPower);
      lv_label_set_text(lbl_grid_l2_grid, buf);
    }

    //Battery power ring, both the full Grid screen and the Home quadrant's
    //new test ring: L1+L2 feeder sum, dynamic In/Out/Idle label and the
    //existing, unchanged battery-state color scheme (Home quadrant uses
    //the short "Batt" label). Note this is a DIFFERENT ring from the
    //existing SoC-percentage battery ring in the Home screen's bottom-left
    //quadrant (ring_home_battery) -- that one is unrelated/unchanged.
    if (g_line1Power >= 0 && g_line2Power >= 0) {
      float batteryPower = g_line1Power + g_line2Power;

      char buf[16];
      sprintf(buf, "%.0f W", batteryPower);
      lv_label_set_text(lbl_grid_battery_watts, buf);
      lv_label_set_text(lbl_home_batt_watts, buf);

      const char* battLabel = batteryFlowLabel(g_batteryState);
      lv_color_t battColor = batteryStateColor(g_batteryState);
      lv_label_set_text(lbl_grid_battery_label, battLabel);
      lv_obj_set_style_text_color(lbl_grid_battery_label, battColor, 0);
      lv_obj_set_style_arc_color(ring_grid_battery, battColor, LV_PART_INDICATOR);
      lv_obj_set_style_text_color(lbl_home_batt_label, battColor, 0);
      lv_obj_set_style_arc_color(ring_home_batt_power, battColor, LV_PART_INDICATOR);
      //Real gauge, not a placeholder arc: magnitude of L1+L2 (batteryPower
      //can be negative in principle, hence fabs) as a fraction of the real
      //1800W max discharge/output limit.
      int16_t battAngle = (int16_t)constrain(fabs(batteryPower) / MAX_BATTERY_POWER * 360.0, 0, 360);
      lv_arc_set_angles(ring_grid_battery, 0, battAngle);
      lv_arc_set_angles(ring_home_batt_power, 0, battAngle);
    }

    //Grid ring (both Home quadrant and full screen): sum of the two
    //independent real per-line readings -- the genuine whole-household
    //net grid flow, per the L1Set/L2Set correction earlier this session.
    //Label text is static ("Grid In/Out", set once at build); only the
    //color reflects the sign now (blue = pulling from grid, red = giving
    //power away), per request -- the old 3-state Consuming/Bypassing/
    //Exporting text is retired.
    if (g_hasLine1GridPower && g_hasLine2GridPower) {
      float totalGrid = g_line1GridPower + g_line2GridPower;
      char wattsBuf[16];
      sprintf(wattsBuf, "%.0f W", totalGrid);
      lv_label_set_text(lbl_home_grid_watts, wattsBuf);
      lv_label_set_text(lbl_grid_grid_watts, wattsBuf);

      lv_color_t flowColor = gridFlowColor(totalGrid);
      lv_obj_set_style_text_color(lbl_home_grid_status, flowColor, 0);
      lv_obj_set_style_text_color(lbl_grid_grid_label, flowColor, 0);
      lv_obj_set_style_text_color(lbl_grid_grid_watts, flowColor, 0);
      lv_obj_set_style_arc_color(ring_home_grid, flowColor, LV_PART_INDICATOR);
      lv_obj_set_style_arc_color(ring_grid_grid, flowColor, LV_PART_INDICATOR);

      //Angle was never actually being set here -- both rings were built
      //with a fixed placeholder arc (60-120 degrees) and only ever had
      //their COLOR updated afterward, so they looked frozen regardless of
      //real consumption. Same real-gauge treatment as the Battery ring
      //above, scaled against MAX_GRID_POWER instead: the ring saturates
      //at 22kW, but totalGrid/wattsBuf above are never clamped.
      int16_t gridAngle = (int16_t)constrain(fabs(totalGrid) / MAX_GRID_POWER * 360.0, 0, 360);
      lv_arc_set_angles(ring_home_grid, 0, gridAngle);
      lv_arc_set_angles(ring_grid_grid, 0, gridAngle);
    }

    //Orbiting charge/discharge dot on the SoC ring (see updateOrbitDot()
    //above): direction/color from g_batteryState, spin rate from real
    //power -- feeder power (L1+L2) while discharging (1800W = 100%, same
    //MAX_BATTERY_POWER ceiling as the Battery ring above), grid power
    //while charging (a temporary stand-in until a real charge-power MQTT
    //topic exists, per request: a two-point calibrated line, 50W->10%,
    //7000W->100%, so even a trickle still shows a faint, visible spin
    //rather than looking dead). Idle never spins, regardless of data; if
    //the state says charging/discharging but that state's own power data
    //hasn't arrived yet, the dot stays hidden rather than guessing.
    {
      int orbitDirection = 0;
      float orbitPercent = 0;
      if (g_batteryState == -1 && g_line1Power >= 0 && g_line2Power >= 0) {
        orbitDirection = -1;
        float feederPower = fabs(g_line1Power + g_line2Power);
        orbitPercent = constrain(feederPower / MAX_BATTERY_POWER * 100.0, 0.0, 100.0);
      } else if (g_batteryState == 1 && g_hasLine1GridPower && g_hasLine2GridPower) {
        orbitDirection = 1;
        float gridPower = fabs(g_line1GridPower + g_line2GridPower);
        orbitPercent = constrain(10.0 + (gridPower - 50.0) * (90.0 / 6950.0), 0.0, 100.0);
      }
      updateOrbitDot(&orbitHomeBattery, orbitDirection, orbitPercent);
      updateOrbitDot(&orbitBattScreen, orbitDirection, orbitPercent);
    }

    //Saved Today -- grounded in real state of charge instead of a single
    //instantaneous discharge-power sample: energy actually withdrawn from
    //the battery is (1 - SoC) x the real 18kWh pack capacity, so a fully
    //charged battery always reads $0.00 saved, rising as it draws down.
    //That energy is valued at a time-blended rate: today's fixed on-peak
    //and off-peak hour totals (per request -- NOT hours elapsed so far,
    //which would read as a pure 0.43492 off-peak rate all morning before
    //the first on-peak window even starts) weighted by their real rates,
    //so the blended rate always sits between $0.43492 and $0.65410,
    //never pinned to either endpoint. Super off-peak hours still
    //contribute nothing to the blend (that's charging, an expense, not a
    //saving), and with no SoC reading yet (g_batterySoC == -1) this
    //reads $0.
    float dailyOnPeakHours, dailyOffPeakHours;
    computeElapsedTierHours(tou.weekendOrHoliday, 24.0, &dailyOnPeakHours, &dailyOffPeakHours);
    float savedToday = 0;
    if (g_batterySoC >= 0) {
      float energyUsedKwh = (1.0 - g_batterySoC / 100.0) * BATTERY_CAPACITY_KWH;
      float dailyHours = dailyOnPeakHours + dailyOffPeakHours;
      if (dailyHours > 0) {
        float blendedRate = (dailyOnPeakHours * RATE_ON_PEAK + dailyOffPeakHours * RATE_OFF_PEAK) / dailyHours;
        savedToday = energyUsedKwh * blendedRate;
      }
    }
    char savedBuf[16];
    sprintf(savedBuf, "$%.2f", savedToday);
    lv_label_set_text(lbl_grid_saved, savedBuf);
  }
}

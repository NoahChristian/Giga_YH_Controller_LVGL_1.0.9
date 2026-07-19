# pyscript: publishes weather (Met.no), sun rise/set, and moon rise/set/
# phase to MQTT for the Giga YH Unit 2 dashboard's Almanac screen,
# replacing its static placeholder text -- see buildAlmanacScreen() in
# Giga_YH_Dashboard_Unit2.ino.
#
# Requires skyfield (see requirements.txt in this same folder) for moon
# rise/set and phase-angle calculation -- HA's own Sun integration only
# gives sunrise/sunset, and its Moon integration only gives a discrete
# phase state, not illumination % or rise/set times.
#
# Entities: edit WEATHER_ENTITY below if it doesn't match your own
# instance (find it in Developer Tools > States).
#
# Payload (single line, colon-delimited, matching this project's existing
# compact-encoding convention -- see battery_day_curve.py):
#   "{temp_f}:{condition}:{sunrise_epoch}:{sunset_epoch}:{moonrise_epoch}:{moonset_epoch}:{phase_angle_deg}:{phase_name}"
# All four rise/set values are for TODAY'S local calendar day (midnight to
# midnight), as Unix epoch seconds (UTC) -- the Arduino side already has
# real NTP time and converts to local for display, same as everywhere else
# on this dashboard. This is deliberately NOT "next occurrence from now":
# an earlier version was, and it produced a confusing display once you're
# past today's own moonrise -- the "next" moonrise then belongs to
# tomorrow, so it'd pair with today's still-upcoming moonset, showing two
# events from different calendar days side by side with no indication of
# that. Today's own past events are still real and worth showing (e.g. a
# moonrise that already happened this morning).
# A rise or set can be genuinely ABSENT from a given calendar day (the
# lunar day is ~24h50m, ~50min longer than a solar day, so roughly once a
# month a given day has two moonrises and no moonset, or vice versa) --
# that value is sent as 0, a sentinel the Arduino side checks for and
# hides that column entirely rather than showing a placeholder "--".
# Sunrise/sunset are computed the same calendar-day way for consistency
# (previously read from the sun.sun entity's next_rising/next_setting,
# which has the identical "next from now" problem, just far less
# noticeable since sunrise/sunset only drift a few minutes a day) --
# 0 is possible there too in principle (polar day/night at extreme
# latitudes) though not realistic at this location.
# phase_angle_deg is 0-360 (0=new, 180=full) -- the Arduino side derives
# both illuminated % and which limb is lit directly from this one number
# (see renderMoonPhaseIcon() in Giga_YH_Dashboard_Unit2.ino), so it's the
# authoritative value; phase_name is included only for convenience.

# Dotted-style imports (not "from skyfield import almanac") -- pyscript's
# own execution/AST-transform model fails a top-level "from X import Y"
# for skyfield's submodules with a misleading AttributeError, even though
# the same statement works fine inside a function or as plain Python.
# Confirmed directly against this instance (pyscript custom_component,
# HA 2026.7.2) before settling on this form.
import skyfield.almanac as almanac
import skyfield.api as skyfield_api
import homeassistant.util.dt as dt_util
import datetime

WEATHER_ENTITY = "weather.forecast_home"
MQTT_TOPIC = "V1.0/Home/Almanac/Data"

# Ephemeris file cached here explicitly (not relying on pyscript's
# implicit working directory) -- ~17MB, downloaded once on first run,
# reused after.
_EPHEMERIS_PATH = "/config/pyscript/de421.bsp"

_ts = None
_eph = None
_observer = None

_PHASE_NAMES = [
    "New moon", "Waxing crescent", "First quarter", "Waxing gibbous",
    "Full moon", "Waning gibbous", "Last quarter", "Waning crescent",
]


def _ensure_loaded():
    # Lazy module-level state -- pyscript reloads this file on every edit,
    # so this avoids re-parsing the ephemeris file (slow) on every reload,
    # only on the first call after one.
    global _ts, _eph, _observer
    if _eph is not None:
        return
    _ts = skyfield_api.load.timescale()
    _eph = skyfield_api.load(_EPHEMERIS_PATH)
    _observer = skyfield_api.wgs84.latlon(hass.config.latitude, hass.config.longitude)


def _phase_name(angle_deg):
    idx = int(((angle_deg + 22.5) % 360) / 45)
    return _PHASE_NAMES[idx]


def _first_event(times, events, want_rise):
    # First timestamp in `times` whose paired boolean matches want_rise
    # (True picks a rise/sunrise transition, False a set/sunset one) --
    # 0 (the Arduino side's "no such event today" sentinel) if none match.
    for ti, is_rise in zip(times, events):
        if is_rise == want_rise:
            return int(ti.utc_datetime().timestamp())
    return 0


def _today_events(body_or_func):
    # body_or_func is either an ephemeris body (moon rise/set) or an
    # already-built almanac function (sun up/down) -- both work with
    # find_discrete the same way. Window is today's local calendar day,
    # midnight to midnight, NOT "from now" -- see this file's header
    # comment for why that distinction matters here.
    today_start = dt_util.now().replace(hour=0, minute=0, second=0, microsecond=0)
    today_end = today_start + datetime.timedelta(days=1)
    t0 = _ts.from_datetime(today_start)
    t1 = _ts.from_datetime(today_end)
    times, events = almanac.find_discrete(t0, t1, body_or_func)
    return _first_event(times, events, True), _first_event(times, events, False)


@time_trigger("cron(0 */6 * * *)")  # every 6 hours -- rise/set/phase change slowly
@service
def publish_almanac_data():
    _ensure_loaded()

    condition = state.get(WEATHER_ENTITY) or "unknown"
    weather_attrs = state.getattr(WEATHER_ENTITY) or {}
    temp_f = weather_attrs.get("temperature", "")

    sunrise_epoch, sunset_epoch = _today_events(almanac.sunrise_sunset(_eph, _observer))
    moonrise_epoch, moonset_epoch = _today_events(almanac.risings_and_settings(_eph, _eph["Moon"], _observer))

    t_now = _ts.from_datetime(dt_util.now())
    phase_angle = almanac.moon_phase(_eph, t_now).degrees
    phase_name = _phase_name(phase_angle)

    payload = (
        f"{temp_f}:{condition}:{sunrise_epoch}:{sunset_epoch}:"
        f"{moonrise_epoch}:{moonset_epoch}:{phase_angle:.1f}:{phase_name}"
    )
    mqtt.publish(topic=MQTT_TOPIC, payload=payload, retain=True)

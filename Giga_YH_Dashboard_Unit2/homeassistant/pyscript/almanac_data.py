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
# All four rise/set values are the NEXT occurrence from now (matching the
# existing tide screen's own "next event" semantics, not a fixed
# calendar-day window), as Unix epoch seconds (UTC) -- the Arduino side
# already has real NTP time and converts to local for display, same as
# everywhere else on this dashboard.
# phase_angle_deg is 0-360 (0=new, 180=full) -- the Arduino side derives
# both illuminated % and which limb is lit directly from this one number
# (see updateMoonPhaseIcon() in Giga_YH_Dashboard_Unit2.ino), so it's the
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
import os

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


def _next_moon_events(now_dt):
    # Returns (next_rise_epoch, next_set_epoch) -- searches a 3-day window
    # from now to comfortably guarantee both a rise and a set are found
    # regardless of where "now" falls in the current rise/set cycle.
    t0 = _ts.from_datetime(now_dt)
    t1 = _ts.from_datetime(now_dt + datetime.timedelta(days=3))
    f = almanac.risings_and_settings(_eph, _eph["Moon"], _observer)
    times, events = almanac.find_discrete(t0, t1, f)

    next_rise = next_set = None
    for ti, is_rise in zip(times, events):
        epoch = int(ti.utc_datetime().timestamp())
        if is_rise and next_rise is None:
            next_rise = epoch
        elif not is_rise and next_set is None:
            next_set = epoch
        if next_rise is not None and next_set is not None:
            break
    return next_rise, next_set


@time_trigger("cron(0 */6 * * *)")  # every 6 hours -- rise/set/phase change slowly
@service
def publish_almanac_data():
    _ensure_loaded()

    condition = state.get(WEATHER_ENTITY) or "unknown"
    weather_attrs = state.getattr(WEATHER_ENTITY) or {}
    temp_f = weather_attrs.get("temperature", "")

    sun_attrs = state.getattr("sun.sun") or {}
    sunrise_epoch = int(dt_util.parse_datetime(sun_attrs["next_rising"]).timestamp())
    sunset_epoch = int(dt_util.parse_datetime(sun_attrs["next_setting"]).timestamp())

    now = dt_util.now()
    moonrise_epoch, moonset_epoch = _next_moon_events(now)

    t_now = _ts.from_datetime(now)
    phase_angle = almanac.moon_phase(_eph, t_now).degrees
    phase_name = _phase_name(phase_angle)

    payload = (
        f"{temp_f}:{condition}:{sunrise_epoch}:{sunset_epoch}:"
        f"{moonrise_epoch}:{moonset_epoch}:{phase_angle:.1f}:{phase_name}"
    )
    mqtt.publish(topic=MQTT_TOPIC, payload=payload, retain=True)

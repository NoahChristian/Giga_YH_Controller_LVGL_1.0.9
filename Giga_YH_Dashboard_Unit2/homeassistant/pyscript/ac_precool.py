# pyscript: publishes today's AC energy split by time-of-use tier, the
# live AC draw, and what that split saved today, to MQTT (retained), for
# the Giga YH Unit 2 dashboard's AC Precool screen.
#
# Why this is tracked separately from the battery's Saved Today:
# pre-cooling the house during super off-peak is thermal storage, and it
# has no conversion loss. A kWh of cooling bought at noon for 13.1c is the
# same cooling that would have cost 80.2c at 6pm -- the building mass does
# the storing. Battery shifting moves the same load but pays a round-trip
# penalty to do it. Both look identical at the meter, so per-circuit data
# is the only way to tell them apart, and the cheaper lever is worth
# knowing about on its own.
#
# Measured against SDG&E interval data for Jul-Sep 2026: 98.2% of AC
# energy landed in super off-peak, 0.5% on-peak, worth roughly $50/month
# of the ~$240/month total shift.
#
# Entities: edit AC_ENERGY_ENTITIES/AC_POWER_ENTITIES below to match your
# own monitor's entity IDs. The defaults are this house's Refoss channels.
#   - energy: per-leg DAILY-RESETTING cumulative kWh for the AC circuit
#   - power:  per-leg instantaneous W for the same circuit
# Both legs are summed; a 240V condenser draws through both.
#
# Payload (colon-delimited, matching almanac_data.py's convention -- see
# subtopicAcPrecool in the .ino):
#     kwhToday:kwhSop:kwhOff:kwhOn:wattsNow:savedToday
# e.g. "8.42:8.26:0.12:0.04:1823:2.27"
#
# NOTE: plain #-comments rather than a module docstring, deliberately --
# see battery_day_curve.py's header for the paste-into-nano corruption
# that made an unterminated """ into a silent SyntaxError.

from homeassistant.components.recorder import history
import homeassistant.util.dt as dt_util
import datetime

AC_ENERGY_ENTITIES = [
    "sensor.refoss_smart_energy_monitor_ac_l1_today_energy",
    "sensor.refoss_smart_energy_monitor_ac_l2_today_energy",
]
AC_POWER_ENTITIES = [
    "sensor.refoss_smart_energy_monitor_ac_l1_power",
    "sensor.refoss_smart_energy_monitor_ac_l2_power",
]

MQTT_TOPIC = "V1.0/Home/AC/Precool"
BUCKET_MINUTES = 15

# SDG&E Schedule EV-TOU-5 total rates ($/kWh), effective 2026-08-01.
# Keep in step with RATE_* in Giga_YH_Dashboard_Unit2.ino -- the sketch is
# the source of truth for what the display claims; this only needs to
# agree with it closely enough that the dollar figure is not misleading.
RATES = {
    "summer": {"ON": 0.80205, "OFF": 0.49627, "SOP": 0.13090},
    "winter": {"ON": 0.52383, "OFF": 0.46566, "SOP": 0.12332},
}

# What the AC would have cost per kWh without pre-cooling: this house's
# own measured pre-battery tier mix (October 2025, the one pre-battery
# summer month in the SDG&E record -- 43.17% super off-peak / 33.43%
# off-peak / 23.41% on-peak), blended against summer rates.
#
# Deliberately NOT "all on-peak", which would flatter the result. This is
# what this house actually did before, with the same appliances.
BASELINE_MIX = {"SOP": 0.4317, "OFF": 0.3343, "ON": 0.2341}

# TOU windows, mirroring buildTouWindows() in the .ino. Hours are
# [start, end). Keep the two in step.
WEEKDAY_WINDOWS = [(0, 6, "SOP"), (6, 10, "OFF"), (10, 14, "SOP"),
                   (14, 16, "OFF"), (16, 21, "ON"), (21, 24, "OFF")]
WEEKEND_WINDOWS = [(0, 14, "SOP"), (14, 16, "OFF"), (16, 21, "ON"), (21, 24, "OFF")]


def _nth_weekday(year, month, weekday, n):
    # n-th `weekday` (Mon=0) of the month; n=-1 means the last one.
    if n > 0:
        d = datetime.date(year, month, 1)
        d += datetime.timedelta(days=(weekday - d.weekday()) % 7)
        return d + datetime.timedelta(weeks=n - 1)
    nxt = datetime.date(year + (month == 12), month % 12 + 1, 1)
    d = nxt - datetime.timedelta(days=1)
    return d - datetime.timedelta(days=(d.weekday() - weekday) % 7)


def _is_holiday(d):
    # Same eight holidays the sketch derives, including the
    # Sunday -> observed-Monday rule.
    raw = [
        datetime.date(d.year, 1, 1),
        _nth_weekday(d.year, 1, 0, 3),
        _nth_weekday(d.year, 2, 0, 3),
        _nth_weekday(d.year, 5, 0, -1),
        datetime.date(d.year, 7, 4),
        _nth_weekday(d.year, 9, 0, 1),
        _nth_weekday(d.year, 11, 3, 4),
        datetime.date(d.year, 12, 25),
    ]
    observed = set()
    for h in raw:
        observed.add(h + datetime.timedelta(days=1) if h.weekday() == 6 else h)
    return d in observed


def _tier_at(dt):
    weekend = dt.weekday() >= 5 or _is_holiday(dt.date())
    for a, b, t in (WEEKEND_WINDOWS if weekend else WEEKDAY_WINDOWS):
        if a <= dt.hour < b:
            return t
    return "OFF"


def _season(d):
    # SDG&E: summer Jun 1 - Oct 31, winter Nov 1 - May 31.
    return "summer" if 6 <= d.month <= 10 else "winter"


def _value_at_or_before(state_list, target_time, cast):
    # Same helper as battery_day_curve.py -- last good value at or before
    # target_time, skipping "unavailable"/"unknown".
    result = None
    for s in state_list:
        if s.last_changed > target_time:
            break
        try:
            result = cast(s.state)
        except (ValueError, TypeError):
            pass
    return result


def _num(entity_id, default=0.0):
    try:
        return float(state.get(entity_id))
    except (ValueError, TypeError):
        return default


@time_trigger("cron(*/5 * * * *)")  # every 5 min
@service
def publish_ac_precool():
    now = dt_util.now()  # timezone-AWARE, to compare against s.last_changed
    today_start = now.replace(hour=0, minute=0, second=0, microsecond=0)

    # Per-leg daily energy counters, walked in 15-minute steps. The
    # counters reset at midnight, so a NEGATIVE delta between buckets is a
    # reset rather than negative energy -- treated as zero, not clamped
    # into a bogus positive. Attribution uses the bucket's START, matching
    # how both SDG&E and the sketch attribute an interval to a period.
    by_tier = {"SOP": 0.0, "OFF": 0.0, "ON": 0.0}
    total = 0.0

    for entity in AC_ENERGY_ENTITIES:
        hist = task.executor(history.state_changes_during_period,
                             hass, today_start, now, entity)
        states = hist.get(entity, [])
        if not states:
            continue

        prev = None
        t = today_start
        while t <= now:
            v = _value_at_or_before(states, t, float)
            if v is not None:
                if prev is not None:
                    delta = v - prev
                    if delta > 0:                 # negative == midnight reset
                        by_tier[_tier_at(t)] += delta
                        total += delta
                prev = v
            t += datetime.timedelta(minutes=BUCKET_MINUTES)

    # Explicit loops throughout rather than generator expressions:
    # pyscript's restricted AST interpreter raises
    # "NotImplementedError: not implemented ast_generatorexp" on them.
    watts_now = 0.0
    for e in AC_POWER_ENTITIES:
        watts_now += _num(e)

    # What it cost, versus what the pre-battery mix would have cost for
    # the same kWh. Both priced at today's season, so the figure is a
    # behaviour difference and not a seasonal one.
    rates = RATES[_season(now.date())]
    actual = 0.0
    for t in by_tier:
        actual += by_tier[t] * rates[t]
    baseline_rate = 0.0
    for t in BASELINE_MIX:
        baseline_rate += BASELINE_MIX[t] * rates[t]
    saved = total * baseline_rate - actual

    payload = "{:.2f}:{:.2f}:{:.2f}:{:.2f}:{:.0f}:{:.2f}".format(
        total, by_tier["SOP"], by_tier["OFF"], by_tier["ON"], watts_now, saved)

    mqtt.publish(topic=MQTT_TOPIC, payload=payload, retain=True)
    log.info("ac_precool: %s", payload)

# pyscript: publishes today's AND yesterday's battery SoC +
# Charging/Discharging/Idle curve to MQTT (retained), for the Giga YH
# Unit 2 dashboard's Battery screen chart and its Saved Today/Saved
# Yesterday figures (the Arduino side prices each curve's discharging
# segments against the real historical TOU tier -- see
# computeSavedFromSocCurve() in Giga_YH_Dashboard_Unit2.ino).
#
# Entities: edit SOC_ENTITY/CHARGING_ENTITY/CURRENT_ENTITY below to match
# your own BMS's actual entity IDs before running this -- the placeholders
# here won't resolve to anything on a real HA instance.
#   - SoC:      a 0-100 battery state-of-charge sensor
#   - Charging: a binary_sensor that's "on" while the battery is charging
#   - Current:  a sensor reporting battery current in Amps (negative while
#               discharging, per the DISCHARGE_CURRENT_THRESHOLD check below)
#
# State per bucket is RE-DERIVED from the same two conditions the existing
# HA automations already use (there's no single entity that tracks
# Charging/Discharging/Idle directly):
#   Charging     if charging binary_sensor == "on"
#   Discharging  elif current < -1
#   Idle         otherwise
#
# Encoding: comma-separated "bucket:soc:state" triples, one per 15-minute
# bucket from midnight to now (bucket = index 0-95 of that 15-min slot in
# the day, e.g. bucket 8 = 2:00 AM). The explicit bucket index lets the
# Arduino-side parser place the first point of the day at the correct
# X position even though buckets before the first reading are omitted, not
# padded -- the list is only as long as there's real data.
# State is a single digit (0=Idle, 1=Charging, 2=Discharging) matching this
# project's own g_batteryState convention.
#
# NOTE: header rewritten from a triple-quoted docstring to plain #-comments
# after a real-world paste-into-nano corruption dropped the opening """,
# which silently became a SyntaxError instead of an obviously-broken single
# line -- plain comments make that failure mode impossible.

from homeassistant.components.recorder import history
import homeassistant.util.dt as dt_util
import datetime

SOC_ENTITY = "sensor.YOUR_BATTERY_SOC_ENTITY"
CHARGING_ENTITY = "binary_sensor.YOUR_BATTERY_CHARGING_ENTITY"
CURRENT_ENTITY = "sensor.YOUR_BATTERY_CURRENT_ENTITY"
DISCHARGE_CURRENT_THRESHOLD = -1.0  # Amps -- matches the existing condition's "below: -1"
BUCKET_MINUTES = 15
MQTT_TOPIC = "V1.0/Home/Battery/DaySOC"
MQTT_TOPIC_YESTERDAY = "V1.0/Home/Battery/YesterdaySOC"

STATE_IDLE = "0"
STATE_CHARGING = "1"
STATE_DISCHARGING = "2"


def _value_at_or_before(state_list, target_time, cast):
    # Given a list of HA State objects (recorder history for one entity,
    # already sorted oldest-first), returns the cast()'d state value that
    # was current at target_time, or None if target_time is before the
    # first recorded state.
    result = None
    for s in state_list:
        if s.last_changed > target_time:
            break
        try:
            result = cast(s.state)
        except (ValueError, TypeError):
            pass  # "unavailable"/"unknown" states -- keep the last good value
    return result


def _build_soc_curve(start, end_inclusive, soc_states, charging_states, current_states):
    # Walks [start, end_inclusive] in BUCKET_MINUTES steps, returning the
    # "bucket:soc:state,..." payload string for that range. Shared by
    # today's (end_inclusive=now, a partial day) and yesterday's
    # (end_inclusive=23:45, the last bucket of a full closed day) curves.
    points = []
    t = start
    bucket = 0
    while t <= end_inclusive:
        soc = _value_at_or_before(soc_states, t, lambda v: round(float(v)))
        is_charging = _value_at_or_before(charging_states, t, lambda v: v == "on")
        current = _value_at_or_before(current_states, t, float)

        if soc is not None:
            if is_charging:
                state_code = STATE_CHARGING
            elif current is not None and current < DISCHARGE_CURRENT_THRESHOLD:
                state_code = STATE_DISCHARGING
            else:
                state_code = STATE_IDLE
            points.append(f"{bucket}:{soc}:{state_code}")
        # else: no SoC data yet for this bucket -- omit it, don't pad

        t += datetime.timedelta(minutes=BUCKET_MINUTES)
        bucket += 1

    return ",".join(points)


@time_trigger("cron(*/10 * * * *)")  # every 10 min
@service
def publish_battery_day_curve():
    now = dt_util.now()  # timezone-AWARE, matching s.last_changed's own convention -- plain
                          # datetime.datetime.now() is naive and can't be compared against it
    today_start = now.replace(hour=0, minute=0, second=0, microsecond=0)
    yesterday_start = today_start - datetime.timedelta(days=1)
    yesterday_end = today_start - datetime.timedelta(minutes=BUCKET_MINUTES)  # yesterday's 23:45 bucket

    # One history query spanning both days, rather than two separate
    # queries -- _build_soc_curve() just needs a state list to search
    # within, it doesn't care which day a given target_time falls on.
    soc_hist = task.executor(history.state_changes_during_period, hass, yesterday_start, now, SOC_ENTITY)
    charging_hist = task.executor(history.state_changes_during_period, hass, yesterday_start, now, CHARGING_ENTITY)
    current_hist = task.executor(history.state_changes_during_period, hass, yesterday_start, now, CURRENT_ENTITY)

    soc_states = soc_hist.get(SOC_ENTITY, [])
    charging_states = charging_hist.get(CHARGING_ENTITY, [])
    current_states = current_hist.get(CURRENT_ENTITY, [])

    today_payload = _build_soc_curve(today_start, now, soc_states, charging_states, current_states)
    yesterday_payload = _build_soc_curve(yesterday_start, yesterday_end, soc_states, charging_states, current_states)

    mqtt.publish(topic=MQTT_TOPIC, payload=today_payload, retain=True)
    mqtt.publish(topic=MQTT_TOPIC_YESTERDAY, payload=yesterday_payload, retain=True)

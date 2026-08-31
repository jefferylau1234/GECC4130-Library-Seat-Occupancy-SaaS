from fastapi import APIRouter, Header, HTTPException, status, Response, Depends
from pydantic import BaseModel, Field
from datetime import datetime, timezone, time, timedelta
from typing import List, Optional, Union
import os

from sqlalchemy.orm import Session
from sqlalchemy import text
from ..db.db import get_db, EnvironmentalReading, Sensors
from sqlalchemy.dialects.postgresql import insert
from zoneinfo import ZoneInfo
from datetime import datetime, timezone, time, timedelta
from backend.data_structure import OccupancyStore, ForecastPoint
from zoneinfo import ZoneInfo

router = APIRouter(prefix = "")

HKT = ZoneInfo("Asia/Hong_Kong")

# Get environmental data for all zones
@router.get("/check")
def get(db: Session = Depends(get_db)):
    sql = text("""
        SELECT
            zone,
            zone_type,
            temperature_c,
            noise_db,
            humidity_percent,
            updated_at,
            EXTRACT(
                EPOCH FROM (
                    NOW() - updated_at
                )
            ) AS seconds_since_update
        FROM public.environmental_readings
        WHERE
            (
                zone_type = 'Floor Overview'
                AND timezone('Asia/Hong_Kong', NOW())::time >= TIME '08:00:00'
                AND timezone('Asia/Hong_Kong', NOW())::time < TIME '22:00:00'
                AND updated_at < NOW() - INTERVAL '31 minutes'
            )
            OR
            (
                zone_type IS DISTINCT FROM 'Floor Overview'
                AND updated_at < NOW() - INTERVAL '31 minutes'
            )
        ORDER BY updated_at ASC
    """)
    result = db.execute(sql)
    rows = result.mappings().all()


    now = datetime.now(HKT)

    WORKING_START_HOUR = 8
    WORKING_END_HOUR = 22
    MAX_WORKING_STATUS_AGE = timedelta(minutes=31)

    # True from 08:00 up to, but not including, 22:00.
    is_working_hours = (
        WORKING_START_HOUR <= now.hour < WORKING_END_HOUR
    )

    expected_status = "working" if is_working_hours else "sleeping"

    sensors = db.query(Sensors).all()

    failed_sensors = []

    for sensor in sensors:
        sensor_id = sensor.sensorId
        actual_status = sensor.status
        updated_at_string = sensor.updated_at

        updated_at = parse_sensor_updated_at(updated_at_string)

        reasons = []
        age_seconds = None

        # Rule 1:
        # 08:00–22:00: sensor must say "working".
        # Outside that window: sensor must say "sleeping".
        if actual_status != expected_status:
            reasons.append(
                f"Expected status '{expected_status}', "
                f"but received '{actual_status}'."
            )

        # Rule 2:
        # During working hours, the status must be no older than 31 minutes.
        # Outside working hours, only check whether the status is "sleeping".
        if is_working_hours:
            if updated_at is None:
                reasons.append(
                    "updated_at cannot be parsed as a valid datetime."
                )
            else:
                age = now - updated_at
                age_seconds = age.total_seconds()

                if age < timedelta(seconds=0):
                    reasons.append(
                        "updated_at is in the future."
                    )

                elif age > MAX_WORKING_STATUS_AGE:
                    reasons.append(
                        "Last status update is older than 31 minutes."
                    )

        # If one or more rules failed, return this sensor.
        if reasons:
            failed_sensors.append(
                {
                    "sensorId": sensor_id,
                }
            )



    return {
        "fail_count": len(rows) + len(failed_sensors),
        "failed environment sensors": [
            {
                "zone_id": row["zone"],
            }
            for row in rows
        ],
        "failed gate sensors": failed_sensors,
    }






def parse_sensor_updated_at(value: str) -> datetime | None:
    """
    Convert Sensors.updated_at VARCHAR text into a Hong Kong datetime.

    Supports:
    - New format: 2026-08-30 01:52pm
    - Old format: 2026-08-29 14:11:58.384321
    - Old format: 2026-08-29 14:11:58
    """

    if not value:
        return None

    value = value.strip()

    formats = [
        "%Y-%m-%d %I:%M%p",      # New: 2026-08-30 01:52pm
        "%Y-%m-%d %H:%M:%S.%f",  # Old: 2026-08-29 14:11:58.384321
        "%Y-%m-%d %H:%M:%S",     # Old: 2026-08-29 14:11:58
    ]

    for fmt in formats:
        try:
            parsed_time = datetime.strptime(value.upper(), fmt)
            return parsed_time.replace(tzinfo=HKT)
        except ValueError:
            pass

    return None
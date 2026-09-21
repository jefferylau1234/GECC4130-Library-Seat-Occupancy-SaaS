from fastapi import APIRouter, Header, HTTPException, status, Response, Depends
from pydantic import BaseModel, Field
from datetime import datetime, time, timedelta
from typing import List, Optional, Union
import os

from sqlalchemy.orm import Session
from ..db.db import get_db, OccupancyReading, EnvironmentalReading, OccupancyRecord, Sensors, Visiting
from sqlalchemy.dialects.postgresql import insert
from sqlalchemy import text
import random

router = APIRouter(prefix = "/api")


# cookie
class PreferenceIn(BaseModel):
    purpose: str
    preferred_floor: str
    temporature: str
    noise: str
    humidity: str
    take: str


@router.post("/preferences")
def save_preferences(data: PreferenceIn, response: Response):
    # can save to database for building recommended zone model (db.save(data.model_dump())

    cookie_value = f"{data.purpose}|{data.preferred_floor}|{data.temporature}|{data.noise}|{data.humidity}|{data.take}"

    print(cookie_value)

    response.set_cookie(            # to tell frontend browser auto add cookie header next time and create cookie
        key = "cc_pref",              # header Cookie: cc_pref=Quiet zone|2F
        value = cookie_value,
        max_age=60 * 60 * 24 * 180,
        path="/",
        samesite="lax",
        secure=False,  
        httponly=False
    )

    return {
        "status": "ok",
        "saved": True,
        "preference": {
            "purpose": data.purpose,
            "preferred_floor": data.preferred_floor,
            "temporature": data.temporature,
            "noise": data.noise,
            "humidity": data.humidity,
            "take": data.take
        }
    }




from datetime import datetime
from zoneinfo import ZoneInfo

HKT = ZoneInfo("Asia/Hong_Kong")



# sensors payload

class OccupancySingle(BaseModel):
    signal: int # 0 or 1

class OccupancyBuffer(BaseModel):
    signal: List[int] # [0, 1, 1, 0, 0, 1]

class OccupancyRecorded(BaseModel):
    signal: int      #  0 or 1
    recorded_at: str # "2026-08-07_15:12:59"

class OccupancyReader(BaseModel):
    series: List[OccupancyRecorded]         
# like series = [{"signal": 1, "recorded_at": "2026-08-07_15:12:59"}, 
#                {"signal": 0, "recorded_at": "2026-08-07_15:58:12"}]




OccupancyReadingCreate = Union[OccupancySingle, OccupancyBuffer, OccupancyReader, OccupancyRecorded]


class EnvironmentalReadingCreate(BaseModel):
    zone: str = Field(min_length=1, max_length=26)
    temperature_c: Optional[float] = Field(default=None, ge=-20, le=60)
    humidity: Optional[float] = Field(default=None, ge=0, le=100)
    noise_db: Optional[float] = Field(default=None, ge=0, le=150)



class SensorStatus(BaseModel):
    sensorId: str = Field(min_length=1, max_length=26)
    status: str = Field(min_length=1, max_length=26)



class Visit(BaseModel):
    status: bool = Optional[bool]




SENSOR_API_KEY = os.getenv("SENSOR_API_KEY")


def verify_sensor_key(x_sensor_key: str | None) -> None:
    print(x_sensor_key)
    print(SENSOR_API_KEY)
    if x_sensor_key != SENSOR_API_KEY:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid sensor API key",
        )


now = datetime.now(HKT)
hour_str = now.strftime("%Y-%m-%d_%H")
print(hour_str)



# sensor signal: 
# 1 => +1 people
# 0 => -1 people







# example
# headers: {x-sensor-key: ABCDEFG12345}
# body: 
# {
#   "signal": 0 or 1,
# }








# for sensors to pass occupancy data
@router.post("/sensor/occupancy")
def create_occupancy_reading(sensor: OccupancyReadingCreate, x_sensor_key: str | None = Header(default=None), db: Session = Depends(get_db)):
    verify_sensor_key(x_sensor_key)

    now = datetime.now(HKT)
    formatted_time = now.strftime("%Y-%m-%d_%H:%M:%S")
    if not time(8, 20) < now.time() < time(22, 0):    
        print("CC library is still not opened yet")
        return {"message": "CC library is not opened yet",}

    hour_str = now.strftime("%Y-%m-%d_%H")
    row = db.query(OccupancyReading).filter(OccupancyReading.hour_str == hour_str).first()

    prev = now - timedelta(hours=1)
    while (row is None and prev.time() > time(7, 00)):
        i = prev.strftime("%Y-%m-%d_%H")
        row = db.query(OccupancyReading).filter(OccupancyReading.hour_str == i).first()
        prev -= timedelta(hours=1)

    current_occupancy = 0 if row is None else row.occupant_count


    i = ""
    j = 0
    RANDOM = random.choices([1, 0], weights=[0.9, 0.1], k=1)[0]
    MINUS = random.choices([1, 2], weights=[0.8, 0.2], k=1)[0]

    if isinstance(sensor, OccupancyBuffer):
        for s in sensor.signal:
            if s == 1:
                current_occupancy += RANDOM
                j+= RANDOM
            elif s == 0:
                current_occupancy -= MINUS
                j-=MINUS
        i = f"+{j}" if j >= 0 else f"{j}"

    elif isinstance(sensor, OccupancySingle):
        if sensor.signal == 1:
            current_occupancy += RANDOM
        elif sensor.signal == 0:
            current_occupancy -= MINUS
        i = f"+{RANDOM}" if sensor.signal == 1 else f"-{MINUS}"

    elif isinstance(sensor, OccupancyRecorded):
        if sensor.signal == 1:
            current_occupancy += RANDOM
            j+= RANDOM
        elif sensor.signal == 0:
            current_occupancy -= MINUS
            j-=MINUS

        i = f"+{j}" if j >= 0 else f"{j}"

    elif isinstance(sensor, OccupancyReader):
        for r in sensor.series:
            if r.signal == 1:
                current_occupancy += RANDOM
                j+= RANDOM
            elif r.signal == 0:
                current_occupancy -= MINUS
                j-=MINUS
            i = f"+{j}" if j >= 0 else f"{j}"






    current_occupancy = 0 if current_occupancy < 0 else current_occupancy



    record = insert(OccupancyRecord).values(
        recorded_at = formatted_time,
        occupant_change = i,
        occupant_count = current_occupancy,
    )
    db.execute(record)


    stmt = insert(OccupancyReading).values(
        hour_str = hour_str,
        occupant_count = current_occupancy,
        updated_at = now,
    )

    stmt = stmt.on_conflict_do_update(
        index_elements=[OccupancyReading.hour_str],
        set_={
            "occupant_count": stmt.excluded.occupant_count,
            "updated_at": stmt.excluded.updated_at,
        },
    )

    db.execute(stmt)
    db.commit()

    received_data = (
        [r.signal for r in sensor.series]
        if isinstance(sensor, OccupancyReader)
        else sensor.signal
    )

    return {
        "message": "Occupancy reading received",
        "current_occupancy": current_occupancy,
        "received_data": received_data,
        "occupancy": i,
        "received_at": now,
        "random": RANDOM,
    }








# example
# headers: {x-sensor-key: ABCDEFG12345}
# body:
# {
#   "zone": "2F_study1" or "1F_hub3" or "G_pc8",
#   "temperature_c": 25,
#   "humidity": 0.79,
#   "noise_db": 30,
# }

import random

SIMULATED_ZONES = [
    "1F_study4",
    "GF_study6",
    "2F_study9",
    "2F_study8",
    "2F_study5",
    "2F_study3",
    "1F_pc13",
    "1F_study12",
    "1F_study11",
    "1F_study9",
    "1F_hub8",
    "1F_study7",
    "1F_study10",
    "GF_pc8",
    "GF_study7",
    "GF_study9",
]


def get_zone_type(zone: str) -> str:
    raw_zone_type = zone.split("_")[1].rstrip("0123456789")

    if raw_zone_type == "study":
        return "Quiet Study zone"
    elif raw_zone_type == "pc":
        return "PC zone"
    elif raw_zone_type == "overview":
        return "Floor Overview"
    elif raw_zone_type == "hub":
        return "Study Hubs"

    return "Unknown zone"


@router.post("/sensor/environmental-data")
def create_environment_reading(
    sensor: EnvironmentalReadingCreate,
    x_sensor_key: str | None = Header(default=None),
    db: Session = Depends(get_db),
):
    verify_sensor_key(x_sensor_key)

    now = datetime.now(HKT)

    # Apply your sensor calibration.
    real_temperature = sensor.temperature_c - 2
    real_noise = sensor.noise_db - 10
    real_humidity = sensor.humidity

    # Save the actual submitted sensor data.
    stmt = insert(EnvironmentalReading).values(
        zone=sensor.zone,
        zone_type=get_zone_type(sensor.zone),
        temperature_c=real_temperature,
        noise_db=real_noise,
        humidity_percent=real_humidity,
        updated_at=now,
    )

    stmt = stmt.on_conflict_do_update(
        index_elements=[EnvironmentalReading.zone],
        set_={
            "zone_type": get_zone_type(sensor.zone),
            "temperature_c": real_temperature,
            "noise_db": real_noise,
            "humidity_percent": real_humidity,
            "updated_at": stmt.excluded.updated_at,
        },
    )

    db.execute(stmt)

    # Generate estimated readings only when the real sensor is 1F_study1.
    if sensor.zone == "1F_study1":
        for zone in SIMULATED_ZONES:
            simulated_temperature = round(
                max(-20, min(60, real_temperature + random.uniform(-0.5, 0.5))),
                1,
            )

            simulated_noise = round(
                max(0, min(150, real_noise + random.uniform(-3, 3))),
                1,
            )

            simulated_humidity = round(
                max(0, min(100, real_humidity + random.uniform(-3, 3))),
                1,
            )

            stmt = insert(EnvironmentalReading).values(
                zone=zone,
                zone_type=get_zone_type(zone),
                temperature_c=simulated_temperature,
                noise_db=simulated_noise,
                humidity_percent=simulated_humidity,
                updated_at=now,
            )

            stmt = stmt.on_conflict_do_update(
                index_elements=[EnvironmentalReading.zone],
                set_={
                    "temperature_c": simulated_temperature,
                    "noise_db": simulated_noise,
                    "humidity_percent": simulated_humidity,
                    "updated_at": stmt.excluded.updated_at,
                },
            )

            db.execute(stmt)




    db.commit()
    
    return {
        "message": "Environmental data received",
    }




# for sensors to pass status
@router.post("/sensor/status")
def create_occupancy_reading(sensor: SensorStatus, x_sensor_key: str | None = Header(default=None), db: Session = Depends(get_db)):
    verify_sensor_key(x_sensor_key)

    now = f"{datetime.now(HKT).strftime('%Y-%m-%d %I:%M%p').lower()}"

    stmt = insert(Sensors).values(
        sensorId = sensor.sensorId,
        status = sensor.status,
        updated_at = now,
    )

    stmt = stmt.on_conflict_do_update(
        index_elements=[Sensors.sensorId],
        set_={ 
            "status": stmt.excluded.status,
            "updated_at": stmt.excluded.updated_at,
        },
    )

    db.execute(stmt)
    db.commit()

    if (sensor.sensorId == "entry1"):
        updated_overviews = refresh_floor_overviews(db)



    return {
        "message": "sensor status received",
    }




@router.post("/visiting")
def visiting(browser: Visit, x_sensor_key: str | None = Header(default=None), db: Session = Depends(get_db)):
    verify_sensor_key(x_sensor_key)

    now = f"{datetime.now(HKT).strftime('%Y-%m-%d %I:%M%p').lower()}"
    

    stmt = insert(Visiting).values(
        recorded_at = now,
        visiting = browser.status
    )

    db.execute(stmt)
    db.commit()


    return { ""}




def refresh_floor_overviews(db: Session) -> list[dict]:
    """
    Calculate environmental averages for GF, 1F, and 2F.

    It excludes existing Floor Overview rows, so an overview
    never accidentally gets included in its own average.
    """

    sql = text("""
        SELECT
            split_part(zone, '_', 1) AS floor_id,
            AVG(temperature_c) AS average_temperature_c,
            AVG(noise_db) AS average_noise_db,
            AVG(humidity_percent) AS average_humidity_percent,
            COUNT(*) AS source_zone_count
        FROM public.environmental_readings
        WHERE zone_type != 'Floor Overview'
          AND split_part(zone, '_', 1) IN ('GF', '1F', '2F')
        GROUP BY split_part(zone, '_', 1)
        ORDER BY floor_id
    """)

    result = db.execute(sql)
    floor_rows = result.mappings().all()

    now = datetime.now(HKT)

    updated_overviews = []




    for row in floor_rows:
        floor_id = row["floor_id"]

        average_temperature = round(
            float(row["average_temperature_c"]),
            2,
        )

        average_noise = round(
            float(row["average_noise_db"]),
            2,
        )

        average_humidity = round(
            float(row["average_humidity_percent"]),
            2,
        )

        # Normally one overview row per floor.
        overview_zones = [f"{floor_id}_overview"]

        # LG_overview must always have exactly the same values as GF_overview.
        if floor_id == "GF":
            overview_zones.append("LG_overview")

        # Save/update each overview row.
        for overview_zone in overview_zones:
            stmt = insert(EnvironmentalReading).values(
                zone=overview_zone,
                zone_type="Floor Overview",
                temperature_c=average_temperature,
                noise_db=average_noise,
                humidity_percent=average_humidity,
                updated_at=now,
            )

            stmt = stmt.on_conflict_do_update(
                index_elements=[EnvironmentalReading.zone],
                set_={
                    "zone_type": "Floor Overview",
                    "temperature_c": stmt.excluded.temperature_c,
                    "noise_db": stmt.excluded.noise_db,
                    "humidity_percent": stmt.excluded.humidity_percent,
                    "updated_at": stmt.excluded.updated_at,
                },
            )

            db.execute(stmt)

            updated_overviews.append(
                {
                    "zone": overview_zone,
                    "temperature_c": average_temperature,
                    "noise_db": average_noise,
                    "humidity_percent": average_humidity,
                    "source_zone_count": row["source_zone_count"],
                }
            )



    db.commit()

    return updated_overviews
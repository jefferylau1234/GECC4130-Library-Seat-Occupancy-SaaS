from fastapi import APIRouter, Header, HTTPException, status, Response, Depends
from pydantic import BaseModel, Field
from datetime import datetime, time, timedelta
from typing import List, Optional, Union
import os

from sqlalchemy.orm import Session
from ..db.db import get_db, OccupancyReading, EnvironmentalReading, OccupancyRecord
from sqlalchemy.dialects.postgresql import insert


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
    # if not time(8, 20) < now.time() < time(22, 0):    
    #     print("CC library is still not opened yet")
    #     return {"message": "CC library is not opened yet",}

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
    if isinstance(sensor, OccupancyBuffer):
        for s in sensor.signal:
            if s == 1:
                current_occupancy += 1
                j+=1
            elif s == 0:
                current_occupancy -= 1
                j-=1
        i = f"+{j}" if j >= 0 else f"{j}"

    elif isinstance(sensor, OccupancySingle):
        if sensor.signal == 1:
            current_occupancy += 1
        elif sensor.signal == 0:
            current_occupancy -= 1
        i = "+1" if sensor.signal == 1 else "-1"

    elif isinstance(sensor, OccupancyRecorded):
        if sensor.signal == 1:
            current_occupancy += 1
            j+=1
        elif sensor.signal == 0:
            current_occupancy -= 1
            j-=1

        i = f"+{j}" if j >= 0 else f"{j}"

    elif isinstance(sensor, OccupancyReader):
        for r in sensor.series:
            if r.signal == 1:
                current_occupancy += 1
                j+=1
            elif r.signal == 0:
                current_occupancy -= 1
                j-=1
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



# for sensors to pass environmantal data
@router.post("/sensor/environmental-data")
def create_environment_reading(sensor: EnvironmentalReadingCreate, x_sensor_key: str | None = Header(default=None), db: Session = Depends(get_db)):
    verify_sensor_key(x_sensor_key)

    now = datetime.now(HKT)
    sensor.humidity *= 1

    type = sensor.zone.split("_")[1].rstrip("0123456789")


    if type == "study":
        type = "Quiet Study zone"
    elif type == "pc": 
        type = "PC zone"
    elif type == "overview":
        type = "Floor Overview"
    elif type == "hub":
        type = "Study Hubs"


    stmt = insert(EnvironmentalReading).values(
        zone= sensor.zone,
        zone_type= type,
        temperature_c= sensor.temperature_c,
        noise_db= sensor.noise_db,
        humidity_percent= sensor.humidity,
        updated_at= now,
    )

    stmt = stmt.on_conflict_do_update(
        index_elements=[EnvironmentalReading.zone],
        set_={
            "zone_type": type,
            "temperature_c": sensor.temperature_c,
            "noise_db": sensor.noise_db,
            "humidity_percent": sensor.humidity,
            "updated_at": stmt.excluded.updated_at,
        },
    )


    db.execute(stmt)
    db.commit()

    return {
        "message": "Environmental data received",
    }

from datetime import datetime, timezone, time, timedelta
from backend.data_structure import OccupancyStore, ForecastPoint
from zoneinfo import ZoneInfo

HKT = ZoneInfo("Asia/Hong_Kong")

def get_current_occupancy_data(db, OccupancyReading):

    row = db.query(OccupancyReading).order_by(OccupancyReading.updated_at.desc()).first()
    if not row or not row.hour_str:
        return {
            "library_name": "CUHK CC Library",
            "current_occupancy": 0,
            "last_updated": datetime.now(timezone.utc),
            "message": "Library is closed 😭",
        }


    now = datetime.now(HKT)
    today = now.strftime("%Y-%m-%d")
    date = row.hour_str.split("_")[0] 

    current_occupancy = row.occupant_count if date == today else 0

    if current_occupancy >= 500:
        message = "Overload 🌋💀"
    elif current_occupancy >= 400:
        message = "Very crowded 🤯"
    elif current_occupancy >= 300:
        message = "Busy now 🔥"
    elif current_occupancy >= 200:
        message = "Moderately busy 🐝"
    elif current_occupancy >= 100:
        message = "Getting active ☕"
    else:
        message = "Available 🏝️"

    w = now.weekday()
    t = now.time()


    if w in (0, 1, 2, 3, 4) and not (time(8, 20) < t < time(22, 0)):
        current_occupancy = 0
        message = "Library is closed 😭"
    elif w == 5 and not (time(8, 20) < t < time(19, 0)):
        current_occupancy = 0
        message = "Library is closed 😭"
    elif w == 6 and not (time(11, 0) < t < time(19, 0)):
        current_occupancy = 0
        message = "Library is closed 😭"

    return {
        "library_name": "CUHK CC Library",
        "current_occupancy": current_occupancy,
        "last_updated": datetime.now(timezone.utc),
        "message": message,
    }




def get_today_occupancy_data(db, model):
    today = datetime.now(HKT).strftime("%Y-%m-%d")

    rows = (
        db.query(model)
        .filter(model.hour_str.like(f"{today}_%"))
        .order_by(model.hour_str.asc())
        .all()
    )

    series = []

    for row in rows:
        if not row.hour_str:
            continue

        hour = row.hour_str.split("_")[1]

        series.append({
            "time": f"{hour}:00",
            "occupancy": row.occupant_count or 0,
        })

    return {
        "series": series,
    }





def get_last_week_occupancy_data(db, model):
    last_week_date = (
        datetime.now(HKT).date() - timedelta(days=7)
    ).strftime("%Y-%m-%d")

    rows = (
        db.query(model)
        .filter(model.hour_str.like(f"{last_week_date}_%"))
        .order_by(model.hour_str.asc())
        .all()
    )

    series = []

    for row in rows:
        if not row.hour_str:
            continue

        hour = row.hour_str.split("_")[1]

        series.append({
            "time": f"{hour}:00",
            "occupancy": row.occupant_count or 0,
        })

    return {
        "series": series,
    }
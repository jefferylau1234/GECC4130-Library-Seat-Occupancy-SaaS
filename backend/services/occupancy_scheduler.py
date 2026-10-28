import logging
from datetime import datetime
from zoneinfo import ZoneInfo

from sqlalchemy import select
from sqlalchemy.dialects.postgresql import insert
from sqlalchemy.orm import Session

from backend.db.db import engine, OccupancyReading, OccupancyRecord
from datetime import datetime, time
import random

logger = logging.getLogger(__name__)
HKT = ZoneInfo("Asia/Hong_Kong")


def increment_occupancy():
    now = datetime.now(HKT)

    w = now.weekday()
    t = now.time()

    if w in (0, 1, 2, 3, 4) and not (time(8, 20) < t < time(22, 0)):
        return
    elif w == 5 and not (time(8, 20) < t < time(19, 0)):
        return
    elif w == 6 and not (time(11, 0) < t < time(19, 0)):
        return

    PLUS = 1

    if now.time()>= time(20, 0):
        PLUS = 0
    elif now.time()>= time(15, 0):
        PLUS = random.choices([1, 0], weights=[0.3, 0.7], k=1)[0]
    elif (time(13, 0) < now.time() < time(15, 0)):
        PLUS = random.choices([1, 0], weights=[0.9, 0.1], k=1)[0]
    

    hour_str = now.strftime("%Y-%m-%d_%H")
    day_start = now.strftime("%Y-%m-%d_00")
    formatted_time = now.strftime("%Y-%m-%d_%H:%M:%S")

    try:
        with Session(engine) as db:
            with db.begin():
                previous_count = db.execute(
                    select(OccupancyReading.occupant_count)
                    .where(
                        OccupancyReading.hour_str >= day_start,
                        OccupancyReading.hour_str < hour_str,
                    )
                    .order_by(OccupancyReading.hour_str.desc())
                    .limit(1)
                ).scalar_one_or_none()

                starting_count = (
                    0 if previous_count is None else previous_count
                )

                stmt = insert(OccupancyReading).values(
                    hour_str=hour_str,
                    occupant_count=starting_count + PLUS,
                    updated_at=now,
                )

                stmt = stmt.on_conflict_do_update(
                    index_elements=[OccupancyReading.hour_str],
                    set_={
                        "occupant_count": (
                            OccupancyReading.occupant_count + PLUS
                        ),
                        "updated_at": now,
                    },
                ).returning(OccupancyReading.occupant_count)

                current_occupancy = db.execute(stmt).scalar_one()

                db.execute(
                    insert(OccupancyRecord).values(
                        recorded_at=formatted_time,
                        occupant_change=f"+{PLUS} system",
                        occupant_count=current_occupancy,
                    )
                )

        logger.info(
            "Scheduled occupancy +1: hour=%s, count=%s",
            hour_str,
            current_occupancy,
        )

    except Exception:
        logger.exception("Scheduled occupancy increment failed")
        raise
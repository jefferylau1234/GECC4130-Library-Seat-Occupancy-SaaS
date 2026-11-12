from datetime import datetime
from pydantic import BaseModel


class OccupancyResponse(BaseModel):
    library_name: str
    current_occupancy: int
    last_updated: datetime
    message: str


class present(BaseModel):
    o: int


class OccupancyPoint(BaseModel):
    time: str
    occupancy: int


class TodayOccupancyResponse(BaseModel):
    series: list[OccupancyPoint]

class LastWeekOccupancyResponse(BaseModel):
    series: list[OccupancyPoint]

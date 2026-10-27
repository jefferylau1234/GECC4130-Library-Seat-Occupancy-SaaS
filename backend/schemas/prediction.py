from datetime import date, datetime
from typing import Literal

from pydantic import BaseModel, Field


# Existing schema — keep for the old /next-hour endpoint.
class PredictedOccupancyResponse(BaseModel):
    library_name: str
    next_period: str
    predicted_occupancy: int
    predicted_peak: str
    last_updated: datetime


# New forecast schemas.
class ForecastSeriesPoint(BaseModel):
    time: str
    occupancy: int = Field(ge=0)
    is_open: bool


class ForecastPeak(BaseModel):
    time: str
    occupancy: int = Field(ge=0)


class ForecastHistory(BaseModel):
    rows_fetched: int
    rows_used: int
    days_used: int
    first_hour: str
    last_hour: str
    training_rows: int


class ForecastResponse(BaseModel):
    date: date
    timezone: str
    generated_at: datetime
    forecast_cutoff: datetime

    model: Literal["lightgbm", "baseline"]
    source: Literal["database", "csv"]

    target_definition: str
    history: ForecastHistory
    capacity_limit: int | None

    series: list[ForecastSeriesPoint]
    peak: ForecastPeak | None

    warnings: list[str]
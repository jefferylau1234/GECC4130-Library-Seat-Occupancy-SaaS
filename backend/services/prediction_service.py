import json
from datetime import datetime, timedelta
from pathlib import Path
from typing import Literal
from zoneinfo import ZoneInfo

from backend.schemas.prediction import ForecastResponse


HKT = ZoneInfo("Asia/Hong_Kong")

FORECAST_DIR = (
    Path(__file__).resolve().parents[1]
    / "data"
    / "forecasts"
)

FORECAST_FILES = {
    "today": "forecast_today.json",
    "tomorrow": "forecast_tomorrow.json",
}


class ForecastUnavailableError(Exception):
    pass


def get_forecast_series(
    day: Literal["today", "tomorrow"] = "today",
) -> ForecastResponse:
    if day not in FORECAST_FILES:
        raise ValueError("day must be today or tomorrow")

    path = FORECAST_DIR / FORECAST_FILES[day]

    try:
        raw_json = path.read_text(encoding="utf-8")

    except FileNotFoundError:
        raise ForecastUnavailableError(
            f"{day.capitalize()} forecast has not been generated yet"
        ) from None

    except OSError:
        raise ForecastUnavailableError(
            f"{day.capitalize()} forecast file cannot be read"
        ) from None

    try:
        forecast = ForecastResponse(**json.loads(raw_json))

    except (ValueError, TypeError):
        raise ForecastUnavailableError(
            f"{day.capitalize()} forecast file is invalid"
        ) from None

    today = datetime.now(HKT).date()

    expected_date = (
        today
        if day == "today"
        else today + timedelta(days=1)
    )

    if forecast.date != expected_date:
        raise ForecastUnavailableError(
            f"{day.capitalize()} forecast is out of date"
        )

    return forecast
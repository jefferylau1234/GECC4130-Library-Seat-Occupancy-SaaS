import json
import logging
from datetime import datetime
from pathlib import Path


logger = logging.getLogger(__name__)

FORECAST_PATH = (
    Path(__file__).resolve().parents[1]
    / "data"
    / "forecasts"
    / "forecast_today.json"
)


def compare_with_forecast(
    current_occupancy: int,
    now: datetime,
) -> dict:
    current_date = now.strftime("%Y-%m-%d")
    current_hour = now.strftime("%H:00")

    result = {
        "available": False,
        "forecast_date": None,
        "time": current_hour,
        "actual_occupancy": current_occupancy,
        "predicted_occupancy": None,
        "difference": None,
        "difference_percent": None,
        "status": "unavailable",
        "reason": None,
    }

    try:
        # Read the latest file contents on every call.
        with FORECAST_PATH.open("r", encoding="utf-8") as file:
            data = json.load(file)

        result["forecast_date"] = data["date"]

        if data["date"] != current_date:
            result["reason"] = (
                f"Forecast date is {data['date']}, "
                f"but the current date is {current_date}"
            )
            return result

        if data["timezone"] != "Asia/Hong_Kong":
            result["reason"] = "Forecast timezone is not Asia/Hong_Kong"
            return result

        series = data["series"]

        if not isinstance(series, list):
            raise ValueError("'series' must be a list")

        prediction = next(
            (
                row
                for row in series
                if row["time"] == current_hour
            ),
            None,
        )

        if prediction is None:
            result["reason"] = (
                f"No prediction found for {current_hour}"
            )
            return result

        predicted_occupancy = prediction["occupancy"]

        # Your JSON stores predictions as non-negative integers.
        if (
            type(predicted_occupancy) is not int
            or predicted_occupancy < 0
        ):
            raise ValueError(
                "Predicted occupancy must be a non-negative integer"
            )

        difference = current_occupancy - predicted_occupancy

        difference_percent = (
            round(difference / predicted_occupancy * 100, 2)
            if predicted_occupancy > 0
            else None
        )

        if difference > 0:
            status = "above_prediction"
        elif difference < 0:
            status = "below_prediction"
        else:
            status = "matches_prediction"

        result.update(
            {
                "available": True,
                "predicted_occupancy": predicted_occupancy,
                "difference": difference,
                "difference_percent": difference_percent,
                "status": status,
            }
        )

        return result

    except FileNotFoundError:
        logger.warning("Forecast file not found: %s", FORECAST_PATH)
        result["reason"] = "Forecast file not found"

    except (OSError, ValueError, KeyError, TypeError):
        logger.exception("Failed to read or validate occupancy forecast")
        result["reason"] = "Forecast file could not be read or validated"

    return result
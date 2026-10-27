from typing import Literal

from fastapi import APIRouter, HTTPException, Response

from backend.schemas.prediction import ForecastResponse
from backend.services.prediction_service import (
    ForecastUnavailableError,
    get_forecast_series,
)


router = APIRouter(
    prefix="/prediction",
    tags=["prediction"],
)


@router.get(
    "/forecast",
    response_model=ForecastResponse,
)
def get_forecast(
    response: Response,
    day: Literal["today", "tomorrow"] = "today",
):
    response.headers["Cache-Control"] = "no-store"

    try:
        return get_forecast_series(day)

    except ForecastUnavailableError as error:
        raise HTTPException(
            status_code=503,
            detail=str(error),
        ) from None
from datetime import datetime, timezone
from backend.data_structure import OccupancyStore, ForecastPoint
from backend.model.forecast import forecast_today_response

# services determine the response data logic


def get_predicted_occupancy_data():

    return {
        "library_name": "CUHK CC Library",
        "next_period": "4pm",
        "predicted_occupancy": 0,
        "predicted_peak": "",
        "last_updated": datetime.now(timezone.utc)
    }



# def get_predicted_occupancy_data(db: Session):
#     historical_rows = db.query(OccupancyReading).all()

#     forecast_response = forecast_today_response(
#         historical_rows,
#         lookback_weeks=8,
#         max_capacity=500,
#     )

#     return forecast_response
# backend/routers/occupancy.py
from fastapi import APIRouter
router = APIRouter(prefix="/occupancy", tags=["occupancy"])

from backend.schemas.occupancy import OccupancyResponse, TodayOccupancyResponse, LastWeekOccupancyResponse, present
from backend.services.occupancy_service import get_current_occupancy_data, get_today_occupancy_data, get_last_week_occupancy_data, get_present_occupancy_data, get_button_occupancy_data

from ..db.db import get_db, OccupancyReading, Presentation
from fastapi import APIRouter, Depends
from sqlalchemy.orm import Session

@router.get("/current", response_model = OccupancyResponse)
def get_current_occupancy(db: Session = Depends(get_db)):
    return get_current_occupancy_data(db, OccupancyReading)



@router.get("/present", response_model = present)
def get_current_occupancy(db: Session = Depends(get_db)):
    return get_present_occupancy_data(db, Presentation)


@router.get("/button", response_model = present)
def get_current_occupancy(db: Session = Depends(get_db)):
    return get_button_occupancy_data(db, Presentation)




@router.get("/today", response_model = TodayOccupancyResponse)
def get_today_occupancy(db: Session = Depends(get_db)):
    return get_today_occupancy_data(db, OccupancyReading)



@router.get("/last-week", response_model = LastWeekOccupancyResponse)
def get_last_week_occupancy(db: Session = Depends(get_db)):
    return get_last_week_occupancy_data(db, OccupancyReading)
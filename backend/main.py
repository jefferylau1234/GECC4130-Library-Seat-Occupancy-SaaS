from contextlib import asynccontextmanager

from apscheduler.schedulers.background import BackgroundScheduler
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from backend.routers.occupancy import router as occupancy_router
from backend.routers.prediction import router as prediction_router
from backend.routers.api import router as api_router
from backend.routers.environment import router as env_router
from backend.routers.check import router as check_router

from backend.db.db import Base, engine
from backend.services.occupancy_scheduler import increment_occupancy


# 必須先定義，再傳入 FastAPI。
@asynccontextmanager
async def lifespan(app: FastAPI):
    Base.metadata.create_all(engine)

    scheduler = BackgroundScheduler(
        timezone="Asia/Hong_Kong",
    )

    scheduler.add_job(
        increment_occupancy,
        trigger="interval",
        minutes=1,
        id="occupancy_plus_one",
        replace_existing=True,
        max_instances=1,
        coalesce=True,
    )

    scheduler.start()
    print("[Scheduler] Started: occupancy +1 every minute", flush=True)

    try:
        yield
    finally:
        scheduler.shutdown(wait=True)
        print("[Scheduler] Stopped", flush=True)


app = FastAPI(
    title="CC Library Occupancy API",
    version="0.1.0",
    lifespan=lifespan,
)


# 保留你現有的 CORS 設定。
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


app.include_router(occupancy_router)
app.include_router(prediction_router)
app.include_router(api_router)
app.include_router(env_router)
app.include_router(check_router)


@app.get("/health")
def health():
    return {"status": "ok"}


# 放在 API routes 後面。
app.frontend(
    "/",
    directory="frontend",
    fallback="index.html",
)



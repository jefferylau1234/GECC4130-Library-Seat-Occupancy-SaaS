# main App entry using FastAPI:
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from backend.routers.occupancy import router as occupancy_router
from backend.routers.prediction import router as prediction_router
from backend.routers.api import router as api_router
from backend.routers.environment import router as env_router
from backend.routers.check import router as check_router
from backend.db.db import Base, engine

app = FastAPI(title="CC Library Occupancy API", version="0.1.0")

# white list for frontend URL to fetch our backend api to send api request
origins = [
    "http://localhost:3000",
    "http://127.0.0.1:3000",
    "http://localhost:5173",
    "http://127.0.0.1:5173",
    "http://127.0.0.1:5500",   # live server port (frontend and backend use different port)
    "http://192.168.0.102",
    ""   
]

# check layer white list
app.add_middleware(
    CORSMiddleware,
    allow_origins = ["*"],
    allow_credentials = True,
    allow_methods = ["*"],
    allow_headers = ["*"],
)

app.include_router(occupancy_router)
app.include_router(prediction_router)
app.include_router(api_router)
app.include_router(env_router)
app.include_router(check_router)

Base.metadata.create_all(engine)


app.frontend("/", directory="frontend", fallback="index.html")

@app.get("/health")
def health():
    return {"status": "ok"}






















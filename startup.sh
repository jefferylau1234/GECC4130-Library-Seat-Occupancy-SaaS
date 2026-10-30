#!/usr/bin/env bash
set -euo pipefail

# Azure can unpack the deployed application into APP_PATH.
cd "${APP_PATH:-/home/site/wwwroot}"

echo "[startup] Generating forecast files..."
python -m backend.jobs.forecast_cron --once

echo "[startup] Forecast generation complete. Starting FastAPI..."

exec python -m uvicorn backend.main:app \
    --host 0.0.0.0 \
    --port 8000
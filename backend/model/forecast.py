"""Single-file CC Library occupancy forecasting (Python 3.10+).

Place at backend/services/forecast.py in your existing project.
Install: pip install pandas numpy lightgbm
Usage:
    from backend.services.forecast import forecast_today
    result = forecast_today()

Uses the existing backend.db.db.SessionLocal and OccupancyReading.
First call trains if no model exists; later calls reuse the saved model.
Forecast target is occupant_count, NOT the notebook's entry_count.
Returns today's 24 HKT hourly points plus the predicted peak.
No router, schema, frontend changes, or database writes are included.
"""
from __future__ import annotations

from datetime import datetime, time
import json
import os
from pathlib import Path
import tempfile
from zoneinfo import ZoneInfo
import zipfile

import numpy as np
import pandas as pd

HKT = ZoneInfo("Asia/Hong_Kong")
MODEL_PATH = Path(__file__).resolve().parent / "occupancy_model.zip"
VERSION = "cc-hourly-occupancy-single-v1"


class ForecastError(RuntimeError):
    pass


def _opening_fraction(index, overrides):
    values = []
    for t in index:
        day = t.strftime("%Y-%m-%d")
        hours = overrides.get(day, ["08:20", "22:00"] if t.weekday() < 5
                              else ["08:20", "19:00"] if t.weekday() == 5
                              else ["11:00", "19:00"])
        if hours is None:
            values.append(0.0)
            continue
        start, end = time.fromisoformat(hours[0]), time.fromisoformat(hours[1])
        if start >= end:
            raise ValueError("Opening overrides must start before closing (no overnight hours)")
        opening = t.normalize() + pd.Timedelta(hours=start.hour, minutes=start.minute)
        closing = t.normalize() + pd.Timedelta(hours=end.hour, minutes=end.minute)
        overlap = (min(t + pd.Timedelta(hours=1), closing) - max(t, opening)).total_seconds()
        values.append(max(0.0, overlap) / 3600)
    return np.asarray(values)


def _acf(values, lag):
    z = values - values.mean()
    denominator = float(np.dot(z, z))
    if denominator <= 1e-12:
        return 1.0 if lag == 0 else 0.0
    return 1.0 if lag == 0 else float(np.dot(z[:-lag], z[lag:]) / denominator)


def _features(y, calendar, overrides):
    x = pd.DataFrame(index=y.index)
    for lag in (1, 24, 168):
        x[f"lag_{lag}"] = y.shift(lag)
    past = y.shift(1)
    x["trend_6"] = past.rolling(6, min_periods=6).mean()
    hour = y.index.hour
    weekly_hour = y.index.dayofweek * 24 + hour
    for k in range(1, 6):
        x[f"daily_sin_{k}"] = np.sin(2*np.pi*k*hour/24)
        x[f"daily_cos_{k}"] = np.cos(2*np.pi*k*hour/24)
        x[f"weekly_sin_{k}"] = np.sin(2*np.pi*k*weekly_hour/168)
        x[f"weekly_cos_{k}"] = np.cos(2*np.pi*k*weekly_hour/168)
    flags = np.zeros(len(y), dtype=np.int8)
    dates = y.index.strftime("%Y-%m-%d")
    for academic_year in calendar.values():
        for key, flag in (("sem1", 1), ("sem2", 2), ("summer", 3)):
            if key in academic_year:
                span = academic_year[key]
                flags[(dates >= span["start"]) & (dates <= span["end"])] = flag
    x["semester_flag"] = flags
    x["weekday"] = y.index.dayofweek
    x["opening_fraction"] = _opening_fraction(y.index, overrides)
    x["available_last_168"] = past.rolling(168, min_periods=1).count()
    for lag in range(11):
        x[f"acf_lag_{lag}"] = past.rolling(168, min_periods=168).apply(
            lambda values, n=lag: _acf(values, n), raw=True)
    return x.astype(float)


def _fetch(db, model, day, history_days, overrides):
    start = day - pd.Timedelta(days=history_days)
    rows = (db.query(model.hour_str, model.occupant_count)
            .filter(model.hour_str >= start.strftime("%Y-%m-%d_00"),
                    model.hour_str < day.strftime("%Y-%m-%d_00"))
            .order_by(model.hour_str.asc()).all())
    if not rows:
        raise ForecastError("No occupancy history before today's Hong Kong midnight")
    raw = pd.DataFrame([(r[0], r[1]) for r in rows], columns=["hour_str", "count"])
    stamps = pd.to_datetime(raw.hour_str, format="%Y-%m-%d_%H", errors="coerce")
    counts = pd.to_numeric(raw["count"], errors="coerce")
    if stamps.isna().any() or counts.isna().any() or (counts < 0).any() or not np.isfinite(counts).all():
        raise ForecastError("Invalid hour_str or occupant_count in occupancy_readings")
    index = pd.DatetimeIndex(stamps).tz_localize(HKT)
    if index.duplicated().any():
        raise ForecastError("Duplicate hourly timestamps")
    observed = pd.Series(counts.to_numpy(dtype=float), index=index).sort_index()
    observed = observed.loc[(observed.index >= start) & (observed.index < day)]
    if observed.empty:
        raise ForecastError("No usable history in the requested window")
    if (day - observed.index[-1]).total_seconds() > 7*24*3600:
        raise ForecastError("Most recent historical reading is over seven days old")
    index = pd.date_range(observed.index[0].normalize(), day - pd.Timedelta(hours=1), freq="1h")
    y = observed.reindex(index)
    actual = y.notna()
    open_hours = _opening_fraction(index, overrides) > 0
    if ((~open_hours) & actual & (y.fillna(0) > 0)).any():
        raise ForecastError("Positive occupancy during scheduled closed hours; correct data or opening_overrides")
    # Unknown open-hour readings remain missing; only scheduled closed hours become zero.
    y.loc[~open_hours] = 0.0
    return y, actual, int(observed.index.normalize().nunique()), int((open_hours & ~actual).sum()), observed.index[-1]


def _lightgbm():
    try:
        import lightgbm as lgb
        return lgb
    except ImportError as exc:
        raise ForecastError("Install lightgbm: pip install lightgbm") from exc


def _save(path, booster, metadata):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    os.close(fd)
    try:
        with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("model.txt", booster.model_to_string())
            archive.writestr("metadata.json", json.dumps(metadata))
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def forecast_today(db=None, *, model_path=None, history_days=180,
                   retrain=False, calendar=None, opening_overrides=None,
                   capacity=None, now=None):
    """Fetch history, train/load LightGBM, return today's forecast series.

    db: optional existing SQLAlchemy Session; caller retains ownership.
    model_path: optional saved-model location. Defaults beside this script.
    retrain: explicitly replace an existing model. Missing models train automatically.
    calendar: {academic_year: {sem1/sem2/summer: {start: YYYY-MM-DD, end: YYYY-MM-DD}}}.
    opening_overrides: {YYYY-MM-DD: None (closed) or [HH:MM, HH:MM]}.
    capacity: optional confirmed upper limit; no arbitrary cap is assumed.
    now: optional timezone-aware datetime for tests. Default is current HKT time.

    Uses only history before today's 00:00 HKT, including when called in afternoon.
    Requires 28 observed days and 168 observed open-hour training targets.
    Missing open-hour values stay NaN (LightGBM handles missing features).
    Point estimates only: no prediction intervals or accuracy guarantee.
    """
    calendar = calendar or {}
    overrides = opening_overrides or {}
    if history_days < 28:
        raise ValueError("history_days must be at least 28")
    if capacity is not None and (not np.isfinite(capacity) or capacity <= 0):
        raise ValueError("capacity must be a finite positive number")
    for year in calendar.values():
        for key, span in year.items():
            if key not in {"sem1", "sem2", "summer"}:
                raise ValueError("Unknown semester key")
            if pd.Timestamp(span["start"]) > pd.Timestamp(span["end"]):
                raise ValueError("Invalid semester interval")
    for date, hours in overrides.items():
        pd.Timestamp(date)
        if hours is not None and len(hours) != 2:
            raise ValueError("Opening override needs [opening, closing] or None")
    stamp = pd.Timestamp(now if now is not None else datetime.now(HKT))
    if stamp.tzinfo is None:
        raise ValueError("now must have a timezone")
    stamp = stamp.tz_convert(HKT)
    day = stamp.normalize()
    path = Path(model_path) if model_path is not None else MODEL_PATH
    if db is None:
        from backend.db.db import SessionLocal
        with SessionLocal() as session:
            return forecast_today(session, model_path=path, history_days=history_days,
                retrain=retrain, calendar=calendar, opening_overrides=overrides,
                capacity=capacity, now=stamp)
    from backend.db.db import OccupancyReading
    history, actual, observed_days, missing, last_observed = _fetch(
        db, OccupancyReading, day, history_days, overrides)
    signature = {"version": VERSION, "calendar": calendar,
                 "opening_overrides": overrides, "capacity": capacity}
    lgb = _lightgbm()
    if retrain or not path.exists():
        if observed_days < 28:
            raise ForecastError(f"Need 28 observed days to train; found {observed_days}")
        features = _features(history, calendar, overrides)
        valid = actual & (_opening_fraction(history.index, overrides) > 0) & (np.arange(len(history)) >= 168)
        x, y = features.loc[valid], history.loc[valid]
        if len(y) < 168 or y.sum() <= 0:
            raise ForecastError("Need 168 valid open-hour targets after weekly warmup, including positive counts")
        booster = lgb.train({
            "objective": "poisson", "metric": ["rmse", "l1"],
            "num_leaves": 31, "min_data_in_leaf": 20, "learning_rate": 0.03,
            "feature_fraction": 0.8, "bagging_fraction": 0.8, "bagging_freq": 1,
            "verbosity": -1, "seed": 42, "num_threads": 2,
        }, lgb.Dataset(x, label=y), num_boost_round=500)
        metadata = {"signature": signature, "features": features.columns.tolist(),
                    "trained_until": history.index[-1].isoformat(),
                    "trained_at": stamp.isoformat(), "training_rows": len(y)}
        _save(path, booster, metadata)
    else:
        try:
            with zipfile.ZipFile(path) as archive:
                metadata = json.loads(archive.read("metadata.json"))
                model_text = archive.read("model.txt").decode()
        except (OSError, zipfile.BadZipFile, KeyError, ValueError) as exc:
            raise ForecastError("Invalid model artifact; retrain explicitly") from exc
        if metadata["signature"] != signature:
            raise ForecastError("Forecast settings changed; call with retrain=True")
        if pd.Timestamp(metadata["trained_until"]) >= day:
            raise ForecastError("Model trained on or after the forecast day; refusing future-data leakage")
        booster = lgb.Booster(model_str=model_text)
    if booster.feature_name() != metadata["features"]:
        raise ForecastError("Model features differ from metadata; retrain")
    current = history.tail(168).copy()
    targets = pd.date_range(day, periods=24, freq="1h")
    fractions = _opening_fraction(targets, overrides)
    points = []
    for target, fraction in zip(targets, fractions):
        prediction = 0.0
        if fraction > 0:
            current.loc[target] = np.nan
            x = _features(current, calendar, overrides).iloc[[-1]]
            if x.columns.tolist() != metadata["features"]:
                raise ForecastError("Feature pipeline changed; retrain")
            prediction = float(booster.predict(x, num_threads=2)[0])
            if not np.isfinite(prediction):
                raise ForecastError("Model returned a nonfinite prediction")
            prediction = float(np.clip(prediction, 0, capacity if capacity is not None else np.inf))
        current.loc[target] = prediction
        current = current.tail(168)
        points.append({"time": target.strftime("%H:%M"), "occupancy": round(prediction, 2),
                       "timestamp": target.isoformat(),
                       "kind": "forecast" if fraction > 0 else "scheduled_closed"})
    open_points = [p for p in points if p["kind"] == "forecast"]
    peak_point = max(open_points, key=lambda p: p["occupancy"]) if open_points else None
    peak = {k: peak_point[k] for k in ("time", "occupancy", "timestamp")} if peak_point else None
    warnings = ["Opening hours follow the supplied schedule, not a live library calendar.",
                "Model accuracy has not been validated by this script; evaluate on held-out days before production."]
    if not calendar:
        warnings.append("No confirmed semester calendar supplied; semester_flag=0.")
    if not overrides:
        warnings.append("No holiday or special-opening overrides supplied.")
    if missing:
        warnings.append(f"{missing} missing historical open-hour readings retained as NaN.")
    if (day - pd.Timestamp(metadata["trained_until"])).total_seconds() > 7*24*3600:
        warnings.append("Saved model is more than seven days old; consider retrain=True.")
    return {"date": day.strftime("%Y-%m-%d"), "timezone": "Asia/Hong_Kong",
            "frequency_minutes": 60, "target": "occupant_count", "forecast_mode": "day_ahead",
            "forecast_origin": day.isoformat(), "generated_at": stamp.isoformat(),
            "last_observed_at": last_observed.isoformat(),
            "model_trained_until": metadata["trained_until"],
            "series": points, "peak": peak, "warnings": warnings}


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retrain", action="store_true")
    parser.add_argument("--model-path")
    args = parser.parse_args()
    print(json.dumps(forecast_today(retrain=args.retrain, model_path=args.model_path),
                     ensure_ascii=False, indent=2))

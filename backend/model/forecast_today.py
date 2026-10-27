"""Standalone CC Library daily occupancy forecast. No application imports or DB writes."""
from __future__ import annotations

import argparse
import json
import os
import sys
from datetime import datetime, time, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

import numpy as np
import pandas as pd

HKT = ZoneInfo("Asia/Hong_Kong")


def opening_hours(day, overrides):
    key = day.strftime("%Y-%m-%d")
    if key in overrides:
        hours = overrides[key]
        if not isinstance(hours, list) or any(type(h) is not int or not 0 <= h <= 23 for h in hours):
            raise ValueError("Schedule overrides must map dates to lists of integer hours 0..23.")
        return sorted(set(hours))
    if day.weekday() < 5:
        return list(range(8, 22))
    if day.weekday() == 5:
        return list(range(8, 19))
    return list(range(11, 19))


def read_history(target, lookback_days, csv_path=None):
    cutoff = datetime.combine(target, time.min, tzinfo=HKT)
    start = target - timedelta(days=lookback_days)
    if csv_path:
        raw = pd.read_csv(csv_path)
    else:
        try:
            from dotenv import load_dotenv
            from sqlalchemy import create_engine, text
            from sqlalchemy.engine import make_url
        except ImportError:
            raise RuntimeError("Install sqlalchemy, psycopg[binary], and python-dotenv for database mode.") from None
        load_dotenv()
        url_text = os.getenv("DATABASE_URL")
        if not url_text:
            raise RuntimeError("DATABASE_URL is not set. Add it to your local .env file.")
        url = make_url(url_text)
        if url.drivername in ("postgres", "postgresql"):
            url = url.set(drivername="postgresql+psycopg")
        if url.drivername != "postgresql+psycopg":
            raise ValueError("Use a postgresql:// or postgresql+psycopg:// DATABASE_URL.")
        engine = None
        try:
            engine = create_engine(url, pool_pre_ping=True, connect_args={"connect_timeout": 15})
            with engine.connect() as conn:
                with conn.begin():
                    conn.execute(text("SET TRANSACTION READ ONLY"))
                    conn.execute(text("SET LOCAL statement_timeout = '30s'"))
                    raw = pd.read_sql_query(text("""
                        SELECT hour_str, occupant_count, updated_at
                        FROM public.occupancy_readings
                        WHERE hour_str >= :start_key AND hour_str < :end_key
                          AND updated_at < :cutoff
                        ORDER BY hour_str
                    """), conn, params={
                        "start_key": f"{start.isoformat()}_00",
                        "end_key": f"{target.isoformat()}_00",
                        "cutoff": cutoff,
                    })
        except Exception:
            raise RuntimeError("Database read failed. Check your local DATABASE_URL, network, permissions, and table schema; connection details are intentionally hidden.") from None
        finally:
            if engine is not None:
                engine.dispose()
    required = {"hour_str", "occupant_count", "updated_at"}
    if not required.issubset(raw.columns):
        raise ValueError(f"Input must include columns: {sorted(required)}")
    raw["timestamp"] = pd.to_datetime(raw["hour_str"], format="%Y-%m-%d_%H", errors="raise")
    raw["updated_at"] = pd.to_datetime(raw["updated_at"], utc=True, format="mixed", errors="raise")
    raw["occupant_count"] = pd.to_numeric(raw["occupant_count"], errors="raise")
    raw = raw.loc[(raw.timestamp >= pd.Timestamp(start)) &
                  (raw.timestamp < pd.Timestamp(target)) &
                  (raw.updated_at < pd.Timestamp(cutoff))].copy()
    if raw.hour_str.duplicated().any():
        raise ValueError("Duplicate hour_str values found.")
    if raw.empty:
        raise ValueError("No historical data exists before the target day's midnight cutoff.")
    return raw


def prepare_history(raw, overrides, exclude_dates, min_day_records, capacity):
    data = raw.copy()
    warnings = []
    counts = data.occupant_count
    invalid = ~np.isfinite(counts) | (counts < 0)
    if invalid.any():
        warnings.append(f"Excluded {int(invalid.sum())} negative/non-finite counts.")
        data = data.loc[~invalid].copy()
    data["date"] = data.timestamp.dt.date
    data["hour"] = data.timestamp.dt.hour
    data = data.loc[~data.date.astype(str).isin(exclude_dates)].copy()
    mask = [h in opening_hours(d, overrides) for d, h in zip(data.date, data.hour)]
    ignored = len(data) - sum(mask)
    data = data.loc[mask].copy()
    if ignored:
        warnings.append(f"Excluded {ignored} observations outside the configured opening-hour buckets.")
    sizes = data.groupby("date").size()
    sparse = sizes[sizes < min_day_records].index
    data = data.loc[~data.date.isin(sparse)].copy()
    if len(sparse):
        warnings.append(f"Excluded {len(sparse)} dates with fewer than {min_day_records} open-hour observations.")
    if data.empty:
        raise ValueError("No usable history remains after filtering.")
    daily_max = data.groupby("date").occupant_count.max()
    zeros = [str(d) for d in daily_max[daily_max == 0].index]
    if zeros:
        warnings.append("All-zero dates retained (not automatically treated as faults): " + ", ".join(zeros))
    if capacity is not None:
        over = int((data.occupant_count > capacity).sum())
        if over:
            warnings.append(f"{over} training counts exceed capacity {capacity}; retained for review, not silently clipped.")
    if data.occupant_count.max() > 500:
        warnings.append("History includes counts above 500. Verify whether these are valid occupancy readings.")
    return data.sort_values("timestamp"), warnings


def features(day, hour, history):
    # Each training day uses only earlier dates, matching a midnight daily forecast.
    past = history.loc[history.date < day]
    same_hour = past.loc[past.hour == hour]
    recent = same_hour.loc[same_hour.date >= day - timedelta(days=28)]
    same_weekday = recent.loc[recent.timestamp.dt.weekday == day.weekday()]
    week = past.loc[past.date >= day - timedelta(days=7)]
    def mean(frame):
        return float(frame.occupant_count.mean()) if len(frame) else np.nan
    row = {
        "hour": hour,
        "weekday": day.weekday(),
        "weekend": int(day.weekday() >= 5),
        "hour_sin": np.sin(2 * np.pi * hour / 24),
        "hour_cos": np.cos(2 * np.pi * hour / 24),
        "same_hour_mean_28d": mean(recent),
        "same_weekday_hour_mean_28d": mean(same_weekday),
        "recent_mean_7d": mean(week),
        "same_hour_samples_28d": len(recent),
    }
    for lag in (1, 7, 14):
        matches = same_hour.loc[same_hour.date == day - timedelta(days=lag)]
        row[f"same_hour_lag_{lag}d"] = mean(matches)
    return row


def baseline_value(day, hour, history):
    past = history.loc[(history.date < day) & (history.date >= day - timedelta(days=28))]
    same_hour = past.loc[past.hour == hour]
    same_weekday = same_hour.loc[same_hour.timestamp.dt.weekday == day.weekday()]
    for frame in (same_weekday, same_hour, past):
        if len(frame):
            return float(frame.occupant_count.median())
    raise ValueError("Insufficient recent history for a baseline forecast.")


def forecast_today(*, target_date=None, csv_path=None, model="lightgbm",
                   lookback_days=56, min_day_records=6, capacity=None,
                   exclude_dates=(), opening_overrides=None):
    target = pd.Timestamp(target_date).date() if target_date else datetime.now(HKT).date()
    overrides = opening_overrides or {}
    if capacity is not None and capacity <= 0:
        raise ValueError("capacity must be positive.")
    if lookback_days < 21 or min_day_records < 1:
        raise ValueError("Use lookback_days >= 21 and min_day_records >= 1.")
    raw = read_history(target, lookback_days, csv_path)
    history, warnings = prepare_history(raw, overrides, set(exclude_dates), min_day_records, capacity)
    if history.date.nunique() < 14:
        raise ValueError("At least 14 usable historical dates are required by this prototype.")
    latest_day = max(history.date)
    if (target - latest_day).days > 2:
        warnings.append(f"Latest usable history is {latest_day}; the data is stale.")
    hours = opening_hours(target, overrides)
    n_training = 0
    predictions = np.array([], dtype=float)
    if hours and model == "lightgbm":
        try:
            from lightgbm import LGBMRegressor
        except ImportError:
            raise RuntimeError("LightGBM is not installed. Install lightgbm or explicitly select --model baseline.") from None
        first = min(history.date)
        samples = history.loc[history.date >= first + timedelta(days=7)]
        if len(samples) < 80:
            raise ValueError("Fewer than 80 training rows after the 7-day feature warm-up.")
        X = pd.DataFrame([features(r.date, int(r.hour), history) for r in samples.itertuples()])
        y = samples.occupant_count.to_numpy(dtype=float)
        if not (y > 0).any():
            raise ValueError("Training targets are all zero; inspect the source data.")
        predictor = LGBMRegressor(
            objective="poisson", n_estimators=200, learning_rate=0.03,
            num_leaves=7, max_depth=3, min_child_samples=20,
            reg_lambda=5.0, verbosity=-1, random_state=42, n_jobs=1,
        )
        predictor.fit(X, y)
        future = pd.DataFrame([features(target, h, history) for h in hours], columns=X.columns)
        predictions = predictor.predict(future)
        n_training = len(y)
    elif hours and model == "baseline":
        predictions = np.array([baseline_value(target, h, history) for h in hours])
    elif model not in ("lightgbm", "baseline"):
        raise ValueError("model must be lightgbm or baseline.")
    if not np.isfinite(predictions).all():
        raise ValueError("Model returned invalid predictions.")
    predictions = np.maximum(0, predictions)
    if capacity is not None:
        predictions = np.minimum(predictions, capacity)
    values = dict(zip(hours, np.rint(predictions).astype(int).tolist()))
    series = [{"time": f"{h:02d}:00", "occupancy": values.get(h, 0),
               "is_open": h in values} for h in range(24)]
    peak = None
    if hours:
        peak_hour = max(hours, key=lambda h: values[h])
        peak = {"time": f"{peak_hour:02d}:00", "occupancy": values[peak_hour]}
    warnings.append("Prototype only: no out-of-sample accuracy evaluation has been performed.")
    warnings.append("Default opening hours do not include public holidays; supply schedule overrides.")
    return {
        "date": target.isoformat(), "timezone": "Asia/Hong_Kong",
        "generated_at": datetime.now(HKT).isoformat(),
        "forecast_cutoff": datetime.combine(target, time.min, tzinfo=HKT).isoformat(),
        "model": model, "source": "csv" if csv_path else "database",
        "target_definition": "occupant_count stored in each hour_str bucket; not entry count or assumed hourly average",
        "history": {"rows_fetched": len(raw), "rows_used": len(history),
                    "days_used": int(history.date.nunique()),
                    "first_hour": history.timestamp.min().strftime("%Y-%m-%d_%H"),
                    "last_hour": history.timestamp.max().strftime("%Y-%m-%d_%H"),
                    "training_rows": n_training},
        "capacity_limit": capacity, "series": series, "peak": peak,
        "warnings": warnings,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--date", help="Target date YYYY-MM-DD; defaults to today in Hong Kong")
    parser.add_argument("--csv", help="Offline alternative to the database")
    parser.add_argument("--model", choices=["lightgbm", "baseline"], default="lightgbm")
    parser.add_argument("--lookback-days", type=int, default=56)
    parser.add_argument("--min-day-records", type=int, default=6)
    parser.add_argument("--capacity", type=int, help="Optional verified capacity; caps predictions only")
    parser.add_argument("--exclude-date", action="append", default=[], help="Repeat for confirmed faulty dates")
    parser.add_argument("--schedule", help='JSON file: {"YYYY-MM-DD": [8,9,...]} or [] for closure')
    parser.add_argument(
    "--output",
    default=None,
    help="Optional output path; otherwise save in backend/data/forecasts",
)
    args = parser.parse_args()
    try:
        overrides = json.loads(Path(args.schedule).read_text()) if args.schedule else {}
        result = forecast_today(target_date=args.date, csv_path=args.csv, model=args.model,
            lookback_days=args.lookback_days, min_day_records=args.min_day_records,
            capacity=args.capacity, exclude_dates=args.exclude_date, opening_overrides=overrides)


        
        payload = json.dumps(
            result,
            ensure_ascii=False,
            indent=2,
            allow_nan=False,
        )

        if args.output:
            # Honour an explicitly supplied output path.
            output_path = Path(args.output).expanduser().resolve()

        else:
            # This script is backend/model/forecast_today.py.
            backend_dir = Path(__file__).resolve().parents[1]
            output_dir = backend_dir / "data" / "forecasts"

            today = datetime.now(HKT).date()
            target = datetime.strptime(
                result["date"],
                "%Y-%m-%d",
            ).date()

            if target == today:
                filename = "forecast_today.json"

            elif target == today + timedelta(days=1):
                filename = "forecast_tomorrow.json"

            else:
                filename = f"forecast_{target.isoformat()}.json"

            output_path = output_dir / filename

        output_path.parent.mkdir(
            parents=True,
            exist_ok=True,
        )

        output_path.write_text(
            payload + "\n",
            encoding="utf-8",
        )

        print(payload)
        print(
            f"Saved forecast to: {output_path}",
            file=sys.stderr,
        )



    except Exception as exc:
        print(f"Forecast failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

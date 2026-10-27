import argparse
import json
import logging
import os
import tempfile

from datetime import datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

from dotenv import load_dotenv

from backend.model.forecast_today import forecast_today


HKT = ZoneInfo("Asia/Hong_Kong")

BACKEND_DIR = Path(__file__).resolve().parents[1]
PROJECT_ROOT = BACKEND_DIR.parent

# Optional override for deployment storage.
FORECAST_DIR = Path(
    os.getenv(
        "FORECAST_DIR",
        str(BACKEND_DIR / "data" / "forecasts"),
    )
).expanduser().resolve()

load_dotenv(PROJECT_ROOT / ".env")

logger = logging.getLogger(__name__)


def save_json_atomically(
    destination: Path,
    data: dict,
):
    destination.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    payload = json.dumps(
        data,
        ensure_ascii=False,
        indent=2,
        allow_nan=False,
    )

    temporary_path = None

    try:
        # Create the temporary file alongside the destination.
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=destination.parent,
            prefix=f".{destination.name}.",
            suffix=".tmp",
            delete=False,
        ) as file:
            temporary_path = Path(file.name)

            file.write(payload)
            file.write("\n")

            file.flush()
            os.fsync(file.fileno())

        os.replace(
            temporary_path,
            destination,
        )

    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def update_forecast_files():
    today = datetime.now(HKT).date()
    tomorrow = today + timedelta(days=1)

    logger.info(
        "Generating forecasts: today=%s, tomorrow=%s",
        today,
        tomorrow,
    )

    # Finish both predictions before replacing either file.
    today_result = forecast_today(
        target_date=today.isoformat(),
    )

    tomorrow_result = forecast_today(
        target_date=tomorrow.isoformat(),
    )

    # Do not publish results under the wrong day's filenames
    # if an unusually long run crosses midnight.
    if datetime.now(HKT).date() != today:
        raise RuntimeError(
            "Forecast generation crossed midnight; "
            "outputs were not published. Run the job again."
        )

    save_json_atomically(
        FORECAST_DIR / "forecast_today.json",
        today_result,
    )

    save_json_atomically(
        FORECAST_DIR / "forecast_tomorrow.json",
        tomorrow_result,
    )

    logger.info(
        "Forecast files updated in %s",
        FORECAST_DIR,
    )

    logger.info(
        "Today peak: %s | Tomorrow peak: %s",
        today_result["peak"],
        tomorrow_result["peak"],
    )


def main():
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "--once",
        action="store_true",
        help="Generate both files immediately, then exit",
    )

    args = parser.parse_args()

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    if args.once:
        try:
            update_forecast_files()
        except Exception:
            logger.exception("Forecast generation failed")
            return 1

        return 0

    from apscheduler.schedulers.blocking import BlockingScheduler
    from apscheduler.triggers.cron import CronTrigger

    scheduler = BlockingScheduler(
        timezone=HKT,
    )

    scheduler.add_job(
        update_forecast_files,

        trigger=CronTrigger(
            hour=0,
            minute=0,
            second=0,
            timezone=HKT,
        ),

        id="daily_library_forecast",
        replace_existing=True,

        max_instances=1,
        coalesce=True,
        misfire_grace_time=3600,
    )

    logger.info(
        "Forecast scheduler started. "
        "Schedule: daily 00:00 Asia/Hong_Kong"
    )

    try:
        scheduler.start()
    except (KeyboardInterrupt, SystemExit):
        logger.info("Forecast scheduler stopped")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
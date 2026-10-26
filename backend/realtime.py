# backend/realtime.py

import asyncio
import json

from fastapi import APIRouter, Request
from fastapi.responses import StreamingResponse

router = APIRouter()

# One queue per connected browser.
_subscribers: set[asyncio.Queue] = set()


async def publish_occupancy(
    current_occupancy: int,
    message: str = "",
):
    payload = {
        "current_occupancy": current_occupancy,
        "message": message,
    }

    for queue in tuple(_subscribers):
        # Only keep the newest occupancy snapshot for slow clients.
        if queue.full():
            queue.get_nowait()

        queue.put_nowait(payload)


@router.get("/dashboard/events")
async def dashboard_events(request: Request):
    queue = asyncio.Queue(maxsize=1)
    _subscribers.add(queue)

    async def stream():
        try:
            # Send something immediately to establish the stream.
            yield ": connected\n\n"

            while True:
                if await request.is_disconnected():
                    break

                try:
                    # Wait for an update, rather than querying the DB.
                    payload = await asyncio.wait_for(
                        queue.get(),
                        timeout=15,
                    )
                except asyncio.TimeoutError:
                    # Heartbeat only: no database query or new API request.
                    yield ": keep-alive\n\n"
                    continue

                yield (
                    "event: occupancy\n"
                    f"data: {json.dumps(payload)}\n\n"
                )

        finally:
            _subscribers.discard(queue)

    return StreamingResponse(
        stream(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
        },
    )
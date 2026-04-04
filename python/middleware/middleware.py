# MARK: Middleware — CORS, security headers, body-size limit, timing, rate limiting.
"""FastAPI Middleware — CORS, security headers, body-size limit, timing, rate limiting."""

import os
import time
from collections import defaultdict
from typing import Callable

from fastapi import FastAPI, Request, Response
from fastapi.middleware.cors import CORSMiddleware

ALLOWED_ORIGINS = ["*"]  # TODO: Restrict in production
MAX_BODY_BYTES = 1 * 1024 * 1024  # 1 MB
RATE_LIMIT = int(os.environ.get("RATE_LIMIT", 60))   # requests per window per IP
RATE_WINDOW = int(os.environ.get("RATE_WINDOW", 60))  # window in seconds

# MARK: Simple in-memory rate limiter
_hits: dict[str, list[float]] = defaultdict(list)
_last_cleanup = 0.0


def _rate_check(ip: str) -> bool:
    """Return True if request is allowed, False if rate limited."""
    global _last_cleanup
    now = time.time()
    # Periodic cleanup — every 60 seconds, remove expired entries
    if now - _last_cleanup > 60:
        _last_cleanup = now
        expired = [k for k, v in _hits.items() if not v or v[-1] < now - RATE_WINDOW]
        for k in expired:
            del _hits[k]
    cutoff = now - RATE_WINDOW
    bucket = _hits[ip]
    # Remove old entries
    while bucket and bucket[0] < cutoff:
        bucket.pop(0)
    if len(bucket) >= RATE_LIMIT:
        return False
    bucket.append(now)
    return True


def add_middlewares(app: FastAPI) -> None:
    """Configure CORS, security headers, body-size limit, and timing."""
    app.add_middleware(
        CORSMiddleware,
        allow_origins=ALLOWED_ORIGINS,
        allow_credentials=True,
        allow_methods=["*"],
        allow_headers=["*"],
    )

    # MARK: Security + timing middleware
    @app.middleware("http")
    async def security_middleware(
        request: Request, call_next: Callable
    ) -> Response:
        # Rate limiting
        ip = request.headers.get("x-forwarded-for", "").split(",")[0].strip()
        if not ip and request.client:
            ip = request.client.host
        if ip and not _rate_check(ip):
            return Response("Rate limit exceeded", status_code=429)

        # Body size limit
        cl = request.headers.get("content-length")
        if cl:
            try:
                if int(cl) > MAX_BODY_BYTES:
                    return Response("Request too large", status_code=413)
            except ValueError:
                return Response("Invalid Content-Length", status_code=400)

        start_time = time.perf_counter()
        response = await call_next(request)

        # Timing
        response.headers["X-Process-Time"] = f"{time.perf_counter() - start_time:0.4f}"
        # Security headers
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["X-Frame-Options"] = "DENY"
        response.headers["Referrer-Policy"] = "strict-origin-when-cross-origin"
        return response

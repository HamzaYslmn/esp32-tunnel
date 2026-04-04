# MARK: esp32-tunnel relay server — FastAPI with auto-import
"""
Deploy on Render.com (or any Python host with WebSocket support).
ESP32 connects via WebSocket, visitors access via HTTP.

  WS   /api/tunnel/ws?id=<name>  — ESP32 tunnel
  ANY  /<name>/<path>            — visitor -> ESP32
  GET  /api/status               — health check
"""

import importlib
import os
import pathlib
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse

from middleware import middleware, log

_landing = (pathlib.Path(__file__).parent / "public" / "landing.html").read_text()
PORT = int(os.environ.get("PORT", 8000))


@asynccontextmanager
async def lifespan(app: FastAPI):
    log.info(f"Server: http://0.0.0.0:{PORT}")
    yield


app = FastAPI(title="esp32-tunnel", lifespan=lifespan)
middleware.add_middlewares(app)


# MARK: Auto-discover routers from api/ directory
_MARKER = "from fastapi import APIRouter"


def _include_all_routers(directory: str, prefix: str):
    api_dir = pathlib.Path(__file__).parent / directory
    base = api_dir.name
    for py in sorted(api_dir.rglob("*.py")):
        if py.name.startswith("_"):
            continue
        with open(py, "rb") as f:
            if _MARKER not in f.read(512).decode("utf-8", errors="ignore"):
                continue
        module = base + "." + ".".join(py.relative_to(api_dir).with_suffix("").parts)
        try:
            app.include_router(importlib.import_module(module).router, prefix=prefix)
        except Exception as e:
            log.error(f"Router error {module}: {e}")


_include_all_routers("api", "/api")


# MARK: Landing page
@app.get("/")
async def landing():
    return HTMLResponse(_landing)


# MARK: Catch-all proxy — /{tid} and /{tid}/{path} (after specific routes)
from api.tunnel import tunnel_proxy

_ALL_METHODS = ["GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"]


@app.api_route("/{tid}/{path:path}", methods=_ALL_METHODS)
async def proxy_path(tid: str, path: str, request: Request):
    return await tunnel_proxy(tid, path, request)


@app.api_route("/{tid}", methods=_ALL_METHODS)
async def proxy_root(tid: str, request: Request):
    return await tunnel_proxy(tid, "", request)


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host="0.0.0.0", port=PORT,
                ws_ping_interval=20, ws_ping_timeout=30)

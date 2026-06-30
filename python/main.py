# MARK: esp32-tunnel relay server
"""
Deploy on Render.com (or any Python host with WebSocket support).
ESP32 connects via WebSocket, visitors access via HTTP.

  WS   /api/tunnel/ws?id=<name>  — ESP32 tunnel
  ANY  /<name>/<path>            — visitor -> ESP32
  GET  /api/status               — health check
"""

import os
import pathlib
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.responses import Response
from fastapi.staticfiles import StaticFiles

from api import root, tunnel
from middleware import middleware, log

_frontend = pathlib.Path(__file__).parent / "frontend"
_p2p_js = (_frontend / "p2p.js").read_text(encoding="utf-8")
PORT = int(os.environ.get("PORT", 8000))


@asynccontextmanager
async def lifespan(app: FastAPI):
    log.info(f"Server: http://0.0.0.0:{PORT}")
    yield


app = FastAPI(title="esp32-tunnel", lifespan=lifespan)
middleware.add_middlewares(app)


# MARK: API routes
app.include_router(root.router, prefix="/api")
app.include_router(tunnel.router, prefix="/api")


# MARK: P2P browser client — explicit route (the /{tid} proxy would 400 on the dot)
@app.get("/p2p.js")
async def p2p_js():
    return Response(_p2p_js, media_type="application/javascript")


# MARK: Catch-all proxy — /{tid} and /{tid}/{path} (after specific routes)
_ALL_METHODS = ["GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"]


@app.api_route("/{tid}/{path:path}", methods=_ALL_METHODS)
async def proxy_path(tid: str, path: str, request: Request):
    return await tunnel.tunnel_proxy(tid, path, request)


@app.api_route("/{tid}", methods=_ALL_METHODS)
async def proxy_root(tid: str, request: Request):
    return await tunnel.tunnel_proxy(tid, "", request)


# MARK: Dashboard — StaticFiles serves index.html at "/" (mounted LAST so the
# API and /{tid} proxy routes above take precedence). https://fastapi.tiangolo.com/tutorial/frontend/
app.mount("/", StaticFiles(directory=_frontend, html=True), name="frontend")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host="0.0.0.0", port=PORT,
                ws_ping_interval=20, ws_ping_timeout=30)

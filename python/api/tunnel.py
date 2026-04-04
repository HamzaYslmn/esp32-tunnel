# MARK: ESP32 Tunnel — WebSocket relay + HTTP proxy
"""
ESP32 connects via WebSocket, visitors access via HTTP proxy.

  WS   /api/tunnel/ws?id=<device>   — ESP32 connects here
  ANY  /api/tunnel/<device>/<path>   — Visitors reach ESP32 here
"""

import asyncio
import json
import re
import time
import uuid

from fastapi import APIRouter, WebSocket, WebSocketDisconnect, Request, HTTPException
from fastapi.responses import Response
from urllib.parse import unquote

from middleware import log

router = APIRouter(tags=["Tunnel"])

# MARK: Config
TIMEOUT = 30
GRACE_PERIOD = 15
MAX_PATH_LEN = 2048
MAX_BODY_LEN = 16384
_VALID_ID = re.compile(r"^[a-zA-Z0-9_-]{1,64}$")
_FORWARD_HEADERS = ("authorization", "cookie", "user-agent", "accept", "accept-language")


# MARK: Tunnel — isolates per-connection state
class Tunnel:
    __slots__ = ("ws", "routes", "_locked", "pending")

    def __init__(self, ws: WebSocket):
        self.ws = ws
        self.routes: list[dict] = []
        self._locked = False
        self.pending: dict[str, asyncio.Future] = {}

    def lock_config(self, routes: list) -> bool:
        if self._locked:
            return False
        self._locked = True
        self.routes = routes if isinstance(routes, list) else []
        return True

    def check_route(self, path: str, key: str) -> bool:
        if not self.routes:
            return True
        for r in self.routes:
            if path.startswith(r.get("path", "/")):
                pw = r.get("password")
                return not pw or key == pw
        return True

    def cancel_pending(self):
        for fut in self.pending.values():
            if not fut.done():
                fut.set_result({"status": 502, "body": "Device disconnected", "type": "text/plain"})
        self.pending.clear()


# MARK: State — `tunnels` is imported by root.py for /api/status
tunnels: dict[str, Tunnel] = {}
_grace: dict[str, float] = {}


# MARK: Helpers

def _safe_path(path: str) -> str:
    decoded = unquote(path)
    if ".." in decoded or "\x00" in decoded:
        return "/"
    if not decoded.startswith("/"):
        decoded = "/" + decoded
    return decoded[:MAX_PATH_LEN]


def _client_ip(request: Request) -> str:
    for hdr in ("cf-connecting-ip", "x-real-ip"):
        ip = request.headers.get(hdr, "")
        if ip:
            return ip
    forwarded = request.headers.get("x-forwarded-for", "")
    if forwarded:
        return forwarded.split(",")[0].strip()
    return request.client.host if request.client else ""


def _pick_headers(request: Request) -> dict:
    return {h: request.headers[h] for h in _FORWARD_HEADERS if h in request.headers}


def _clean_grace():
    now = time.time()
    for k in [k for k, v in _grace.items() if now - v > GRACE_PERIOD * 2]:
        del _grace[k]


# MARK: WebSocket endpoint — ESP32 connects here
@router.websocket("/tunnel/ws")
async def tunnel_ws(ws: WebSocket, id: str):
    if not _VALID_ID.match(id):
        await ws.close(4000, "invalid id")
        return

    await ws.accept()

    old = tunnels.pop(id, None)
    if old:
        try:
            await old.ws.close(4001, "replaced")
        except Exception:
            pass
        old.cancel_pending()

    tun = Tunnel(ws)
    tunnels[id] = tun
    _grace.pop(id, None)
    log.info(f"[tunnel] + {id} ({len(tunnels)} active)")

    try:
        while True:
            raw = await ws.receive_text()
            if len(raw) > MAX_BODY_LEN:
                continue
            msg = json.loads(raw)

            # MARK: Config — accept only ONCE, then lock
            if msg.get("type") == "config":
                tun.lock_config(msg.get("routes", []))
                continue

            rid = msg.get("id")
            if rid and rid in tun.pending:
                tun.pending[rid].set_result(msg)
    except (WebSocketDisconnect, Exception):
        pass
    finally:
        if tunnels.get(id) is tun:
            del tunnels[id]
        _grace[id] = time.time()
        _clean_grace()
        tun.cancel_pending()
        log.info(f"[tunnel] - {id} ({len(tunnels)} active)")


# MARK: HTTP proxy — visitors access ESP32 through this
async def tunnel_proxy(tid: str, path: str = "", request: Request = None):
    if not _VALID_ID.match(tid):
        raise HTTPException(400, "Invalid tunnel ID")

    tun = tunnels.get(tid)
    if not tun:
        if tid in _grace and time.time() - _grace[tid] < GRACE_PERIOD:
            raise HTTPException(503, "Tunnel unavailable")
        raise HTTPException(404, "Tunnel not found")

    safe = _safe_path("/" + path if path else "/")

    # MARK: Route auth
    key = request.query_params.get("key", "") if request else ""
    if not tun.check_route(safe, key):
        raise HTTPException(403, "Access denied")

    ip = _client_ip(request)
    body_str = ""
    ct = ""
    if request.method in ("POST", "PUT", "PATCH"):
        body_bytes = await request.body()
        if body_bytes and len(body_bytes) <= MAX_BODY_LEN:
            body_str = body_bytes.decode("utf-8", errors="replace")
        ct = request.headers.get("content-type", "")

    rid = str(uuid.uuid4())
    fut = asyncio.get_running_loop().create_future()
    tun.pending[rid] = fut

    try:
        msg = {"id": rid, "method": request.method, "path": safe, "ip": ip}
        if body_str:
            msg["body"] = body_str
        if ct:
            msg["ct"] = ct
        hdrs = _pick_headers(request)
        if hdrs:
            msg["hdrs"] = hdrs

        try:
            await tun.ws.send_json(msg)
        except Exception:
            if tunnels.get(tid) is tun:
                del tunnels[tid]
            _grace[tid] = time.time()
            raise HTTPException(502, "Device disconnected")

        r = await asyncio.wait_for(fut, timeout=TIMEOUT)
        resp_body = r.get("body", "")
        if len(resp_body) > MAX_BODY_LEN:
            resp_body = resp_body[:MAX_BODY_LEN]
        return Response(
            resp_body,
            status_code=r.get("status", 200),
            media_type=r.get("type", "text/html; charset=utf-8"),
        )
    except asyncio.TimeoutError:
        raise HTTPException(504, "Device timeout")
    except HTTPException:
        raise
    except Exception:
        raise HTTPException(502, "Tunnel error")
    finally:
        tun.pending.pop(rid, None)

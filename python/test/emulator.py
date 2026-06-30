"""ESP32 emulator — speaks the esp32-tunnel WS protocol, no hardware needed.

As a library (tests):
    async with FakeESP32(id="dev", key="s3cret") as dev:
        ...                      # device is connected and answering

Standalone (drive the dashboard against a virtual device):
    uv run python test/emulator.py --id p2ptest --key s3cret
    # then open the server's dashboard and talk to "p2ptest"
"""
import argparse
import asyncio
import json

import websockets


class FakeESP32:
    def __init__(self, server="ws://localhost:8000", id="emu", key="",
                 handler=None, p2p_answer=""):
        self.server, self.id, self.key = server, id, key
        self.handler = handler or self._default      # (method, path, body) -> (status, body, ctype)
        self.p2p_answer = p2p_answer                 # "" = decline P2P (relay fallback)
        self._ws = self._task = None

    def _default(self, method, path, body):
        if path.rstrip("/") in ("", "/status"):
            return 200, json.dumps({"device": self.id, "heap": 128000}), "application/json"
        return 200, f"{method} {path}", "text/plain"

    async def _serve(self, ws):
        async for raw in ws:
            m = json.loads(raw)
            if m.get("type") == "webrtc":
                await ws.send(json.dumps({"id": m["id"], "sdp": self.p2p_answer}))
            elif m.get("method"):
                status, body, ctype = self.handler(m["method"], m.get("path", "/"), m.get("body", ""))
                await ws.send(json.dumps({"id": m["id"], "status": status, "body": body, "type": ctype}))

    async def __aenter__(self):
        url = f"{self.server}/api/tunnel/ws?id={self.id}"
        if self.key:
            url += f"&token={self.key}"
        self._ws = await websockets.connect(url)
        self._task = asyncio.create_task(self._serve(self._ws))
        return self

    async def __aexit__(self, *_):
        self._task.cancel()
        await self._ws.close()

    async def run_forever(self):
        async with self:
            print(f"[emulator] {self.id} connected to {self.server} "
                  f"({'public' if not self.key else 'key=' + self.key})")
            await asyncio.Future()   # until Ctrl+C


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="ws://localhost:8000")
    ap.add_argument("--id", default="emu")
    ap.add_argument("--key", default="")
    ap.add_argument("--p2p", action="store_true", help="answer P2P with a fake SDP (default: decline)")
    a = ap.parse_args()
    try:
        asyncio.run(FakeESP32(a.server, a.id, a.key,
                              p2p_answer="v=0\\r\\nfake-answer" if a.p2p else "").run_forever())
    except KeyboardInterrupt:
        pass

"""End-to-end auth test: spins up the real app + a FakeESP32, checks access
control over real HTTP/WS. No hardware. Run: uv run python test/test_auth.py
"""
import asyncio
import pathlib
import sys
import threading
import time
import urllib.error
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).parent.parent))   # repo: python/

import uvicorn

from emulator import FakeESP32
from main import app

BASE = "http://127.0.0.1:8011"


def get(devid, key=None, where="query"):
    url = f"{BASE}/{devid}/status"
    req = urllib.request.Request(url)
    if key and where == "query":
        req.full_url = f"{url}?key={key}"
    elif key and where == "xquery":
        req.full_url = f"{url}?x-tunnel-key={key}"
    elif key and where == "header":
        req.add_header("X-Tunnel-Key", key)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code


async def main():
    loop = asyncio.get_running_loop()
    async def g(*a, **k): return await loop.run_in_executor(None, lambda: get(*a, **k))

    results = []
    def ok(name, got, want): results.append((name, got, want, got == want))

    # ── secured device ──
    async with FakeESP32(f"ws://127.0.0.1:8011", "secdev", "s3cret"):
        await asyncio.sleep(0.4)
        ok("no key -> 401",            await g("secdev"), 401)
        ok("?key= -> 200",             await g("secdev", "s3cret", "query"), 200)
        ok("?x-tunnel-key= -> 200",    await g("secdev", "s3cret", "xquery"), 200)
        ok("header -> 200",            await g("secdev", "s3cret", "header"), 200)
        ok("wrong key -> 401",         await g("secdev", "nope", "query"), 401)
        two = await asyncio.gather(g("secdev", "s3cret"), g("secdev", "s3cret"))
        ok("2 clients -> 200/200",     tuple(two), (200, 200))
        try:                            # hijack: same id, wrong token
            import websockets
            async with websockets.connect("ws://127.0.0.1:8011/api/tunnel/ws?id=secdev&token=WRONG") as w:
                await w.recv(); hij = "not-closed"
        except Exception:
            hij = "rejected"
        ok("hijack -> rejected", hij, "rejected")

    # ── public device ──
    async with FakeESP32(f"ws://127.0.0.1:8011", "pubdev", ""):
        await asyncio.sleep(0.4)
        ok("public, no key -> 200", await g("pubdev"), 200)

    for name, got, want, good in results:
        print(f"  [{'OK' if good else 'FAIL'}] {name:24} got={got}")
    if not all(r[3] for r in results):
        raise SystemExit("auth test FAILED")
    print("auth e2e ok")


if __name__ == "__main__":
    server = uvicorn.Server(uvicorn.Config(app, host="127.0.0.1", port=8011, log_level="error"))
    threading.Thread(target=server.run, daemon=True).start()
    for _ in range(50):                                   # wait until listening
        try:
            urllib.request.urlopen(f"{BASE}/api/status", timeout=1); break
        except Exception:
            time.sleep(0.1)
    asyncio.run(main())
    server.should_exit = True

# Runnable check for P2P signaling + auth — drives the endpoint directly in one
# asyncio loop with a fake device WS, avoiding TestClient's portal threading.
import asyncio

from fastapi import HTTPException
from starlette.requests import Request

from api.tunnel import tunnel_signal, tunnels, Tunnel


class FakeWS:
    def __init__(self): self.sent = []
    async def send_json(self, m): self.sent.append(m)


def _req(body: bytes, key: str = "") -> Request:
    async def receive(): return {"type": "http.request", "body": body, "more_body": False}
    headers = [(b"x-tunnel-key", key.encode())] if key else []   # key via header, not query
    return Request({"type": "http", "method": "POST", "headers": headers}, receive)


async def _run(answer_sdp, token="", key=""):
    ws = FakeWS()
    tunnels["dev"] = Tunnel(ws, token)
    try:
        task = asyncio.create_task(tunnel_signal("dev", _req(b"OFFER_SDP", key)))
        await asyncio.sleep(0.05)                       # let it forward + register pending
        if not ws.sent:                                 # rejected before forwarding (e.g. 401)
            return await task
        rid = ws.sent[0]["id"]
        tunnels["dev"].pending[rid].set_result({"id": rid, "sdp": answer_sdp})
        return await task
    finally:
        tunnels.pop("dev", None)


async def expect(coro, code):
    try:
        r = await coro
        assert code == 200 and r.status_code == 200, f"expected {code}, got {r.status_code}"
        return r
    except HTTPException as e:
        assert e.status_code == code, f"expected {code}, got {e.status_code}"


async def main():
    # open device (no token)
    r = await _run("ANSWER_SDP")
    assert r.status_code == 200 and r.body == b"ANSWER_SDP"
    await expect(_run(""), 501)                          # device declined -> fallback
    await expect(tunnel_signal("nope", _req(b"OFFER_SDP")), 404)

    # secured device (token set)
    await expect(_run("ANSWER", token="s3cret"), 401)               # no key -> blocked
    await expect(_run("ANSWER", token="s3cret", key="wrong"), 401)  # bad key -> blocked
    r = await _run("ANSWER", token="s3cret", key="s3cret")          # right key -> through
    assert r.status_code == 200 and r.body == b"ANSWER"

    print("signaling + auth ok")


if __name__ == "__main__":
    asyncio.run(main())

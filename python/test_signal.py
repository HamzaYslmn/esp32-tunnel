# Runnable check for P2P signaling logic — drives the endpoint directly in one
# asyncio loop with a fake device WS, avoiding TestClient's portal threading.
import asyncio

from fastapi import HTTPException
from starlette.requests import Request

from api.tunnel import tunnel_signal, tunnels, Tunnel


class FakeWS:
    def __init__(self): self.sent = []
    async def send_json(self, m): self.sent.append(m)


def _req(body: bytes) -> Request:
    async def receive(): return {"type": "http.request", "body": body, "more_body": False}
    return Request({"type": "http", "method": "POST", "headers": []}, receive)


async def _run(answer_sdp):
    ws = FakeWS()
    tun = Tunnel(ws)
    tunnels["dev"] = tun
    try:
        task = asyncio.create_task(tunnel_signal("dev", _req(b"OFFER_SDP")))
        await asyncio.sleep(0.05)                       # let it forward + register pending
        assert ws.sent[0]["type"] == "webrtc" and ws.sent[0]["sdp"] == "OFFER_SDP"
        rid = ws.sent[0]["id"]
        tun.pending[rid].set_result({"id": rid, "sdp": answer_sdp})  # device replies
        return await task
    finally:
        tunnels.pop("dev", None)


async def main():
    resp = await _run("ANSWER_SDP")
    assert resp.status_code == 200 and resp.body == b"ANSWER_SDP"

    try:
        await _run("")                                  # empty answer = device declined
        assert False, "expected 501"
    except HTTPException as e:
        assert e.status_code == 501

    # unknown device -> 404
    try:
        await tunnel_signal("nope", _req(b"OFFER_SDP"))
        assert False, "expected 404"
    except HTTPException as e:
        assert e.status_code == 404

    print("signaling ok")


if __name__ == "__main__":
    asyncio.run(main())

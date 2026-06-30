# Tests & ESP32 emulator

No hardware needed — `emulator.py` speaks the device WS protocol.

```bash
# from python/
uv run python test/test_signal.py   # P2P signaling + auth (direct, no server)
uv run python test/test_auth.py     # e2e: real app + emulated device over HTTP/WS
```

## Emulate a device against a running server

```bash
uv run python main.py &                              # start the relay (port 8000)
uv run python test/emulator.py --id p2ptest --key s3cret
# open http://localhost:8000  -> Device tab -> id "p2ptest", key "s3cret"
```

`--key ""` (omit) = public device. `--p2p` answers signaling with a fake SDP.
Access a secured device via `?key=<key>` in the URL or the `X-Tunnel-Key` header.

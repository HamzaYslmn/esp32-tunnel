# esp32-tunnel

[![Arduino Library](https://img.shields.io/badge/Arduino-Library-blue?logo=arduino)](https://github.com/HamzaYslmn/esp32-tunnel)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![GitHub Stars](https://img.shields.io/github/stars/HamzaYslmn/esp32-tunnel?style=social)](https://github.com/HamzaYslmn/esp32-tunnel)

Expose your ESP32 web server to the public internet — no port forwarding, no ngrok, no cloud accounts, no companion devices.

Three providers (pick one):
- **Self-hosted** (default) — plain WebSocket relay, no TLS on ESP32, saves ~40 KB RAM
- **[localtunnel](https://localtunnel.me)** — free HTTPS subdomain URLs (uses WiFiClientSecure)
- **[bore](https://github.com/ekzhang/bore)** — free TCP tunnel via bore.pub (no login, no account, no TLS)

## Features

- **One API, three providers** — `tunnelSetup(SELFHOST | LOCALTUNNEL | BORE, ...)`
- **Secure by default** — auto-generated per-device access key (self-hosted)
- **Runs itself** — a FreeRTOS task drives the tunnel; `loop()` stays free (ESP32)
- **ESPAsyncWebServer compatible** — or handler mode for direct, no-proxy handling
- **Local + remote** — reach the device at `http://<id>.local` (mDNS) or the public URL
- **Auto-reconnect** — WiFi drops, stale connections, tunnel expiry all handled
- **Lean** — self-hosted uses plain WiFiClient (no TLS), ~40 KB less RAM

## Footprint & trimming (ESP32)

Measured with arduino-cli. The library's own code is small — most of the flash is
WiFi + TLS (the `https` relay) + your web server, not the tunnel.

| Config | Flash | Trim |
|---|---|---|
| Relay + local web server + mDNS (typical) | ~1108 KB | — |
| Handler/P2P mode + `#define TUN_MDNS 0` | ~1037 KB | **−71 KB** |

- **Drop the local web server** — use handler mode (`tunnelSetup(SELFHOST, handler, "host/id")`); no `ESPAsyncWebServer`, saves its flash **and** its runtime `AsyncTCP` task/buffers.
- **`#define TUN_MDNS 0`** before the include — drops mDNS (~30 KB) if you don't need `<id>.local`.
- **`#define TUN_TASK_STACK 6144`** — trims the background-task stack (RAM). Keep ≥8 KB if your relay is `https` (TLS needs the headroom).
- **CPU** is a non-issue — the task idles on a 10 ms poll (microsecond checks). Don't optimize it.

## Dependencies

Automatically installed:
- **[espfetch](https://github.com/HamzaYslmn/espfetch)** — neofetch-style system info + ESPLogger
- **[esp-rtosSerial](https://github.com/HamzaYslmn/esp-rtosSerial)** — thread-safe Serial for FreeRTOS (`#include <rtosSerial.h>`)

## Installation

### Arduino Library Manager

1. Open Arduino IDE
2. **Sketch → Include Library → Manage Libraries**
3. Search for **"esp32-tunnel"**
4. Click **Install**

### Manual

Download ZIP → **Sketch → Include Library → Add .ZIP Library**

## Quick Start

### Self-hosted (default — lightweight, no TLS on ESP)

```cpp
#include <ESPAsyncWebServer.h>
#include <esp32tunnel.h>
#include <esp32tunnel_testpage.h>

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);
  WiFi.begin("SSID", "PASS");
  while (WiFi.status() != WL_CONNECTED) delay(500);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "text/html", TUN_TEST_HTML);
  });
  server.begin();

  tunnelSetup(SELFHOST, "myserver.com/my-device");
}

void loop() {}   // ESP32: tunnel runs in its own task — loop() is free
```

> **ESP32:** `tunnelSetup()` starts a background FreeRTOS task, so you do **not**
> call `tunnelLoop()`. Leave `loop()` empty, use it for your own code, or
> `vTaskDelete(NULL)` to reclaim its stack. Enable request logs with `tunnelLog()`.
> Tune the task with `-DTUN_TASK_CORE=0 -DTUN_TASK_PRIO=2 -DTUN_TASK_STACK=10240`.
>
> **ESP8266** has no such task — there you must call `tunnelLoop()` in `loop()`.

### Localtunnel (free HTTPS — no server needed)

```cpp
#include <ESPAsyncWebServer.h>
#include <esp32tunnel.h>
#include <esp32tunnel_testpage.h>

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);
  WiFi.begin("SSID", "PASS");
  while (WiFi.status() != WL_CONNECTED) delay(500);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "text/html", TUN_TEST_HTML);
  });
  server.begin();

  tunnelSetup(LOCALTUNNEL, "my-esp32");
}

void loop() { tunnelLoop(); }
```

### Bore (free TCP tunnel — no login, no account)

```cpp
#include <ESPAsyncWebServer.h>
#include <esp32tunnel.h>
#include <esp32tunnel_testpage.h>

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);
  WiFi.begin("SSID", "PASS");
  while (WiFi.status() != WL_CONNECTED) delay(500);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "text/html", TUN_TEST_HTML);
  });
  server.begin();

  tunnelSetup(BORE);  // public URL: http://bore.pub:PORT
}

void loop() { tunnelLoop(); }
```

## API

All providers expose the same public API:

| Function | Description |
|---|---|
| `tunnelSetup(...)` | Start tunnel (args differ per provider). ESP32: spawns the task |
| `tunnelLog(bool)` | Enable/disable request logging |
| `tunnelPublic()` | Disable auth — open access (call before `tunnelSetup`) |
| `tunnelKey()` | The device's access key (`""` if public) |
| `tunnelLocalURL()` | Direct LAN URL `http://<id>.local` (mDNS) |
| `tunnelLoop()` | ESP8266 only — drive tunnel in `loop()`. No-op once the ESP32 task runs |
| `tunnelStop()` | Stop tunnel, end the task, free resources |
| `tunnelURL()` | Public URL or `"(connecting...)"` |
| `tunnelReady()` | `true` when tunnel is live |
| `tunnelLastIP()` | Last requester's IP address |
| `tunnelProviderName()` | `"self-hosted"`, `"localtunnel"`, or `"bore"` |

### Self-hosted `tunnelSetup()`

```cpp
tunnelSetup(SELFHOST, "host/device-id");           // proxy mode (local port 80)
tunnelSetup(SELFHOST, handler, "host/device-id");  // handler callback (no proxy)
tunnelSetup(SELFHOST, "host/device-id", "pass");   // custom password (whole tunnel)
tunnelSetup(SELFHOST, "host/device-id", routes);   // per-route auth
tunnelPublic();                                    // before setup: OPEN access (no key)
tunnelP2P(answerFn);                               // opt-in WebRTC P2P (see below)
```

#### Access paths — local or public, your choice

| Path | URL | Latency | Needs |
|---|---|---|---|
| **Local (direct)** | `http://<id>.local/` or the LAN IP | ~5 ms | same WiFi |
| **Public (WS relay)** | `https://server/<id>?key=<key>` | ~200 ms | works anywhere |
| **Public (P2P)** | via `p2p.js` | direct RTT | a WebRTC engine on the device |

On the same network, skip the tunnel entirely — the device serves its own pages at
`http://<id>.local/` (mDNS, on by default; `tunnelLocalURL()`) or its LAN IP. The
tunnel is only for reaching it from outside. The dashboard exposes all three as tabs.

#### Access keys (secure by default)

Self-hosted devices are **private by default** — the ESP32 generates a random key on
first boot (persisted in NVS, read via `tunnelKey()`). Send it as `?key=<key>` or the
`X-Tunnel-Key` header (the dashboard and `p2p.js` do this for you). Set your own via
the password overload above, or call `tunnelPublic()` to disable auth. Direct LAN
access hits the device's own server and isn't gated by the key.

#### P2P mode (offload traffic from your relay)

By default the server relays every request/response (acts like an HTTP VPN). With
`tunnelP2P()`, the server only brokers a WebRTC handshake and visitors connect
**peer-to-peer** — your server stops carrying the traffic. Falls back to the relay
automatically when P2P can't be established (symmetric NAT, P2P disabled).

The WebRTC engine itself (ICE+DTLS+SCTP, e.g. [libpeer](https://github.com/sepfy/libpeer))
is **not** bundled — it's the one piece that can't be header-only. You provide it in
`answerFn`; the library does the signaling. Browser side loads `https://yourserver/p2p.js`.
See [`examples/P2P`](examples/P2P/P2P.ino).

### Localtunnel `tunnelSetup()`

```cpp
tunnelSetup(LOCALTUNNEL);                            // random subdomain
tunnelSetup(LOCALTUNNEL, "my-esp32");                // custom subdomain
tunnelSetup(LOCALTUNNEL, "my-esp32", TUN_STRICT);    // fail if subdomain is taken
tunnelSetup(LOCALTUNNEL, handler, "my-esp32");       // handler callback
```

### Bore `tunnelSetup()`

```cpp
tunnelSetup(BORE);                                   // bore.pub, random port
tunnelSetup(BORE, "your-server.com");                // self-hosted bore server
```

## Providers

| | Self-hosted | localtunnel | bore |
|---|---|---|---|
| Enum | `SELFHOST` | `LOCALTUNNEL` | `BORE` |
| TLS on ESP32 | ❌ (plain WS) | ✅ (WiFiClientSecure) | ❌ (plain TCP) |
| RAM usage | Low (~40 KB less) | Higher (TLS) | Low |
| URL format | `http://host/device-id` | `https://xxx.loca.lt` | `http://bore.pub:PORT` |
| Protocol | WebSocket relay | TCP pool | TCP tunnel |
| Custom name | ✅ path-based | ✅ subdomain | ❌ random port |
| Needs server | ✅ | ❌ | ❌ (bore.pub free) |
| Account needed | ❌ | ❌ | ❌ |

## Self-Hosted Server

A free public server is available at `esp32-tunnel.onrender.com`.
Pick a unique device ID:

```cpp
#include <esp32tunnel.h>

void setup() {
  // ...
  tunnelSetup(SELFHOST, "esp32-tunnel.onrender.com/my-device");
  // Visit: http://esp32-tunnel.onrender.com/my-device
}
```

> **Note:** The relay server must accept plain WebSocket (`ws://`) connections.
> If your server is behind HTTPS-only (e.g. Render.com), you'll need a plain WS
> endpoint or a proxy that terminates TLS before reaching the ESP32 connection.

### Deploy Your Own (Render.com)

[![Deploy to Render](https://render.com/images/deploy-to-render-button.svg)](https://render.com/deploy?repo=https://github.com/HamzaYslmn/esp32-tunnel)

Or manually:

1. Fork this repo on GitHub
2. Go to [render.com](https://render.com) → **New Web Service**
3. Connect your fork
4. Configure:

| Setting | Value |
|---|---|
| **Root Directory** | `python` |
| **Environment** | Add `PORT` = `8000` |
| **Build Command** | `pip install uv && uv sync --active` |
| **Start Command** | `uv run --active main.py` |

5. Deploy — your server will be at `your-app.onrender.com`

The server provides:
- **WebSocket tunnel** relay between visitors and ESP32
- **Dashboard** at the root URL with live server stats
- **Status API** at `/api/status` for health checks

## Tuning

Override **before** `#include`:

```cpp
#define TUN_POOL      2         // localtunnel pool size (default: 2)
#define TUN_STALE     30000     // recycle connections after 30s
#define TUN_REALLOC   12        // re-allocate tunnel every 12h
#define TUN_LOG       0         // disable tunnel Serial logs
#include <esp32tunnel.h>
```

## How It Works

### Self-hosted
```
Browser → your-server (HTTPS) → WebSocket → ESP32 → JSON response → back
```

### localtunnel
```
Browser → loca.lt (HTTPS) → TCP pool → ESP32 → HTTP response → back
```

### bore
```
Browser → bore.pub:PORT (HTTP) → TCP tunnel → ESP32 localhost:80 → back
```

## Examples

| Example | Description |
|---|---|
| [SelfHosted](examples/SelfHosted) | Full-featured self-hosted relay (auth, TLS, handler) |
| [Localtunnel](examples/Localtunnel) | Free HTTPS URL via localtunnel.me |
| [Bore](examples/Bore) | Free TCP tunnel via bore.pub (no login) |
| [HandlerMode](examples/HandlerMode) | Direct request handling (no AsyncWebServer) |
| [DualCore](examples/DualCore) | ESP32 FreeRTOS task on dedicated core |

## Companion Libraries

- [espfetch](https://github.com/HamzaYslmn/espfetch) — neofetch-style system info + ESPLogger (Python-style logging)
- [esp-rtosSerial](https://github.com/HamzaYslmn/esp-rtosSerial) — thread-safe Serial reads for FreeRTOS

## License

MIT

## Author

**Hamza Yesilmen** — [@HamzaYslmn](https://github.com/HamzaYslmn)

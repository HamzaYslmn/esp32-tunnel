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

- **Unified API** — `tunnelSetup(SELFHOST, ...)` or `tunnelSetup(LOCALTUNNEL, ...)` or `tunnelSetup(BORE)`
- **No WiFiClientSecure for self-hosted** — plain WiFiClient, ~40 KB less RAM
- **ESPAsyncWebServer compatible** — forwards requests to localhost (local proxy)
- **Handler mode** — optional callback for direct request handling (no proxy)
- **FreeRTOS dual-core** — Core 0 maintains tunnel, Core 1 serves requests
- **Auto-reconnect** — WiFi drops, stale connections, tunnel expiry all handled
- **Security hardened** — path sanitization, bounded reads
- **Built-in logging** — Python/FastAPI-style `logger.info()`, `logger.request()` via [ESPLogger](https://github.com/HamzaYslmn/espfetch)
- **Thread-safe Serial** — reads via [esp-rtosSerial](https://github.com/HamzaYslmn/esp-rtosSerial)
- **Simple API** — `tunnelSetup()`, `tunnelURL()`, `tunnelReady()`

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

void loop() { tunnelLoop(); }
```

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
| `tunnelSetup(...)` | Start tunnel (args differ per provider) |
| `tunnelLoop()` | Drive tunnel — call in `loop()` or FreeRTOS task |
| `tunnelStop()` | Stop tunnel and free resources |
| `tunnelURL()` | Public URL or `"(connecting...)"` |
| `tunnelReady()` | `true` when tunnel is live |
| `tunnelLastIP()` | Last requester's IP address |
| `tunnelProviderName()` | `"self-hosted"`, `"localtunnel"`, or `"bore"` |

### Self-hosted `tunnelSetup()`

```cpp
tunnelSetup(SELFHOST, "host/device-id");           // proxy mode (local port 80)
tunnelSetup(SELFHOST, handler, "host/device-id");  // handler callback (no proxy)
tunnelSetup(SELFHOST, "host/device-id", "pass");   // global password (?key=pass)
tunnelSetup(SELFHOST, "host/device-id", routes);   // per-route auth
```

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

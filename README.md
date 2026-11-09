# esp32-tunnel

[![Arduino Library](https://img.shields.io/badge/Arduino-Library-blue?logo=arduino)](https://github.com/HamzaYslmn/esp32-tunnel)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![GitHub Stars](https://img.shields.io/github/stars/HamzaYslmn/esp32-tunnel?style=social)](https://github.com/HamzaYslmn/esp32-tunnel)

Put your ESP32 web server on the internet. No port forwarding, no router setup, no account.
Your sketch stays a normal web server; one line opens a public link to it.

## Install

Arduino IDE → **Sketch → Include Library → Manage Libraries** → search **esp32-tunnel** → Install.
(ESP Async WebServer and AsyncTCP are installed with it.)

## Quick start

```cpp
#include <ESPAsyncWebServer.h>
#include <esp32tunnel.h>

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);
  WiFi.setSleep(false);                        // much faster replies through the tunnel
  WiFi.begin("YOUR_SSID", "YOUR_PASS");
  while (WiFi.status() != WL_CONNECTED) delay(500);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "text/html", "<h1>Hello from ESP32</h1>");
  });
  server.begin();

  tunnelSetup(SELFHOST, "https://esp32-tunnel-waa0.onrender.com/my-device");   // pick a unique id
}

void loop() {
  static bool shown = false;
  if (!shown && tunnelReady()) {               // the tunnel runs in its own task; loop() is yours
    shown = true;
    Serial.printf("Open: %s/?key=%s\n", tunnelURL().c_str(), tunnelKey());
  }
}
```

Open the printed link from anywhere. On the same WiFi, `http://my-device.local/` reaches the
device directly (faster, no key).

## Pick a provider

Change one line; everything else stays the same.

| Provider | Call | Link you get | Typical reply* | Notes |
|---|---|---|---|---|
| **Self-hosted relay** | `tunnelSetup(SELFHOST, "https://host/id")` | `https://host/id/?key=…` | 250 ms | Private by default (access key). Free public relay, or [run your own](#run-your-own-relay) |
| **OpenPort** | `tunnelSetup(OPENPORT)` | `http://openport.pw:PORT` | 190 ms | Same port every boot, any page size. ESP32 |
| **Tunnelmole** | `tunnelSetup(TUNNELMOLE)` | `https://xxx.tunnelmole.net` | 900 ms | Random link, changes on reconnect. ESP32 |
| **localtunnel** | `tunnelSetup(LOCALTUNNEL, "name")` | `https://name.loca.lt` | 0.7–1.3 s | Browsers see a loca.lt reminder page first |
| **bore** | `tunnelSetup(BORE)` | `http://bore.pub:PORT` | ~1 s | Random port each boot. Some ISPs block bore.pub |

\*Measured on an ESP32 over home WiFi. All are free and need no account. Only the self-hosted
relay has an access key; with the others, anyone who has the link can open it.

## Things to know

- **Use relative links in your pages** (`href="status"`, `fetch("data")`, not `"/status"`).
  On the relay your site lives under `/<device-id>/`, and a leading `/` points outside it.
- **Access key (self-hosted).** Made once and kept in flash; `tunnelKey()` returns it. Visitors add
  `?key=<key>` (or send an `X-Tunnel-Key` header). Change it with
  `tunnelSetup(SELFHOST, url, "my-secret")`, or remove it with `tunnelPublic()` before `tunnelSetup()`.
- **Page size.** Self-hosted, Tunnelmole and handler mode hold one reply in RAM, up to `TUN_MAX_BODY`
  (16 KB default, 64 KB max without PSRAM). Bigger replies get a clear `502`. OpenPort, bore and
  localtunnel stream, so any size works.
- **No Server-Sent Events or WebSockets through the tunnel.** One request gets one finished reply,
  so poll instead: `setInterval(() => fetch("state").then(r => r.json()).then(show), 1000)`.
  Self-hosted and Tunnelmole answer an SSE request with a `501`.
- **`WiFi.setSleep(false)`** — with WiFi power saving on, replies slow down a lot
  (measured: 1 KB took 11 s with it on, 64 ms off).
- **Text only through the self-hosted relay** (HTML, CSS, JS, JSON, SVG).
  For images and other binary files use OpenPort, bore or localtunnel.

## Handler mode (no web server)

Skip ESPAsyncWebServer and answer requests in a function. About 40 KB less flash (measured:
HandlerMode vs SelfHosted example).

```cpp
String handler(const String &method, const String &path) {
  if (path == "/") return "<h1>Hello</h1>";
  if (path == "/data") { tunnelContentType("application/json"); return "{\"t\":21.5}"; }
  return "";                                   // 404
}

tunnelSetup(SELFHOST, handler, "https://esp32-tunnel-waa0.onrender.com/my-device");
// also: tunnelSetup(TUNNELMOLE, handler, nullptr);
```

Serve files from flash with the optional headers:

```cpp
#include <esp32tunnel_progmem.h>   // tunnelProgmem(INDEX_HTML, "text/html")
#include <esp32tunnel_littlefs.h>  // tunnelFile(path)  — from LittleFS, "/" → index.html
```

## API

| Function | What it does |
|---|---|
| `tunnelSetup(provider, ...)` | Start the tunnel (see the provider table) |
| `tunnelReady()` / `tunnelURL()` | Is it live? / the public link |
| `tunnelKey()` | Self-hosted access key (`""` if public) |
| `tunnelPublic()` | No access key — call before `tunnelSetup()` |
| `tunnelLocalURL()` | `http://<id>.local` for the same WiFi |
| `tunnelLog()` | Print every request to Serial |
| `tunnelContentType(type)` | Handler mode: Content-Type of this reply |
| `tunnelLastIP()` | IP of the last visitor |
| `tunnelProviderName()` | `"self-hosted"`, `"openport"`, `"tunnelmole"`, `"localtunnel"`, `"bore"` |
| `tunnelCACert(pem)` | Self-hosted: verify the relay's TLS certificate |
| `tunnelStop()` | Close the tunnel |
| `tunnelLoop()` | ESP8266 only — call in `loop()`. On ESP32 the tunnel runs itself |

## Options

`#define` before `#include <esp32tunnel.h>`, or pass as `-D` build flags.

| Option | Default | What it does |
|---|---|---|
| `TUN_MAX_BODY` | `16384` | Largest reply in bytes for self-hosted/Tunnelmole/handler mode (max 65535 without PSRAM) |
| `TUN_PORT` | `80` | Port of your local web server |
| `TUN_MDNS` | `1` | `0` drops `<id>.local` (~30 KB flash) |
| `TUN_NO_LOCALTUNNEL` | — | Leave localtunnel out (saves 5.1 KB) |
| `TUN_NO_OPENPORT` | — | Leave OpenPort out (3.7 KB) |
| `TUN_NO_TUNNELMOLE` | — | Leave Tunnelmole out (3.0 KB) |
| `TUN_NO_BORE` | — | Leave bore out (1.9 KB). All four: 14.5 KB |
| `TUN_TASK_CORE` / `TUN_TASK_PRIO` / `TUN_TASK_STACK` | `1` / `2` / `8192` | Tunnel task. Keep the priority above 1 (`loop()`'s), or a busy `loop()` stalls the tunnel |

## Run your own relay

The relay is a small Python (FastAPI) server in [`python/`](python). It has a dashboard at `/`
and a health check at `/api/status`.

[![Deploy to Render](https://render.com/images/deploy-to-render-button.svg)](https://render.com/deploy?repo=https://github.com/HamzaYslmn/esp32-tunnel)

Or anywhere with Python 3.13+:

```bash
cd python && uv run main.py          # listens on 0.0.0.0:8000
```

Then use `tunnelSetup(SELFHOST, "https://your-host/my-device")`. Use `https://` for TLS;
without a scheme the ESP32 connects over plain `ws://` (less RAM, for LAN or trusted servers).

## Examples

| Example | Shows |
|---|---|
| [SelfHosted](examples/SelfHosted) | Web server through the relay, access key, `.local` link |
| [HandlerMode](examples/HandlerMode) | No web server — a function answers requests |
| [DualCore](examples/DualCore) | `loop()` keeps working (a sensor) while the tunnel serves it |
| [OpenPort](examples/OpenPort) | Permanent free port via openport.pw |
| [Tunnelmole](examples/Tunnelmole) | Free HTTPS link via tunnelmole.com |
| [Localtunnel](examples/Localtunnel) | Free HTTPS link via loca.lt |
| [Bore](examples/Bore) | Free TCP port via bore.pub |

## License

MIT — **Hamza Yesilmen** ([@HamzaYslmn](https://github.com/HamzaYslmn))

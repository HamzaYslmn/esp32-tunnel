/*
 * SelfHosted.ino — Full-featured self-hosted relay tunnel
 *
 * Demonstrates ALL tunnel features with the self-hosted provider:
 *   - ESPAsyncWebServer proxy (default)
 *   - Global password protection
 *   - Per-route authentication (RouteConfig)
 *   - TLS CA cert verification
 *   - Handler mode (no proxy)
 *   - Serial commands (espfetch, url, ip)
 *
 * The relay server forwards browser HTTPS requests to this device over
 * a plain WebSocket — no TLS needed on the ESP, saving ~40 KB RAM.
 *
 * A free public relay is available at:
 *   https://esp32-tunnel-waa0.onrender.com
 *
 * Public URL:  https://esp32-tunnel-waa0.onrender.com/<your-device-id>
 *
 * Board: ESP32 or ESP8266
 */

#include <ESPAsyncWebServer.h>
#include <esp32tunnel.h>
#include <esp32tunnel_testpage.h>
#include <espfetch.h>
#include <rtosSerial.h>

// ── Configuration ────────────────────────────────────────────
const char *WIFI_SSID = "YOUR_SSID";
const char *WIFI_PASS = "YOUR_PASS";

// Replace with your relay server + unique device ID
const char *TUNNEL_SERVER = "https://esp32-tunnel-waa0.onrender.com/my-device";
// ─────────────────────────────────────────────────────────────

AsyncWebServer server(80);

// ── Authentication (uncomment ONE to use) ────────────────────
// Option A: Per-route passwords (longest prefix match wins)
// RouteConfig routes[] = {
//   {"/api",   "api-secret"},    // /api/*   → ?key=api-secret
//   {"/admin", "admin-pass"},    // /admin/* → ?key=admin-pass
//   {"/",      nullptr}          // everything else → public
// };
//
// Option B: Global password (all routes)
// const char *GLOBAL_PASSWORD = "my-secret";

void setup() {
  rtosSerial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  logger.info("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) delay(500);
  logger.info("WiFi: %s", WiFi.localIP().toString().c_str());

  // MARK: Local routes (served through the tunnel)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "text/html", TUN_TEST_HTML);
  });

  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "application/json",
      "{\"pong\":true,\"heap\":" + String(ESP.getFreeHeap()) +
      ",\"uptime\":" + String(millis() / 1000) + "}");
  });

  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "application/json", "{\"sensor\":42,\"status\":\"ok\"}");
  });

  server.on("/admin", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "text/html", "<h1>Admin Panel</h1><p>Protected area.</p>");
  });

  server.begin();

  // MARK: Start tunnel — pick ONE:

  // No auth (public):
  tunnelSetup(SELFHOST, TUNNEL_SERVER);

  // Option A — per-route auth: uncomment RouteConfig above
  // tunnelSetup(SELFHOST, TUNNEL_SERVER, routes);

  // Option B — global password: ?key=my-secret on all routes
  // tunnelSetup(SELFHOST, TUNNEL_SERVER, GLOBAL_PASSWORD);

  // Optional: verify server TLS certificate
  // tunnelCACert(nullptr);  // nullptr = skip verification (insecure)
}

void loop() {
  // MARK: Drive the tunnel (required in loop or FreeRTOS task)
  tunnelLoop(true);

  // Print URL once ready
  static bool logged = false;
  if (!logged && tunnelReady()) {
    logger.info("Tunnel live: %s (%s)", tunnelURL().c_str(), tunnelProviderName());
    logged = true;
  }

  // MARK: Serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n'); cmd.trim();
    if (cmd.length() == 0) return;
    if (espfetch.check(cmd)) return;  // /espfetch — system info
    if (cmd == "url") logger.info("Tunnel: %s", tunnelURL().c_str());
    if (cmd == "ip")  logger.info("Last IP: %s", tunnelLastIP().c_str());
  }
}

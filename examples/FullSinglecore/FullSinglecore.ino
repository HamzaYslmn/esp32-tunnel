/*
 * FullSinglecore.ino — ESP8266 / ESP32 (single-core, cooperative)
 *
 * Demonstrates ALL tunnel features:
 *   - Public tunnel (no auth)
 *   - Global password protection
 *   - Per-route authentication (RouteConfig)
 *   - Custom log callback
 *   - Manual device token
 *
 * Note: tunnelSetup() and tunnelLoop() work on both ESP32 and ESP8266.
 *
 * Requires: ESPAsyncWebServer, esp32-tunnel, espfetch
 */

#include <ESPAsyncWebServer.h>
#include <esp32tunnel.h>
#include <esp32tunnel_testpage.h>
#include <espfetch.h>

// ── WiFi ─────────────────────────────────────────────────────
#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASS "YOUR_PASS"

// ── Tunnel server ────────────────────────────────────────────
#define TUNNEL_SERVER "https://esp32-tunnel.onrender.com/my-device"

// ─────────────────────────────────────────────────────────────

AsyncWebServer server(80);

// MARK: Per-route auth — each path can have its own password
RouteConfig routes[] = {
  {"/api",    "api-secret"},   // /api/* requires ?key=api-secret
  {"/admin",  "admin-pass"},   // /admin/* requires ?key=admin-pass
  {"/",       nullptr}          // everything else is public
};

void setup() {
  rtosSerial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.printf("WiFi: %s\n", WiFi.localIP().toString().c_str());

  // MARK: Local routes
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

  // Per-route auth:
  tunnelSetup(SELFHOST, TUNNEL_SERVER, routes);

  // Or global password:  tunnelSetup(SELFHOST, TUNNEL_SERVER, "password");
  // Or public tunnel:    tunnelSetup(SELFHOST, TUNNEL_SERVER);
}

void loop() {
  tunnelLoop(true);

  static bool logged = false;
  if (!logged && tunnelReady()) {
    Serial.printf("Tunnel ready: %s (%s)\n", tunnelURL().c_str(), tunnelProviderName());
    logged = true;
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n'); cmd.trim();
    if (cmd.length() == 0) return;
    if (espfetch.check(cmd)) return;
    if (cmd == "url") Serial.printf("Tunnel: %s\n", tunnelURL().c_str());
    if (cmd == "ip")  Serial.printf("Last IP: %s\n", tunnelLastIP().c_str());
  }
}

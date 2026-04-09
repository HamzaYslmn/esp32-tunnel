/*
 * FullMulticore.ino — ESP32 (dual-core, FreeRTOS)
 *
 * Demonstrates ALL tunnel features:
 *   - Public tunnel (no auth)
 *   - Global password protection
 *   - Per-route authentication (RouteConfig)
 *   - Custom log callback
 *   - Manual device token
 *   - TLS CA cert verification
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
#define TUNNEL_SERVER "https://esp32-tunnel-waa0.onrender.com/my-device"

// ─────────────────────────────────────────────────────────────

AsyncWebServer server(80);

// MARK: Per-route auth — each path can have its own password
RouteConfig routes[] = {
  {"/api",    "api-secret"},   // /api/* requires ?key=api-secret
  {"/admin",  "admin-pass"},   // /admin/* requires ?key=admin-pass
  {"/",       nullptr}          // everything else is public
};

// MARK: FreeRTOS task — runs tunnelLoop on core 1
void tunnelTask(void *) {

  // Per-route auth:
  tunnelSetup(SELFHOST, TUNNEL_SERVER, routes);

  // Or global password:  tunnelSetup(SELFHOST, TUNNEL_SERVER, "password");
  // Or public tunnel:    tunnelSetup(SELFHOST, TUNNEL_SERVER);

  while (true) {
    tunnelLoop(true);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  rtosSerial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  logger.info("WiFi: %s", WiFi.localIP().toString().c_str());

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

  xTaskCreatePinnedToCore(tunnelTask, "tunnel", 8192, nullptr, 3, nullptr, 1);
}

void loop() {
  static bool logged = false;
  if (!logged && tunnelReady()) {
    logger.info("Tunnel ready: %s (%s)", tunnelURL().c_str(), tunnelProviderName());
    logged = true;
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n'); cmd.trim();
    if (cmd.length() == 0) return;
    if (espfetch.check(cmd)) return;
    if (cmd == "url") logger.info("Tunnel: %s", tunnelURL().c_str());
    if (cmd == "ip")  logger.info("Last IP: %s", tunnelLastIP().c_str());
  }
}

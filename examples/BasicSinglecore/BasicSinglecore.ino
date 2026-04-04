/*
 * BasicSinglecore.ino — ESP8266 (single-core, cooperative)
 *
 * The tunnel runs cooperatively via tunnelLoop().
 * You MUST call tunnelLoop() in your loop() function.
 *
 * Self-hosted (default):
 *   #include <esp32tunnel.h>
 *   tunnelSetup(SELFHOST, "https://server.com/my-device");
 *
 * Localtunnel:
 *   #include <esp32tunnel.h>
 *   tunnelSetup(LOCALTUNNEL, "my-subdomain");
 *
 * Note: tunnelSetup() and tunnelLoop() also work on ESP32.
 *       This example is written for ESP8266 but is cross-platform compatible.
 *
 * Requires: ESPAsyncWebServer, esp32-tunnel, espfetch
 *
 * Serial commands:
 *   /espfetch  — system info (chip, WiFi, heap, etc.)
 *   url        — print tunnel URL
 *   ip         — print last visitor IP
 */

#include <ESPAsyncWebServer.h>
#include <esp32tunnel.h>
#include <esp32tunnel_testpage.h>
#include <espfetch.h>

// ── WiFi ─────────────────────────────────────────────────────
#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASS "YOUR_PASS"

// ── Provider (pick one) ──────────────────────────────────────
#define TUNNEL_SERVER "https://esp32-tunnel.onrender.com/my-device"

// ─────────────────────────────────────────────────────────────

AsyncWebServer server(80);

void setup() {
  rtosSerial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  logger.info("WiFi: %s", WiFi.localIP().toString().c_str());

  // MARK: Local routes (direct WiFi access via AsyncWebServer)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "text/html", TUN_TEST_HTML);
  });

  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "application/json",
      "{\"pong\":true,\"heap\":" + String(ESP.getFreeHeap()) +
      ",\"uptime\":" + String(millis() / 1000) + "}");
  });

  server.begin();

  // MARK: Start tunnel
  tunnelSetup(SELFHOST, TUNNEL_SERVER);
  // Or localtunnel: tunnelSetup(LOCALTUNNEL, "my-esp32");
}

void loop() {
  // MARK: Required — drives the tunnel on ESP8266 (no-op on ESP32)
  tunnelLoop(true);

  // Tunnel status logging
  static bool logged = false;
  if (!logged && tunnelReady()) {
    logger.info("Tunnel ready: %s (%s)", tunnelURL().c_str(), tunnelProviderName());
    logged = true;
  }

  // Serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n'); cmd.trim();
    if (cmd.length() == 0) return;
    if (espfetch.check(cmd)) return;  // /espfetch — system info
    if (cmd == "url") logger.info("Tunnel: %s", tunnelURL().c_str());
    if (cmd == "ip")  logger.info("Last IP: %s", tunnelLastIP().c_str());
  }
}

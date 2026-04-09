/*
 * Localtunnel.ino — Free HTTPS URL via localtunnel.me (no server needed)
 *
 * localtunnel provides free, random or named HTTPS subdomains:
 *   https://my-esp32.loca.lt
 *
 * No relay server required — the ESP32 connects directly to loca.lt
 * and receives forwarded TCP connections.
 *
 * Modes:
 *   TUN_FLEX   (default) — falls back to random subdomain if taken
 *   TUN_STRICT           — fails if the requested subdomain is taken
 *
 * Public URL:  https://<subdomain>.loca.lt
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

// Requested subdomain (https://my-esp32.loca.lt)
const char *SUBDOMAIN = "my-esp32";
// ─────────────────────────────────────────────────────────────

AsyncWebServer server(80);

void setup() {
  rtosSerial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  logger.info("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) delay(500);
  logger.info("WiFi: %s", WiFi.localIP().toString().c_str());

  // Local web server routes (served through the tunnel)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "text/html", TUN_TEST_HTML);
  });

  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "application/json",
      "{\"pong\":true,\"heap\":" + String(ESP.getFreeHeap()) +
      ",\"uptime\":" + String(millis() / 1000) + "}");
  });

  server.begin();

  // Start the tunnel — use TUN_STRICT to fail if subdomain is taken
  tunnelSetup(LOCALTUNNEL, SUBDOMAIN);
}

void loop() {
  tunnelLoop(true);

  static bool logged = false;
  if (!logged && tunnelReady()) {
    logger.info("Tunnel live: %s (%s)", tunnelURL().c_str(), tunnelProviderName());
    logged = true;
  }

  // Serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n'); cmd.trim();
    if (cmd.length() == 0) return;
    if (espfetch.check(cmd)) return;
    if (cmd == "url") logger.info("Tunnel: %s", tunnelURL().c_str());
    if (cmd == "ip")  logger.info("Last IP: %s", tunnelLastIP().c_str());
  }
}

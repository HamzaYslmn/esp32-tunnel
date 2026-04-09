/*
 * Bore.ino — Free public TCP tunnel via bore.pub
 *
 * bore.pub gives your ESP32 a public URL with NO login, NO account,
 * NO companion app. Just raw TCP tunneling.
 *
 * Public URL: http://bore.pub:PORT  (PORT assigned by server)
 * Self-hosted: `bore server` on your VPS → tunnelSetup(BORE, "your-vps.com")
 *
 * Board: ESP32 or ESP8266
 * Depends: espfetch, esp-rtosSerial, ESP Async WebServer
 */

#include <ESPAsyncWebServer.h>
#include <esp32tunnel.h>
#include <esp32tunnel_testpage.h>
#include <espfetch.h>
#include <rtosSerial.h>

// ── Configuration ────────────────────────────────────────────
const char *WIFI_SSID = "YOUR_SSID";
const char *WIFI_PASS = "YOUR_PASS";
// ─────────────────────────────────────────────────────────────

AsyncWebServer server(80);

void setup() {
  rtosSerial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  logger.info("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) delay(500);
  logger.info("WiFi: %s", WiFi.localIP().toString().c_str());

  // Local web server
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "text/html", TUN_TEST_HTML);
  });

  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "application/json",
      "{\"pong\":true,\"heap\":" + String(ESP.getFreeHeap()) +
      ",\"uptime\":" + String(millis() / 1000) + "}");
  });

  server.begin();

  // MARK: Start bore tunnel — free, no login, no account
  tunnelSetup(BORE);
  // Self-hosted bore server:
  // tunnelSetup(BORE, "your-server.com");
}

void loop() {
  tunnelLoop();

  static bool logged = false;
  if (!logged && tunnelReady()) {
    logger.info("Public URL: %s (%s)", tunnelURL().c_str(), tunnelProviderName());
    logged = true;
  }

  // Serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n'); cmd.trim();
    if (cmd.length() == 0) return;
    if (espfetch.check(cmd)) return;
    if (cmd == "url") logger.info("URL: %s", tunnelURL().c_str());
    if (cmd == "ip")  logger.info("Last IP: %s", tunnelLastIP().c_str());
  }
}

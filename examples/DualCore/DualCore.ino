/*
 * DualCore.ino — Run tunnel on a dedicated FreeRTOS core (ESP32 only)
 *
 * On ESP32, the tunnel can run in a FreeRTOS task on core 1, leaving
 * the main loop() completely free for your application logic.
 *
 * This is the recommended pattern for ESP32 when you need the main
 * loop for sensors, displays, motor control, etc.
 *
 * Note: ESP8266 is single-core — use tunnelLoop() in loop() instead.
 *
 * Works with all providers: SELFHOST, LOCALTUNNEL
 *
 * Board: ESP32 (dual-core only)
 */

#include <ESPAsyncWebServer.h>
#include <esp32tunnel.h>
#include <esp32tunnel_testpage.h>
#include <espfetch.h>
#include <rtosSerial.h>

// ── Configuration ────────────────────────────────────────────
const char *WIFI_SSID = "YOUR_SSID";
const char *WIFI_PASS = "YOUR_PASS";

const char *TUNNEL_SERVER = "https://esp32-tunnel-waa0.onrender.com/my-device";
// ─────────────────────────────────────────────────────────────

AsyncWebServer server(80);

// FreeRTOS task — tunnel runs on core 1
void tunnelTask(void *) {
  tunnelSetup(SELFHOST, TUNNEL_SERVER);

  // Or localtunnel:
  // tunnelSetup(LOCALTUNNEL, "my-esp32");

  while (true) {
    tunnelLoop(true);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  rtosSerial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  logger.info("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) delay(500);
  logger.info("WiFi: %s", WiFi.localIP().toString().c_str());

  // Local web server routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "text/html", TUN_TEST_HTML);
  });

  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "application/json",
      "{\"pong\":true,\"heap\":" + String(ESP.getFreeHeap()) +
      ",\"uptime\":" + String(millis() / 1000) + "}");
  });

  server.begin();

  // Launch tunnel on core 1 (8 KB stack, priority 3)
  xTaskCreatePinnedToCore(tunnelTask, "tunnel", 8192, nullptr, 3, nullptr, 1);
}

void loop() {
  // Main loop is free — tunnel runs independently on core 1

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

  // Your application logic here — sensors, displays, etc.
  delay(100);
}

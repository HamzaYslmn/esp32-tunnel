/*
 * DualCore.ino — Free main loop() while the tunnel runs itself (ESP32)
 *
 * On ESP32 the library runs the tunnel in its own FreeRTOS task
 * automatically — you do NOT call tunnelLoop() in loop(). Your loop()
 * is free for sensors, displays, motor control, etc.
 *
 * Tune the tunnel task with build flags if needed:
 *   -DTUN_TASK_CORE=0   (default 1)   -DTUN_TASK_PRIO=2   -DTUN_TASK_STACK=10240
 *
 * Note: ESP8266 has no FreeRTOS task — there you must call tunnelLoop()
 * in loop() (see the Localtunnel/SelfHosted examples).
 *
 * Board: ESP32
 */

#include <ESPAsyncWebServer.h>
#include <esp32tunnel.h>
#include <esp32tunnel_testpage.h>
#include <espfetch.h>
#include <rtosSerial.h>

const char *WIFI_SSID = "YOUR_SSID";
const char *WIFI_PASS = "YOUR_PASS";
const char *TUNNEL_SERVER = "https://esp32-tunnel-waa0.onrender.com/my-device";

AsyncWebServer server(80);

void setup() {
  rtosSerial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  logger.info("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) delay(500);
  logger.info("WiFi: %s", WiFi.localIP().toString().c_str());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "text/html", TUN_TEST_HTML);
  });
  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "application/json",
      "{\"pong\":true,\"heap\":" + String(ESP.getFreeHeap()) +
      ",\"uptime\":" + String(millis() / 1000) + "}");
  });
  server.begin();

  tunnelLog(true);
  tunnelSetup(SELFHOST, TUNNEL_SERVER);   // tunnel now runs on its own core
}

void loop() {
  // Main loop is free — the tunnel runs independently.
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

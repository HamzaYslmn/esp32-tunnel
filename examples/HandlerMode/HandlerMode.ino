/*
 * HandlerMode.ino — Direct request handling without AsyncWebServer
 *
 * Instead of proxying requests to a local HTTP server, the tunnel
 * calls your handler function directly. This is lighter weight and
 * avoids the ESPAsyncWebServer dependency for simple use cases.
 *
 * The handler receives the HTTP method and path, and returns the
 * response body as a String. Return an empty string for 404.
 *
 * Works with all providers: SELFHOST, LOCALTUNNEL
 *
 * Board: ESP32 or ESP8266
 */

#include <esp32tunnel.h>
#include <espfetch.h>
#include <rtosSerial.h>

// ── Configuration ────────────────────────────────────────────
const char *WIFI_SSID = "YOUR_SSID";
const char *WIFI_PASS = "YOUR_PASS";

const char *TUNNEL_SERVER = "https://esp32-tunnel-waa0.onrender.com/my-device";
// ─────────────────────────────────────────────────────────────

// Request handler — called for every incoming request
String handleRequest(const String &method, const String &path) {
  if (path == "/" || path == "/index.html") {
    return "<h1>ESP32 Tunnel</h1>"
           "<p>Handler mode — no web server needed.</p>"
           "<p>Heap: " + String(ESP.getFreeHeap()) + " bytes</p>"
           "<p><a href=\"/ping\">/ping</a></p>";
  }

  if (path == "/ping") {
    return "{\"pong\":true,\"heap\":" + String(ESP.getFreeHeap()) +
           ",\"uptime\":" + String(millis() / 1000) + "}";
  }

  return "";  // 404
}

void setup() {
  rtosSerial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  logger.info("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) delay(500);
  logger.info("WiFi: %s", WiFi.localIP().toString().c_str());

  // No AsyncWebServer needed — handler receives requests directly
  tunnelSetup(SELFHOST, handleRequest, TUNNEL_SERVER);
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

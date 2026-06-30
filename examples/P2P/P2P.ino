/*
 * P2P.ino — WebRTC DataChannel P2P over the self-hosted tunnel
 *
 * Goal: take traffic OFF your relay server. The server only brokers the
 * WebRTC handshake (a few hundred bytes, once); after that the browser talks
 * straight to this device. If P2P can't be established (symmetric NAT, no
 * engine), the browser automatically falls back to the normal relay.
 *
 * Browser side — served by your tunnel server at /p2p.js:
 *   <script src="https://myserver.com/p2p.js"></script>
 *   const dev = espTunnel('https://myserver.com', 'my-device');
 *   const r = await dev.fetch('/status');   // r.via === 'p2p' or 'relay'
 *
 * THE HEAVY PART — the WebRTC engine — is NOT in this library. ESP32 WebRTC
 * (ICE + DTLS + SCTP DataChannel) means an external component, e.g.:
 *   libpeer:  https://github.com/sepfy/libpeer   (ESP-IDF / PlatformIO)
 * This sketch wires the SIGNALING; you drop the engine into p2pAnswer() and
 * its DataChannel onmessage handler. Until you do, p2pAnswer() returns "" and
 * everything keeps working over the relay.
 *
 * Board: ESP32 (WebRTC engines need ESP32-class RAM/flash)
 */

#include <ESPAsyncWebServer.h>
#include <esp32tunnel.h>

const char *WIFI_SSID    = "YOUR_SSID";
const char *WIFI_PASS    = "YOUR_PASS";
const char *TUNNEL_SERVER = "https://myserver.com/my-device";

AsyncWebServer server(80);

// ── P2P signaling hook ───────────────────────────────────────
// Called when a browser wants a direct connection. Feed `offer` to your
// WebRTC engine, return its SDP answer. Return "" to decline (-> relay).
//
// Inside the engine's DataChannel onmessage, mirror p2p.js's wire protocol:
//   in : {"i":<id>,"m":<method>,"p":<path>,"b":<body>}
//   out: {"i":<id>,"s":<status>,"b":<body>,"t":<contentType>}
// Route those to your local server the same way the relay does.
String p2pAnswer(const String &offer) {
  // ponytail: stub. Wire libpeer here. Empty answer => graceful relay fallback.
  (void)offer;
  return "";
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.printf("WiFi: %s\n", WiFi.localIP().toString().c_str());

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "application/json",
      "{\"heap\":" + String(ESP.getFreeHeap()) + "}");
  });
  server.begin();

  tunnelP2P(p2pAnswer);              // enable P2P signaling
  tunnelLog(true);                   // optional access logging
  tunnelSetup(SELFHOST, TUNNEL_SERVER);   // runs in its own task on ESP32

  // Secure by default: an access key is auto-generated (persisted in NVS) and
  // required on every request via the X-Tunnel-Key header (the dashboard and
  // p2p.js send it for you). For an open device instead, call tunnelPublic()
  // before tunnelSetup(); for a fixed key, tunnelSetup(SELFHOST, server, "pass").
  delay(500);
  Serial.printf("Access key: %s\n", tunnelKey());
}

void loop() {
  vTaskDelete(NULL);   // nothing to do here — free the loop stack
}

// openport.pw provider — internal, included by esp32tunnel.h. ESP32 only.
// A free permanent TCP port, no account: the device registers once (id + token kept in NVS),
// asks the API where to connect, then speaks rathole (bincode, little-endian) over plain TCP:
//   control: -> Hello(0, v1, sha256(id))   <- Hello(nonce)   -> sha256(token + nonce)   <- Ack 0
//            <- 4-byte commands: 0 = open a data channel, 1 = heartbeat (every 30 s)
//   data:    -> Hello(1, v1, session)      <- 0 = start, then raw bytes both ways
// Idle data channels must stay open: the server hands visitors to any channel it was given.

#ifndef ESP32TUNNEL_OPENPORT_H
#define ESP32TUNNEL_OPENPORT_H
#ifdef ESP32

#include <mbedtls/sha256.h>

#ifndef OPENPORT_POOL
#define OPENPORT_POOL 2          // idle data channels kept ready for visitors
#endif
#ifndef OPENPORT_HEARTBEAT_MS
#define OPENPORT_HEARTBEAT_MS 600000UL   // 10 min; server expires idle sessions after 30 min
#endif

static struct : _TunBase {
  String host, cid, tok;
  int ctrlPort;
  int owed;                      // data channels the server asked for, not opened yet
  unsigned long lastCmd, lastHb;
  uint8_t session[32];
} _op;

static WiFiClient _opCtrl, _opLocal, _opData[OPENPORT_POOL];

// HTTPS POST to https://openport.pw/api/<path>; the JSON reply, or "" on failure.
static String _opApi(const char *path, const String &json, int *httpCode = nullptr) {
  if (httpCode) *httpCode = 0;
  WiFiClientSecure tls;
  if (!_tlsConnect(tls, "openport.pw", 443)) return "";
  tls.printf("POST /api/%s HTTP/1.1\r\nHost: openport.pw\r\nContent-Type: application/json\r\n"
             "Content-Length: %d\r\nConnection: close\r\n\r\n%s", path, (int)json.length(), json.c_str());
  char line[128];
  int code = 0;
  if (_readLineBuf(tls, line, sizeof line, _CONNECT_MS) > 0) {
    const char *sp = strchr(line, ' ');
    if (sp) code = atoi(sp + 1);
  }
  if (httpCode) *httpCode = code;
  bool ok = (code >= 200 && code < 300);
  while (_readLineBuf(tls, line, sizeof line, _DRAIN_TIMEOUT) > 0) {}
  String body;
  for (unsigned long t = millis(); (tls.connected() || tls.available()) && millis() - t < 5000 && body.length() < 1024;) {
    int c = tls.read();
    if (c >= 0) body += (char)c; else _DELAY(1);
  }
  tls.stop();
  return ok ? body : "";
}

static bool _opReadExact(WiFiClient &c, uint8_t *buf, int n) {
  for (unsigned long t = millis(); n > 0 && c.connected() && millis() - t < _CONNECT_MS;) {
    int r = c.read(buf, n);
    if (r > 0) { buf += r; n -= r; } else _DELAY(1);
  }
  return n == 0;
}

// rathole Hello: u32 variant, u8 protocol version 1, 32-byte digest
static void _opHello(WiFiClient &c, uint8_t variant, const uint8_t *digest) {
  uint8_t p[37] = {variant, 0, 0, 0, 1};
  memcpy(p + 5, digest, 32);
  c.write(p, sizeof p);
}

static void _opForget() {                // credentials rejected: register again next time
  _op.cid = _op.tok = "";
  Preferences p;
  if (p.begin("esp32tunnel", false)) { p.remove("op_id"); p.remove("op_tok"); p.end(); }
}

static String _opCredJson() {
  return "{\"client_id\":\"" + _op.cid + "\",\"token\":\"" + _op.tok + "\"}";
}

static void _opPumpCtrl() {
  uint8_t cmd[4];
  while (_opCtrl.available() >= 4 && _opCtrl.read(cmd, 4) == 4) {
    if (cmd[0] == 0) _op.owed++;                 // CreateDataChannel; 1 = heartbeat
    _op.lastCmd = millis();
  }
}

static bool _opInit() {
  if (WiFi.status() != WL_CONNECTED) return false;
  Preferences p;
  if (!_op.cid.length() && p.begin("esp32tunnel", true)) {
    _op.cid = p.getString("op_id", ""); _op.tok = p.getString("op_tok", ""); p.end();
  }
  if (!_op.cid.length()) {               // first boot: claim a free permanent port
    String r = _opApi("register", "{}");
    _op.cid = _jStr(r, "client_id"); _op.tok = _jStr(r, "token");
    if (!_op.cid.length() || !_op.tok.length()) return false;
    if (p.begin("esp32tunnel", false)) { p.putString("op_id", _op.cid); p.putString("op_tok", _op.tok); p.end(); }
  }
  int code = 0;
  String r = _opApi("connect", "{\"client_id\":\"" + _op.cid + "\",\"token\":\"" + _op.tok +
                               "\",\"protocol\":\"tcp\",\"local_port\":" + String(TUN_PORT) + "}", &code);
  if (code == 401) { _opForget(); return false; }
  if (_jStr(r, "status") != "ok") return false;
  _op.host = _jStr(r, "server_host");
  if (!_op.host.length()) _op.host = "openport.pw";
  _op.ctrlPort = _jInt(r, "control_port");
  int publicPort = _jInt(r, "remote_port");
  if (_op.ctrlPort <= 0 || publicPort <= 0) return false;

  if (!_tcpConnectHost(_opCtrl, _op.host.c_str(), _op.ctrlPort)) return false;
  _setKeepAlive(_opCtrl);
  uint8_t digest[32], hello[37], ack[4];
  mbedtls_sha256((const uint8_t *)_op.cid.c_str(), _op.cid.length(), digest, 0);
  _opHello(_opCtrl, 0, digest);
  if (!_opReadExact(_opCtrl, hello, sizeof hello)) { _opCtrl.stop(); return false; }
  mbedtls_sha256_context h;                      // session key = sha256(token + nonce)
  mbedtls_sha256_init(&h);
  mbedtls_sha256_starts(&h, 0);
  mbedtls_sha256_update(&h, (const uint8_t *)_op.tok.c_str(), _op.tok.length());
  mbedtls_sha256_update(&h, hello + 5, 32);
  mbedtls_sha256_finish(&h, _op.session);
  mbedtls_sha256_free(&h);
  _opCtrl.write(_op.session, 32);
  if (!_opReadExact(_opCtrl, ack, sizeof ack) || ack[0] != 0) { _opCtrl.stop(); return false; }

  _op.owed = 0;
  _op.lastCmd = _op.lastHb = millis();
  _op.url = "http://" + _op.host + ":" + String(publicPort);
  _op.ready = true;
  return true;
}

static bool _opServe() {
  if (WiFi.status() != WL_CONNECTED || !_opCtrl.connected()) return false;
  _opPumpCtrl();
  if ((unsigned long)(millis() - _op.lastCmd) > 45000UL) return false;   // missed heartbeats: reconnect

  if ((unsigned long)(millis() - _op.lastHb) >= OPENPORT_HEARTBEAT_MS) {
    int code = 0;
    String r = _opApi("heartbeat", _opCredJson(), &code);
    if (code == 401) { _opForget(); return false; }
    if (_jStr(r, "status") == "ok") _op.lastHb = millis();
    else if (OPENPORT_HEARTBEAT_MS > 60000UL) _op.lastHb = millis() - (OPENPORT_HEARTBEAT_MS - 60000UL);
    else _op.lastHb = millis();
  }

  for (int i = 0; i < OPENPORT_POOL; i++) {
    WiFiClient &d = _opData[i];
    if (!d.connected()) {                        // keep the pool topped up
      if (_op.owed > 0 && _tcpConnectHost(d, _op.host.c_str(), _op.ctrlPort)) {
        _op.owed--;
        d.setNoDelay(true);
        _opHello(d, 1, _op.session);
      }
      continue;
    }
    uint8_t cmd[4];
    if (d.available() < 4 || d.read(cmd, 4) != 4) continue;
    if (cmd[0] != 0) { d.stop(); continue; }    // only TCP forwarding
    if (_tcpConnect(_opLocal, IPAddress(127, 0, 0, 1), TUN_PORT)) {
      _opLocal.setNoDelay(true);
      _tunPipe(d, _opLocal);                     // a visitor: bytes both ways until done
      _opPumpCtrl();                             // drain control commands received during pipe
    } else d.stop();
  }
  return true;
}

// MARK: Internal API (dispatched from esp32tunnel.h)

static void _opBegin() {
  if (_op.started) return;
  _tunBegin(_op);
}

static void _opDrop() {
  _opCtrl.stop(); _opLocal.stop();
  for (auto &d : _opData) d.stop();
}

static void _opLoop() { _tunStep(_op, 2000, 60000, _opInit, _opServe, _opDrop); }

static void _opStop() {
  bool wasReady = _op.ready;
  _opDrop();
  if (wasReady && WiFi.status() == WL_CONNECTED && _op.cid.length() && _op.tok.length()) {
    _opApi("release", _opCredJson());
  }
  _tunEnd(_op);
}

#endif
#endif

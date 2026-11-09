// Self-hosted relay provider — internal, included by esp32tunnel.h.
//
// One task owns the WebSocket and never blocks in write() while it isn't reading:
// a writer that stops reading can deadlock lwIP (the relay's ACKs ride on data that
// gets refused once our RX mailbox is full). So every byte read lands in _shRx and is
// parsed only as complete frames, and writes go out only when select() says they fit.

#ifndef ESP32TUNNEL_SELFHOSTED_H
#define ESP32TUNNEL_SELFHOSTED_H

static WiFiClient _shWsPlain, _shLocal;
static WiFiClientSecure *_shWsTLS = nullptr;

static struct : _TunBase {
  TunnelHandler handler;
  String host, id;
  WiFiClient *ws;
  bool useTLS;
  int shPort;
} _sh;

static String _shRx;                    // bytes from the relay, not yet parsed
static unsigned long _shLastRx, _shLastPing;

// MARK: Socket I/O

// Sleep in select() until the socket is readable/writable, or `ms` passes.
static bool _shWait(bool write, int ms) {
  int fd = _sh.ws->fd();
  if (fd < 0) return false;
  fd_set s; FD_ZERO(&s); FD_SET(fd, &s);
  timeval tv = {ms / 1000, (ms % 1000) * 1000};
  return select(fd + 1, write ? nullptr : &s, write ? &s : nullptr, nullptr, &tv) > 0;
}

// Move everything readable into _shRx. false = connection gone or inbox overflow.
static bool _shPump() {
  static uint8_t buf[512];
  int n;
  while ((n = _sh.ws->available()) > 0 && (n = _sh.ws->read(buf, min(n, (int)sizeof buf))) > 0) {
    _shRx.concat((const char *)buf, n);
    _shLastRx = millis();
  }
  return _sh.ws->connected() && _shRx.length() <= (unsigned)_MAX_BODY_SIZE * 2 + 1024;
}

// Write all of b, ≤1 KB per call, only when it fits; keep reading meanwhile.
static bool _shSend(const uint8_t *b, size_t n) {
  unsigned long progress = millis();
  while (n) {
    if (millis() - progress > 30000 || !_shPump()) return false;
    if (!_shWait(true, 50)) continue;
    int w = _sh.ws->write(b, min(n, (size_t)1024));
    if (w <= 0) return false;
    b += w; n -= w; progress = millis();
  }
  return true;
}

// MARK: WebSocket frames (client frames must be masked — a zero mask is a no-op)

static bool _shFrame(uint8_t op, size_t len) {
  uint8_t h[14]; int n = 0;
  h[n++] = 0x80 | op;
  if (len < 126) h[n++] = 0x80 | len;
  else if (len <= 0xFFFF) { h[n++] = 0x80 | 126; h[n++] = len >> 8; h[n++] = len; }
  else { h[n++] = 0x80 | 127; for (int i = 7; i >= 0; i--) h[n++] = i < 4 ? len >> (8 * i) : 0; }
  memset(h + n, 0, 4);
  return _shSend(h, n + 4);
}

// Take one complete frame off _shRx; false = not all here yet.
static bool _shFrameIn(uint8_t &op, String &payload) {
  const uint8_t *p = (const uint8_t *)_shRx.c_str();
  size_t n = _shRx.length(), h = 2, len;
  if (n < 2) return false;
  len = p[1] & 0x7F;
  if (len == 126) { if (n < 4) return false; len = p[2] << 8 | p[3]; h = 4; }
  else if (len == 127) { if (n < 10) return false; len = 0; for (int i = 6; i < 10; i++) len = len << 8 | p[i]; h = 10; }
  const uint8_t *mask = (p[1] & 0x80) ? p + h : nullptr;
  if (mask) h += 4;
  if (n < h + len) return false;
  op = p[0] & 0x0F;
  payload = _shRx.substring(h, h + len);
  if (mask) for (size_t i = 0; i < len; i++) payload[i] ^= mask[i & 3];
  _shRx.remove(0, h + len);
  return true;
}

static int _escLen(const String &s) {
  int n = 0;
  for (unsigned i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    n += (c == '"' || c == '\\') ? 2 : c < 0x20 ? 6 : 1;
  }
  return n;
}

// JSON-escape s straight onto the socket — no escaped copy in RAM.
static bool _shSendEscaped(const String &s) {
  static char buf[1024];
  int pos = 0;
  for (unsigned i = 0; i < s.length(); i++) {
    if (pos > (int)sizeof buf - 6) { if (!_shSend((uint8_t *)buf, pos)) return false; pos = 0; }
    char c = s[i];
    if (c == '"' || c == '\\') { buf[pos++] = '\\'; buf[pos++] = c; }
    else if ((uint8_t)c < 0x20) pos += snprintf(buf + pos, 7, "\\u%04x", (uint8_t)c);
    else buf[pos++] = c;
  }
  return _shSend((uint8_t *)buf, pos);
}

// {"id":…,"status":…,"body":"…","type":"…"} as one text frame
static void _shRespond(const String &rid, int code, const String &body, const String &type) {
  char pre[80];
  int plen = snprintf(pre, sizeof pre, "{\"id\":\"%s\",\"status\":%d,\"body\":\"", rid.c_str(), code);
  int len = plen + _escLen(body) + 10 + _escLen(type) + 2;   // + ","type":"  + "}
  _shFrame(0x1, len) && _shSend((uint8_t *)pre, plen) && _shSendEscaped(body) &&
    _shSend((const uint8_t *)"\",\"type\":\"", 10) && _shSendEscaped(type) &&
    _shSend((const uint8_t *)"\"}", 2);
}

// MARK: Requests

// One reply from the sketch: its handler (handler mode) or its web server on TUN_PORT
// (proxy mode). Shared by every WebSocket provider.
static void _tunFetch(const String &method, const String &path, const String &ip,
                      const String &reqBody, const String &ct, const String &rawMsg,
                      int &code, String &body, String &type) {
  if (_sh.handler) {
    _tunType = _TUN_DEFAULT_TYPE;
    body = _sh.handler(method, path);
    code = 200; type = _tunType;
    if (!body.length()) { code = 404; body = "Not Found"; }
  } else {
    code = 502; type = "text/plain";
    if (_shLocal.connected()) _shLocal.stop();
    if (!_openLocal(_shLocal, method, path, ip, reqBody, ct, rawMsg)) body = "Local server unreachable";
    else if (!_waitLocal(_shLocal)) { code = 504; body = "Local timeout"; }
    else {
      int len;
      _parseLocalResponse(_shLocal, code, type, len);
      // ponytail: one reply per request — an endless SSE stream would hold the tunnel
      // until TUN_MAX_BODY fills. A non-200 also stops EventSource's reconnect loop.
      if (type.startsWith("text/event-stream")) {
        code = 501; type = "text/plain"; body = "Server-Sent Events don't pass the tunnel; poll with fetch()";
      } else if (len > _MAX_BODY_SIZE) {         // say so instead of sending a truncated page
        code = 502; type = "text/plain"; body = "Response larger than TUN_MAX_BODY";
      } else body = _readLocalBody(_shLocal, len);
    }
    _shLocal.stop();
  }
  _tunLog(ip, method, path, code);
}

static void _shRequest(const String &msg) {
  String rid = _jStr(msg, "id"), method = _jStr(msg, "method");
  String path = _jStr(msg, "path"), ip = _jStr(msg, "ip");
  if (!rid.length() || !method.length()) return;
  if (!path.length()) path = "/";
  if (path.indexOf("..") >= 0) return _shRespond(rid, 400, "Bad path", "text/plain");
  String reqBody, ct;
  if (method == "POST" || method == "PUT" || method == "PATCH") {
    reqBody = _jStr(msg, "body");
    ct      = _jStr(msg, "ct");
  }
  _sh.lastIP = ip;
  int code; String body, type;
  _tunFetch(method, path, ip, reqBody, ct, msg, code, body, type);
  _shRespond(rid, code, body, type);
}

// MARK: Connect + serve

// "https://host:port/device-id" — scheme picks TLS, port defaults to 443/80, id to "esp32"
static void _shParseOption(String s) {
  _sh.useTLS = s.startsWith("https://");
  if (s.indexOf("://") >= 0) s = s.substring(s.indexOf("://") + 3);
  int slash = s.indexOf('/');
  String hostPort = slash < 0 ? s : s.substring(0, slash);
  _sh.id = slash < 0 ? String("esp32") : s.substring(slash + 1);
  int colon = hostPort.indexOf(':');
  _sh.host = colon < 0 ? hostPort : hostPort.substring(0, colon);
  _sh.shPort = colon < 0 ? (_sh.useTLS ? 443 : 80) : hostPort.substring(colon + 1).toInt();
}

static bool _shConnect(const String &path, const String &hdrs = "") {
  if (_sh.useTLS && !_shWsTLS) { _shWsTLS = new WiFiClientSecure(); _shWsTLS->setInsecure(); }
  _sh.ws = _sh.useTLS ? _shWsTLS : &_shWsPlain;
  if (!_tcpConnectHost(*_sh.ws, _sh.host.c_str(), _sh.shPort)) return false;
  String host = _sh.host;
  if (_sh.shPort != (_sh.useTLS ? 443 : 80)) host += ":" + String(_sh.shPort);
  _sh.ws->printf("GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n%s\r\n",
                 path.c_str(), host.c_str(), hdrs.c_str());
  char line[128];
  _readLineBuf(*_sh.ws, line, sizeof line, _CONNECT_MS);
  if (!strstr(line, " 101")) { _sh.ws->stop(); return false; }
  while (_readLineBuf(*_sh.ws, line, sizeof line, _DRAIN_TIMEOUT) > 0) {}
  _setKeepAlive(*_sh.ws);
  _shRx = "";
  _shLastRx = _shLastPing = millis();
  return true;
}

static bool _shInit() {
  if (WiFi.status() != WL_CONNECTED) return false;
#if TUN_MDNS
  static bool mdnsUp = false;           // http://<id>.local for direct LAN access
  if (!mdnsUp && MDNS.begin(_sh.id.c_str())) { MDNS.addService("http", "tcp", TUN_PORT); mdnsUp = true; }
#endif
  if (_sh.useTLS && time(nullptr) < 100000) {   // TLS needs a sane clock; sync once
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    for (unsigned long t0 = millis(); time(nullptr) < 100000 && millis() - t0 < 5000;) _DELAY(100);
  }
  // The key rides on the handshake (a header, so it stays out of access logs)
  // so the relay can reject id hijacks.
  String q = String(TUN_WS_PATH) + "?id=" + _sh.id + "&max=" + String(_MAX_BODY_SIZE);
  if (!_shConnect(q, _tunKey.length() ? "X-Tunnel-Key: " + _tunKey + "\r\n" : "")) return false;

  if (_tunRouteCount > 0) {             // per-route passwords, sent once
    String cfg = "{\"type\":\"config\",\"routes\":[";
    for (int i = 0; i < _tunRouteCount; i++) {
      if (i) cfg += ",";
      cfg += "{\"path\":\"" + String(_tunRoutes[i].path) + "\"";
      if (_tunRoutes[i].password) cfg += ",\"password\":\"" + String(_tunRoutes[i].password) + "\"";
      cfg += "}";
    }
    cfg += "]}";
    if (!(_shFrame(0x1, cfg.length()) && _shSend((const uint8_t *)cfg.c_str(), cfg.length()))) return false;
  }

  _sh.url = String(_sh.useTLS ? "https://" : "http://") + _sh.host;
  if (_sh.shPort != (_sh.useTLS ? 443 : 80)) _sh.url += ":" + String(_sh.shPort);
  _sh.url += "/" + _sh.id;
  _sh.ready = true;
  return true;
}

// One step of a WebSocket session: sleep until data, answer pings, hand text frames to onText.
static bool _wsServe(void (*onText)(const String &)) {
  if (!_sh.ws->available()) _shWait(false, 10);   // the task sleeps here until relay data
  // Relay pings every 20 s and pongs ours, so 60 s of silence = dead link.
  if (!_shPump() || millis() - _shLastRx > 60000) return false;
  if (millis() - _shLastPing > 25000) {
    _shLastPing = millis();
    if (!_shFrame(0x9, 0)) return false;
  }
  uint8_t op; String msg;
  while (_shFrameIn(op, msg)) {
    if (op == 0x8) return false;                  // relay closed
    if (op == 0x9 && !(_shFrame(0xA, msg.length()) && _shSend((const uint8_t *)msg.c_str(), msg.length())))
      return false;
    if (op == 0x1) onText(msg);
  }
  return true;
}

// Text heartbeat: proxies like Render's answer WS pings themselves, so the relay
// only notices a dead device when these stop.
static bool _shServe() {
  static unsigned long beat;
  if (millis() - beat > 20000) {
    beat = millis();
    static const char hb[] = "{\"type\":\"ping\"}";
    if (!(_shFrame(0x1, sizeof hb - 1) && _shSend((const uint8_t *)hb, sizeof hb - 1))) return false;
  }
  return _wsServe(_shRequest);
}

// MARK: Internal API (dispatched from esp32tunnel.h)

static void _shBegin(TunnelHandler handler, const char *server) {
  if (_sh.started) return;
  _sh.handler = handler;
  _sh.ws = &_shWsPlain;
  _tunBegin(_sh);
  _shParseOption(server ? server : "");
}

static void _shDrop() { _sh.ws->stop(); }

static void _shLoop() { _tunStep(_sh, 1000, 60000, _shInit, _shServe, _shDrop); }

static void _shStop() {
  _sh.ws->stop();
  if (_shWsTLS) { delete _shWsTLS; _shWsTLS = nullptr; }
  _sh.ws = &_shWsPlain;
  _sh.useTLS = false;
  _tunEnd(_sh);
}

#endif

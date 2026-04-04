/*
 * esp32tunnel_selfhosted.h — Self-hosted relay provider
 * Internal — included by esp32tunnel.h, do not include directly.
 */

#ifndef ESP32TUNNEL_SELFHOSTED_H
#define ESP32TUNNEL_SELFHOSTED_H

// ---------------------------------------------------------------------------
// MARK: State
// ---------------------------------------------------------------------------

static WiFiClient _shWsPlain;
static WiFiClient _shLocal;
static WiFiClientSecure *_shWsTLS = nullptr;

static struct {
  TunnelHandler handler;
  String option, host, id, url, lastIP;
  WiFiClient *ws;
  bool useTLS;
  int shPort;
  _Phase phase;
  unsigned long waitUntil;
  bool ready, stop, started;
} _sh = {};

// =========================================================================
// MARK: WebSocket framing (plain WiFiClient)
// =========================================================================

static String _wsRecv() {
  uint8_t b0, b1;
  int opcode;
  for (;;) {
    if (!_sh.ws->connected() || !_sh.ws->available()) return "";
    b0 = _sh.ws->read(); b1 = _sh.ws->read();
    opcode = b0 & 0x0F;
    if (opcode == 0x08) return "";
    if (opcode == 0x09) {
      // MARK: Stream pong — echo ping payload (cap at 125 bytes per WS spec)
      int pLen = b1 & 0x7F;
      if (pLen >= 126) { for (int k = 0; k < pLen; k++) _sh.ws->read(); continue; }
      uint8_t hdr[6] = {0x8A, (uint8_t)(pLen | 0x80), 0, 0, 0, 0};
      _sh.ws->write(hdr, 6);
      for (int k = 0; k < pLen; k++) _sh.ws->write((uint8_t)_sh.ws->read());
      continue;
    }
    if (opcode == 0x0A) {
      for (int k = 0, n = b1 & 0x7F; k < n; k++) _sh.ws->read();
      continue;
    }
    break;
  }

  bool masked = b1 & 0x80;
  int len = b1 & 0x7F;
  if (len == 126) {
    len = (_sh.ws->read() << 8) | _sh.ws->read();
  } else if (len == 127) {
    uint32_t hi = 0, lo = 0;
    for (int i = 0; i < 4; i++) hi = (hi << 8) | _sh.ws->read();
    for (int i = 0; i < 4; i++) lo = (lo << 8) | _sh.ws->read();
    len = (hi == 0 && lo <= (uint32_t)_MAX_BODY_SIZE) ? (int)lo : 0;
  }
  uint8_t mask[4] = {0};
  if (masked) for (int i = 0; i < 4; i++) mask[i] = _sh.ws->read();
  if (len > _MAX_BODY_SIZE || len <= 0) return "";

  String out; out.reserve(len);
  uint8_t buf[256]; int got = 0;
  unsigned long rt = millis();
  while (got < len && _sh.ws->connected() && millis() - rt < 30000) {
    int avail = _sh.ws->available();
    if (!avail) { _DELAY(1); continue; }
    int chunk = min(avail, min((int)sizeof(buf), len - got));
    int r = _sh.ws->read(buf, chunk);
    if (r > 0) {
      if (masked) for (int i = 0; i < r; i++) buf[i] ^= mask[(got + i) & 3];
      out.concat((const char*)buf, r);
      got += r; rt = millis();
    }
  }
  return out;
}

static bool _wsConnect() {
  if (_sh.useTLS) {
    if (!_shWsTLS) {
      _shWsTLS = new WiFiClientSecure();
      _shWsTLS->setInsecure();
    }
    _sh.ws = _shWsTLS;
  } else {
    _sh.ws = &_shWsPlain;
  }
  if (!_tcpConnectHost(*_sh.ws, _sh.host.c_str(), _sh.shPort)) return false;
  _sh.ws->printf(
    "GET %s?id=%s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
    TUN_WS_PATH, _sh.id.c_str(), _sh.host.c_str()
  );
  unsigned long t0 = millis();
  while (!_sh.ws->available() && millis() - t0 < _CONNECT_MS) _DELAY(10);
  if (!_sh.ws->available()) { _sh.ws->stop(); return false; }
  char line[128];
  _readLineBuf(*_sh.ws, line, sizeof(line), _CONNECT_MS);
  if (!strstr(line, "101")) { _sh.ws->stop(); return false; }
  while (_sh.ws->connected()) {
    int n = _readLineBuf(*_sh.ws, line, sizeof(line), _DRAIN_TIMEOUT);
    if (n == 0) break;
  }
  _setKeepAlive(*_sh.ws);
  return true;
}

// =========================================================================
// MARK: Response helpers — streaming to avoid double-buffering
// =========================================================================

// MARK: Count escaped length without allocating a String
static int _escapeLen(const char *s, int len) {
  int out = 0;
  for (int i = 0; i < len; i++) {
    char c = s[i];
    if (c == '"' || c == '\\') out += 2;
    else if ((uint8_t)c < 0x20) out += 6;  // \uXXXX
    else out += 1;
  }
  return out;
}

// MARK: Write escaped chars directly to WS (128-byte buffer, no String alloc)
static void _wsWriteEscaped(const char *s, int len) {
  char buf[128]; int pos = 0;
  for (int i = 0; i < len; i++) {
    char c = s[i];
    int need;
    if (c == '"' || c == '\\') need = 2;
    else if ((uint8_t)c < 0x20) need = 6;
    else need = 1;
    if (pos + need > (int)sizeof(buf)) {
      _sh.ws->write((uint8_t*)buf, pos); pos = 0;
    }
    if (c == '"')       { buf[pos++] = '\\'; buf[pos++] = '"'; }
    else if (c == '\\') { buf[pos++] = '\\'; buf[pos++] = '\\'; }
    else if ((uint8_t)c < 0x20) {
      pos += snprintf(buf + pos, 7, "\\u%04x", (uint8_t)c);
    }
    else { buf[pos++] = c; }
  }
  if (pos > 0) _sh.ws->write((uint8_t*)buf, pos);
}

// MARK: Write WS frame header (payload length known upfront)
static bool _wsBeginFrame(int payloadLen) {
  if (!_sh.ws->connected()) return false;
  uint8_t hdr[14]; int hlen = 0;
  hdr[hlen++] = 0x81;
  if (payloadLen < 126) {
    hdr[hlen++] = payloadLen | 0x80;
  } else if (payloadLen <= 65535) {
    hdr[hlen++] = 126 | 0x80;
    hdr[hlen++] = (payloadLen >> 8) & 0xFF;
    hdr[hlen++] = payloadLen & 0xFF;
  } else {
    hdr[hlen++] = 127 | 0x80;
    hdr[hlen++] = 0; hdr[hlen++] = 0; hdr[hlen++] = 0; hdr[hlen++] = 0;
    hdr[hlen++] = (payloadLen >> 24) & 0xFF;
    hdr[hlen++] = (payloadLen >> 16) & 0xFF;
    hdr[hlen++] = (payloadLen >> 8) & 0xFF;
    hdr[hlen++] = payloadLen & 0xFF;
  }
  uint8_t mask[4] = {0, 0, 0, 0};
  memcpy(hdr + hlen, mask, 4); hlen += 4;
  _sh.ws->write(hdr, hlen);
  return true;
}

// MARK: Stream JSON response — avoids _escape() String allocation
static void _wsSendResponse(const String &rid, int code, const String &body, const String &type) {
  // Build prefix into stack buffer (rid max ~36, code max 3 digits)
  char prefix[80];
  int plen = snprintf(prefix, sizeof(prefix),
    "{\"id\":\"%s\",\"status\":%d,\"body\":\"", rid.c_str(), code);
  if (plen >= (int)sizeof(prefix)) plen = sizeof(prefix) - 1;

  int bodyEscLen = _escapeLen(body.c_str(), body.length());
  int typeEscLen = _escapeLen(type.c_str(), type.length());
  // ","type":"  = 10 chars,  "}  = 2 chars
  int totalLen = plen + bodyEscLen + 10 + typeEscLen + 2;

  if (!_wsBeginFrame(totalLen)) return;
  _sh.ws->write((uint8_t*)prefix, plen);
  _wsWriteEscaped(body.c_str(), body.length());
  _sh.ws->write((const uint8_t*)"\",\"type\":\"", 10);
  _wsWriteEscaped(type.c_str(), type.length());
  _sh.ws->write((const uint8_t*)"\"}", 2);
  _sh.ws->flush();
}

static void _wsSendError(const String &rid, int code, const char *text) {
  // Errors are small — no streaming needed
  char buf[128];
  int len = snprintf(buf, sizeof(buf),
    "{\"id\":\"%s\",\"status\":%d,\"body\":\"%s\"}",
    rid.c_str(), code, text);
  if (len <= 0 || len >= (int)sizeof(buf)) return;
  if (!_wsBeginFrame(len)) return;
  _sh.ws->write((uint8_t*)buf, len);
  _sh.ws->flush();
}

// =========================================================================
// MARK: Request handlers
// =========================================================================

static void _shLocalForward(const String &rid, const String &method,
                            const String &path, const String &ip,
                            const String &reqBody, const String &ct,
                            const String &rawMsg) {
  if (_shLocal.connected()) _shLocal.stop();
  if (!_openLocal(_shLocal, method, path, ip, reqBody, ct, rawMsg)) {
    _wsSendError(rid, 502, "Local server unreachable");
    return;
  }
  if (!_waitLocal(_shLocal)) {
    _shLocal.stop();
    _wsSendError(rid, 504, "Local timeout");
    return;
  }
  int code; String respCT; int respCL;
  _parseLocalResponse(_shLocal, code, respCT, respCL);
  String body = _readLocalBody(_shLocal, respCL);
  _shLocal.stop();
  _wsSendResponse(rid, code, body, respCT);
  if (_tunAutoLog) _TUN_LOG(ip.c_str(), method.c_str(), path.c_str(), code);
}

static void _shHandlerForward(const String &rid, const String &method,
                               const String &path) {
  String body = _sh.handler(method, path);
  int code = body.length() ? 200 : 404;
  if (!body.length()) body = "Not Found";
  _wsSendResponse(rid, code, body, "text/html; charset=utf-8");
  if (_tunAutoLog) _TUN_LOG(_sh.lastIP.c_str(), method.c_str(), path.c_str(), code);
}

// =========================================================================
// MARK: Parse option — "host:port/device-id"
// =========================================================================

static void _shParseOption() {
  const char *s = _sh.option.c_str();
  // MARK: Auto-detect TLS from URL scheme
  if (strncmp(s, "https://", 8) == 0) { _sh.useTLS = true; s += 8; }
  else if (strncmp(s, "http://", 7) == 0) { _sh.useTLS = false; s += 7; }
  else { _sh.useTLS = false; }

  const char *slash = strchr(s, '/');
  const char *colon = strchr(s, ':');
  int hostEnd = slash ? (int)(slash - s) : (int)strlen(s);
  if (colon && colon < s + hostEnd) {
    int hLen = (int)(colon - s);
    char hBuf[128]; if (hLen >= (int)sizeof(hBuf)) hLen = sizeof(hBuf) - 1;
    memcpy(hBuf, s, hLen); hBuf[hLen] = 0;
    _sh.host = hBuf;
    _sh.shPort = atoi(colon + 1);
  } else {
    char hBuf[128]; int hLen = hostEnd; if (hLen >= (int)sizeof(hBuf)) hLen = sizeof(hBuf) - 1;
    memcpy(hBuf, s, hLen); hBuf[hLen] = 0;
    _sh.host = hBuf;
    _sh.shPort = _sh.useTLS ? 443 : 80;
  }
  _sh.id = slash ? String(slash + 1) : "esp32";
  _sh.option = "";
}

// =========================================================================
// MARK: Init + serve
// =========================================================================

static bool _shInit() {
  if (WiFi.status() != WL_CONNECTED) return false;

  // MARK: NTP time sync — once only, skip if RTC already valid
  if (_sh.useTLS) {
    time_t now = 0;
    time(&now);
    if (now < 100000) {
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      unsigned long t0 = millis();
      while (now < 100000 && millis() - t0 < 5000) {
        time(&now);
        _DELAY(100);
      }
    }
  }

  if (!_wsConnect()) return false;

  // MARK: Send route config if any routes defined
  if (_tunRouteCount > 0) {
    String cfg = "{\"type\":\"config\",\"routes\":[";
    for (int i = 0; i < _tunRouteCount; i++) {
      if (i > 0) cfg += ",";
      cfg += "{\"path\":\""; cfg += _tunRoutes[i].path; cfg += "\"";
      if (_tunRoutes[i].password) {
        cfg += ",\"password\":\""; cfg += _tunRoutes[i].password; cfg += "\"";
      }
      cfg += "}";
    }
    cfg += "]}";
    int cfgLen = cfg.length();
    if (_wsBeginFrame(cfgLen)) {
      _sh.ws->write((const uint8_t*)cfg.c_str(), cfgLen);
      _sh.ws->flush();
    }
  }

  _sh.url = _sh.useTLS ? "https://" : "http://";
  _sh.url += _sh.host;
  if (_sh.shPort != (_sh.useTLS ? 443 : 80)) {
    _sh.url += ":"; _sh.url += _sh.shPort;
  }
  _sh.url += "/";
  _sh.url += _sh.id;
  _sh.ready = true;
  return true;
}

// MARK: Single-pass request parse — avoids 4× _jStr full scans
static void _shParseMsg(const String &j, String &rid, String &method,
                        String &path, String &ip) {
  const char *s = j.c_str();
  int len = j.length(), found = 0;
  const char *keys[] = {"id", "method", "path", "ip"};
  const int klens[] = {2, 6, 4, 2};
  String *vals[] = {&rid, &method, &path, &ip};
  for (int i = 0; i < len && found < 4; i++) {
    if (s[i] != '"') continue;
    for (int k = 0; k < 4; k++) {
      if (vals[k]->length()) continue;
      if (i + klens[k] + 2 > len) continue;
      if (memcmp(s + i + 1, keys[k], klens[k]) || s[i + 1 + klens[k]] != '"') continue;
      int vi = i + klens[k] + 2;
      while (vi < len && (s[vi] == ':' || s[vi] == ' ')) vi++;
      if (vi >= len || s[vi] != '"') break;
      vi++;
      char buf[128]; int p = 0;
      while (vi < len && s[vi] != '"') {
        char c;
        if (s[vi] == '\\' && vi + 1 < len) {
          char e = s[++vi]; c = (e=='n')?'\n':(e=='r')?'\r':(e=='t')?'\t':e;
        } else c = s[vi];
        if (p < 127) buf[p++] = c;
        vi++;
      }
      buf[p] = 0; *vals[k] = String(buf);
      i = vi; found++;
      break;
    }
  }
}

static unsigned long _shLastPing = 0;

static bool _shServe() {
  if (!_sh.ws->connected()) return false;

  // MARK: Periodic WS ping — keeps connection alive through proxies
  if (millis() - _shLastPing > 25000) {
    _shLastPing = millis();
    uint8_t ping[6] = {0x89, 0x80, 0, 0, 0, 0};
    _sh.ws->write(ping, 6);
  }

  String msg = _wsRecv();
  if (!msg.length()) return _sh.ws->connected();

  String rid, method, path, ip;
  _shParseMsg(msg, rid, method, path, ip);
  if (!rid.length() || !method.length()) return true;
  if (!path.length()) path = "/";
  if (path.indexOf("..") >= 0) { _wsSendError(rid, 400, "Bad path"); return true; }
  String body, ct;
  if (method == "POST" || method == "PUT" || method == "PATCH") {
    body = _jStr(msg, "body");
    ct   = _jStr(msg, "ct");
  }
  _sh.lastIP = ip;

  if (_sh.handler) _shHandlerForward(rid, method, path);
  else             _shLocalForward(rid, method, path, ip, body, ct, msg);
  return true;
}

// =========================================================================
// MARK: Internal API (called by dispatch in esp32tunnel.h)
// =========================================================================

static void _shBegin(TunnelHandler handler, const char *server) {
  if (_sh.started) return;
  _sh.handler = handler;
  _sh.option  = server ? server : "";
  _sh.stop    = false;
  _sh.ready   = false;
  _sh.started = true;
  _sh.url     = "(connecting...)";
  _sh.phase   = _PH_INIT;
  _sh.ws      = &_shWsPlain;
  _shParseOption();
}

static unsigned long _shBackoff = 1000;

static void _shLoop() {
  if (!_sh.started || _sh.stop) return;
  switch (_sh.phase) {
    case _PH_IDLE: break;
    case _PH_INIT:
      if (_shInit()) { _sh.phase = _PH_SERVE; _shBackoff = 1000; }
      else { _sh.phase = _PH_WAIT; _sh.waitUntil = millis() + _shBackoff; _shBackoff = min(_shBackoff * 2, 60000UL); }
      break;
    case _PH_SERVE:
      if (!_shServe()) {
        _sh.ws->stop(); _sh.ready = false;
        if (!TUN_RECONNECT) { _sh.phase = _PH_IDLE; _sh.started = false; }
        else { _sh.phase = _PH_WAIT; _sh.waitUntil = millis() + _shBackoff; _shBackoff = min(_shBackoff * 2, 60000UL); }
      }
      break;
    case _PH_WAIT:
      if (millis() >= _sh.waitUntil) _sh.phase = _PH_INIT;
      break;
  }
}

static void _shStop() {
  _sh.stop = true;
  if (_sh.ws) _sh.ws->stop();
  if (_sh.useTLS && _shWsTLS) {
    delete _shWsTLS;
    _shWsTLS = nullptr;
  }
  _sh.ws      = &_shWsPlain;
  _sh.useTLS  = false;
  _sh.ready   = false;
  _sh.started = false;
  _sh.phase   = _PH_IDLE;
  _sh.url     = "(stopped)";
}

#endif

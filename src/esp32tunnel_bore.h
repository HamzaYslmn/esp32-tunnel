/*
 * esp32tunnel_bore.h — bore.pub TCP tunnel provider
 * Internal — included by esp32tunnel.h, do not include directly.
 *
 * Protocol (bore v0.5+):
 *   Control port 7835, JSON messages delimited by \0
 *   Client sends {"Hello":0}\0       → server replies {"Hello":PORT}\0
 *   Server sends {"Connection":"UUID"}\0  → client opens new TCP to 7835,
 *     sends {"Accept":"UUID"}\0, then proxies raw TCP ↔ local port.
 *
 * Public server: bore.pub (free, no login)
 * Self-hosted:   `bore server` on your VPS
 */

#ifndef ESP32TUNNEL_BORE_H
#define ESP32TUNNEL_BORE_H

// ---------------------------------------------------------------------------
// MARK: Config
// ---------------------------------------------------------------------------

#ifndef BORE_CONTROL_PORT
#define BORE_CONTROL_PORT 7835
#endif

#ifndef BORE_MAX_PROXY
#define BORE_MAX_PROXY 4
#endif

// ---------------------------------------------------------------------------
// MARK: State
// ---------------------------------------------------------------------------

static struct {
  String host, url, lastIP;
  uint16_t remotePort;
  uint16_t localPort;
  _Phase phase;
  unsigned long waitUntil;
  bool ready, stop, started;
} _bore = {};

static WiFiClient _boreCtrl;
static WiFiClient _boreProxy[BORE_MAX_PROXY];
static WiFiClient _boreLocal[BORE_MAX_PROXY];

// ---------------------------------------------------------------------------
// MARK: Null-delimited JSON read — reads until \0 or timeout
// ---------------------------------------------------------------------------

static String _boreRecvMsg(WiFiClient &c, int timeoutMs = 5000) {
  String msg; msg.reserve(128);
  unsigned long t = millis();
  while (millis() - t < (unsigned long)timeoutMs) {
    if (!c.connected()) return "";
    if (!c.available()) { _DELAY(1); continue; }
    char ch = c.read();
    if (ch == '\0') return msg;
    msg += ch;
    if (msg.length() > 256) return ""; // safety cap
  }
  return "";
}

// MARK: Send null-terminated JSON
static void _boreSendMsg(WiFiClient &c, const String &msg) {
  c.print(msg);
  c.write((uint8_t)0);
}

// ---------------------------------------------------------------------------
// MARK: Parse server messages — extract tagged enum fields
// ---------------------------------------------------------------------------

// {"Hello":PORT}  → returns PORT
static int _boreParseHello(const String &msg) {
  int i = msg.indexOf("\"Hello\"");
  if (i < 0) return -1;
  i += 7; // skip past "Hello"
  while (i < (int)msg.length() && (msg[i] == ':' || msg[i] == ' ')) i++;
  return atoi(msg.c_str() + i);
}

// {"Connection":"UUID"}  → returns UUID
static String _boreParseConnection(const String &msg) {
  int i = msg.indexOf("\"Connection\"");
  if (i < 0) return "";
  i += 12; // skip past "Connection"
  while (i < (int)msg.length() && (msg[i] == ':' || msg[i] == ' ')) i++;
  if (i >= (int)msg.length() || msg[i] != '"') return "";
  i++;
  int start = i;
  while (i < (int)msg.length() && msg[i] != '"') i++;
  return msg.substring(start, i);
}

// {"Error":"MSG"}  → returns error text
static String _boreParseError(const String &msg) {
  int i = msg.indexOf("\"Error\"");
  if (i < 0) return "";
  i += 7;
  while (i < (int)msg.length() && (msg[i] == ':' || msg[i] == ' ')) i++;
  if (i >= (int)msg.length() || msg[i] != '"') return "";
  i++;
  int start = i;
  while (i < (int)msg.length() && msg[i] != '"') i++;
  return msg.substring(start, i);
}

// ---------------------------------------------------------------------------
// MARK: Proxy — bidirectional TCP copy
// ---------------------------------------------------------------------------

static void _boreProxyConn(WiFiClient &remote, WiFiClient &local) {
  uint8_t buf[512];
  unsigned long idle = millis();
  while (remote.connected() && local.connected() && millis() - idle < 30000) {
    bool activity = false;
    if (remote.available()) {
      int n = remote.read(buf, sizeof(buf));
      if (n > 0) { local.write(buf, n); activity = true; }
    }
    if (local.available()) {
      int n = local.read(buf, sizeof(buf));
      if (n > 0) { remote.write(buf, n); activity = true; }
    }
    if (activity) idle = millis();
    else _DELAY(1);
  }
  remote.stop();
  local.stop();
}

// MARK: Handle a single Connection message — open accept stream + proxy
static void _boreAccept(const String &uuid, int slot) {
  WiFiClient &proxy = _boreProxy[slot];
  WiFiClient &local = _boreLocal[slot];

  if (!_tcpConnectHost(proxy, _bore.host.c_str(), BORE_CONTROL_PORT)) return;
  _boreSendMsg(proxy, "{\"Accept\":\"" + uuid + "\"}");

  if (!_tcpConnect(local, IPAddress(127, 0, 0, 1), _bore.localPort)) {
    proxy.stop(); return;
  }
  local.setNoDelay(true);
  _boreProxyConn(proxy, local);
}

// ---------------------------------------------------------------------------
// MARK: Init + serve
// ---------------------------------------------------------------------------

static bool _boreInit() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (_boreCtrl.connected()) _boreCtrl.stop();

  if (!_tcpConnectHost(_boreCtrl, _bore.host.c_str(), BORE_CONTROL_PORT)) return false;
  _setKeepAlive(_boreCtrl);

  // MARK: Send Hello — request random port (0)
  _boreSendMsg(_boreCtrl, "{\"Hello\":0}");

  String resp = _boreRecvMsg(_boreCtrl, 10000);
  if (!resp.length()) { _boreCtrl.stop(); return false; }

  // Check for error
  String err = _boreParseError(resp);
  if (err.length()) { _boreCtrl.stop(); return false; }

  int port = _boreParseHello(resp);
  if (port <= 0) { _boreCtrl.stop(); return false; }

  _bore.remotePort = (uint16_t)port;
  _bore.url = "http://" + _bore.host + ":" + String(port);
  _bore.ready = true;
  return true;
}

static bool _boreServe() {
  if (!_boreCtrl.connected()) return false;

  // Non-blocking: check if control connection has data
  if (!_boreCtrl.available()) return true;

  String msg = _boreRecvMsg(_boreCtrl, 3000);
  if (!msg.length()) return _boreCtrl.connected();

  // Heartbeat — just ignore
  if (msg.indexOf("\"Heartbeat\"") >= 0) return true;

  // Connection request
  String uuid = _boreParseConnection(msg);
  if (!uuid.length()) return true;

  // Find a free proxy slot
  int slot = -1;
  for (int i = 0; i < BORE_MAX_PROXY; i++) {
    if (!_boreProxy[i].connected() && !_boreLocal[i].connected()) {
      slot = i; break;
    }
  }
  if (slot < 0) return true; // all slots busy, drop connection

  _boreAccept(uuid, slot);
  return true;
}

// ---------------------------------------------------------------------------
// MARK: Internal API (called by dispatch in esp32tunnel.h)
// ---------------------------------------------------------------------------

static void _boreBegin(const char *host, uint16_t localPort) {
  if (_bore.started) return;
  _bore.host      = (host && strlen(host) > 0) ? host : "bore.pub";
  _bore.localPort = localPort > 0 ? localPort : TUN_PORT;
  _bore.stop      = false;
  _bore.ready     = false;
  _bore.started   = true;
  _bore.url       = "(connecting...)";
  _bore.phase     = _PH_INIT;
}

static unsigned long _boreBackoff = 2000;

static void _boreLoop() {
  if (!_bore.started || _bore.stop) return;
  switch (_bore.phase) {
    case _PH_IDLE: break;
    case _PH_INIT:
      if (_boreInit()) { _bore.phase = _PH_SERVE; _boreBackoff = 2000; }
      else {
        _bore.phase = _PH_WAIT;
        _bore.waitUntil = millis() + _boreBackoff;
        _boreBackoff = min(_boreBackoff * 2, 60000UL);
      }
      break;
    case _PH_SERVE:
      if (!_boreServe()) {
        _boreCtrl.stop(); _bore.ready = false;
        if (!TUN_RECONNECT) { _bore.phase = _PH_IDLE; _bore.started = false; }
        else { _bore.phase = _PH_WAIT; _bore.waitUntil = millis() + _boreBackoff; _boreBackoff = min(_boreBackoff * 2, 60000UL); }
      }
      break;
    case _PH_WAIT:
      if (millis() >= _bore.waitUntil) _bore.phase = _PH_INIT;
      break;
  }
}

static void _boreStop() {
  _bore.stop = true;
  _boreCtrl.stop();
  for (int i = 0; i < BORE_MAX_PROXY; i++) {
    _boreProxy[i].stop();
    _boreLocal[i].stop();
  }
  _bore.ready   = false;
  _bore.started = false;
  _bore.phase   = _PH_IDLE;
  _bore.url     = "(stopped)";
}

#endif

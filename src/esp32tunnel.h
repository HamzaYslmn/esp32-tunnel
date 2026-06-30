/*
 * esp32tunnel.h — ESP32 tunnel library (single entry point)
 *
 * Usage:
 *   #include <esp32tunnel.h>
 *   tunnelSetup(SELFHOST, "https://myserver.com/my-device");
 *   tunnelSetup(SELFHOST, "http://myserver.local/my-device");
 *   tunnelSetup(LOCALTUNNEL, "my-esp32");
 *   tunnelSetup(BORE);                                // bore.pub random port
 *   tunnelSetup(BORE, "my-server.com");               // self-hosted bore
 */

#ifndef ESP32TUNNEL_H
#define ESP32TUNNEL_H

#include <Arduino.h>

#ifdef ESP32
  #include <WiFi.h>
  #include <lwip/sockets.h>
  #include <Preferences.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
#endif

#include <WiFiClientSecure.h>

static const char* _httpStatus(int c) {
  switch (c) {
    case 200: return "OK"; case 201: return "Created"; case 204: return "No Content";
    case 301: return "Moved"; case 302: return "Found"; case 304: return "Not Modified";
    case 400: return "Bad Request"; case 403: return "Forbidden"; case 404: return "Not Found";
    case 500: return "Internal Error"; case 502: return "Bad Gateway"; case 504: return "Timeout";
    default: return "";
  }
}

#ifdef ESP32
  #include <rtosSerial.h>
  #define _TUN_LOG(ip, m, p, s) rtosSerial.printf("INFO:     %s - \"%s %s HTTP/1.1\" %d %s\n", ip, m, p, s, _httpStatus(s))
#else
  #define _TUN_LOG(ip, m, p, s) Serial.printf("INFO:     %s - \"%s %s HTTP/1.1\" %d %s\n", ip, m, p, s, _httpStatus(s))
#endif

// ---------------------------------------------------------------------------
// Shared types & config
// ---------------------------------------------------------------------------

enum TunnelProvider { SELFHOST, LOCALTUNNEL, BORE };
enum TunnelMode { TUN_STRICT, TUN_FLEX };

typedef String (*TunnelHandler)(const String &method, const String &path);

// MARK: P2P (WebRTC) signaling hook — return SDP answer for the given offer,
// or "" to decline (browser then falls back to the relay). The WebRTC engine
// (DTLS+SCTP DataChannel, e.g. libpeer) lives in the sketch, not this header.
typedef String (*P2PSignalHandler)(const String &offerSdp);
static P2PSignalHandler _p2pHandler = nullptr;

static bool _tunAutoLog = false;

// MARK: Route-based access control
struct RouteConfig {
  const char *path;
  const char *password;   // nullptr = public
};

#ifndef TUN_MAX_ROUTES
#define TUN_MAX_ROUTES 8
#endif
static RouteConfig _tunRoutes[TUN_MAX_ROUTES];
static int _tunRouteCount = 0;

// MARK: Access key — secure by default. Auto-generated (ESP32 NVS, stable
// across reboots) unless the sketch sets its own or opts out. Required on the
// WS handshake (blocks id hijack) and on every visitor request (?key=/header).
static String _tunKey;            // "" = open access (insecure)
static bool _tunAutoAuth = true;

static String _tunMakeKey() {
#ifdef ESP32
  Preferences p;
  if (p.begin("esp32tunnel", false)) {
    String k = p.getString("key", "");
    if (k.length() < 24) {
      static const char hex[] = "0123456789abcdef";
      char buf[33];
      for (int i = 0; i < 32; i++) buf[i] = hex[esp_random() & 0xF];
      buf[32] = 0;
      k = buf;
      p.putString("key", k);
    }
    p.end();
    return k;
  }
#endif
  return "";   // ESP8266 / NVS unavailable: pass a password to tunnelSetup()
}

#ifndef TUN_PORT
#define TUN_PORT 80
#endif
#ifndef TUN_RECONNECT
#define TUN_RECONNECT 1
#endif
#ifndef TUN_WS_PATH
#define TUN_WS_PATH "/api/tunnel/ws"
#endif
#ifndef TUN_POOL
#define TUN_POOL 2
#endif
#ifndef TUN_STALE
#define TUN_STALE 30000
#endif
#ifndef TUN_REALLOC
#define TUN_REALLOC 12
#endif

// MARK: Background task (ESP32) — tunnelSetup() runs the tunnel in its own
// FreeRTOS task, so no tunnelLoop() in loop() is needed. Tune if required.
#ifndef TUN_TASK_STACK
#define TUN_TASK_STACK 8192      // proven enough for the WS + TLS path
#endif
#ifndef TUN_TASK_PRIO
#define TUN_TASK_PRIO 1
#endif
#ifndef TUN_TASK_CORE
#define TUN_TASK_CORE 1
#endif

// ---------------------------------------------------------------------------
// MARK: Platform abstraction
// ---------------------------------------------------------------------------

#ifdef ESP32
  #define _DELAY(ms) vTaskDelay(pdMS_TO_TICKS(ms))
#else
  #define _DELAY(ms) delay(ms)
#endif

static const int _CONNECT_MS   = 10000;
static const int _LOCAL_TIMEOUT = 5000;
static const int _DRAIN_TIMEOUT = 200;
static const int _HDR_TIMEOUT   = 3000;
static const int _MAX_HDR_SIZE  = 4096;
static const int _MAX_BODY_SIZE = 16384;
static const int _MAX_REQ_LINE  = 2048;

enum _Phase { _PH_IDLE, _PH_INIT, _PH_SERVE, _PH_WAIT };

static bool _tcpConnect(WiFiClient &c, IPAddress ip, uint16_t port) {
#ifdef ESP32
  return c.connect(ip, port, _LOCAL_TIMEOUT);
#else
  c.setTimeout(_LOCAL_TIMEOUT);
  return c.connect(ip, port);
#endif
}

static bool _tcpConnectHost(WiFiClient &c, const char *host, uint16_t port) {
#ifdef ESP32
  return c.connect(host, port, _CONNECT_MS);
#else
  c.setTimeout(_CONNECT_MS);
  return c.connect(host, port);
#endif
}

static String _chipID() {
  char buf[12];
#ifdef ESP32
  snprintf(buf, sizeof(buf), "%x", (uint32_t)ESP.getEfuseMac());
#else
  snprintf(buf, sizeof(buf), "%x", ESP.getChipId());
#endif
  return String(buf);
}

// ---------------------------------------------------------------------------
// MARK: JSON helpers
// ---------------------------------------------------------------------------

static int _jFind(const String &j, const char *key) {
  const char *s = j.c_str();
  int klen = strlen(key);
  const char *p = s;
  while ((p = strstr(p, key)) != nullptr) {
    if (p > s && *(p - 1) == '"' && *(p + klen) == '"') {
      int i = (p - s) + klen + 1;
      while (i < (int)j.length() && (s[i] == ':' || s[i] == ' ')) i++;
      return i;
    }
    p++;
  }
  return -1;
}

static String _jStr(const String &j, const char *key) {
  int i = _jFind(j, key);
  if (i < 0 || j[i] != '"') return "";
  i++;
  char stack[128]; int pos = 0; bool heap = false;
  String out;
  while (i < (int)j.length() && j[i] != '"') {
    char c;
    if (j[i] == '\\' && i + 1 < (int)j.length()) {
      char e = j[++i];
      c = (e == 'n') ? '\n' : (e == 'r') ? '\r' : (e == 't') ? '\t' : e;
    } else c = j[i];
    if (!heap && pos < 127) { stack[pos++] = c; }
    else {
      if (!heap) { heap = true; out.reserve(256); out.concat(stack, pos); }
      out += c;
    }
    i++;
  }
  if (heap) return out;
  stack[pos] = 0;
  return String(stack);
}

static int _jInt(const String &j, const char *key, int def = 0) {
  int i = _jFind(j, key);
  if (i < 0) return def;
  const char *s = j.c_str() + i;
  return atoi(s);
}

static void _writeHdrs(WiFiClient &local, const String &msg) {
  int i = _jFind(msg, "hdrs");
  if (i < 0 || msg[i] != '{') return;
  i++;
  const char *s = msg.c_str();
  int len = msg.length();
  while (i < len && s[i] != '}') {
    while (i < len && (s[i] == ' ' || s[i] == ',' || s[i] == '\n')) i++;
    if (i >= len || s[i] == '}') break;
    if (s[i] != '"') { i++; continue; }
    i++; int ks = i;
    while (i < len && s[i] != '"') i++;
    int ke = i; i++;
    while (i < len && (s[i] == ':' || s[i] == ' ')) i++;
    if (i >= len || s[i] != '"') continue;
    i++; int vs = i;
    while (i < len && s[i] != '"') { if (s[i] == '\\' && i + 1 < len) i += 2; else i++; }
    int ve = i; i++;
    if (ke > ks && ve > vs) {
      local.write((const uint8_t*)(s + ks), ke - ks);
      local.write((const uint8_t*)": ", 2);
      local.write((const uint8_t*)(s + vs), ve - vs);
      local.write((const uint8_t*)"\r\n", 2);
    }
  }
}

// ---------------------------------------------------------------------------
// MARK: Shared helpers
// ---------------------------------------------------------------------------

static void _setKeepAlive(WiFiClient &c) {
#ifdef ESP32
  int fd = c.fd(); if (fd < 0) return;
  int on = 1, idle = 10, intvl = 10, cnt = 3;
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,   &on,    sizeof(on));
#endif
  c.setNoDelay(true);
}

static bool _hdrMatch(const char *h, const char *prefix) {
  return strncasecmp(h, prefix, strlen(prefix)) == 0;
}

// MARK: Read line into caller buffer — zero heap allocation
static int _readLineBuf(WiFiClient &c, char *buf, int maxLen, int timeoutMs) {
  int pos = 0;
  unsigned long t = millis();
  while (pos < maxLen - 1 && millis() - t < (unsigned long)timeoutMs) {
    if (!c.available()) { _DELAY(1); continue; }
    char ch = c.read();
    if (ch == '\n') break;
    if (ch != '\r') buf[pos++] = ch;
  }
  buf[pos] = 0;
  return pos;
}

// ---------------------------------------------------------------------------
// MARK: Local HTTP proxy (forward to ESPAsyncWebServer on TUN_PORT)
// ---------------------------------------------------------------------------

static bool _openLocal(WiFiClient &local, const String &method, const String &path,
                       const String &ip, const String &body, const String &ct,
                       const String &msg = String()) {
  if (!_tcpConnect(local, IPAddress(127, 0, 0, 1), TUN_PORT)) return false;
  local.setNoDelay(true);
  local.printf("%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
               "X-Forwarded-For: %s\r\n", method.c_str(), path.c_str(), ip.c_str());
  _writeHdrs(local, msg);
  if (body.length()) {
    local.printf("Content-Type: %s\r\nContent-Length: %d\r\n",
                 ct.length() ? ct.c_str() : "text/plain", (int)body.length());
  }
  local.print("\r\n");
  if (body.length()) local.print(body);
  return true;
}

static bool _waitLocal(WiFiClient &local) {
  unsigned long t0 = millis();
  while (!local.available() && local.connected() && millis() - t0 < _LOCAL_TIMEOUT) _DELAY(1);
  return local.available();
}

static void _parseLocalResponse(WiFiClient &local, int &code, String &contentType, int &contentLen) {
  char line[256];
  _readLineBuf(local, line, sizeof(line), _LOCAL_TIMEOUT);
  code = 200;
  char *sp = strchr(line, ' ');
  if (sp) code = atoi(sp + 1);
  if (code == 0) code = 200;
  contentType = "text/html";
  contentLen = -1;
  int hdrBytes = 0;
  while (local.connected() && hdrBytes < _MAX_HDR_SIZE) {
    if (!local.available()) { _DELAY(1); continue; }
    int n = _readLineBuf(local, line, sizeof(line), _DRAIN_TIMEOUT);
    hdrBytes += n;
    if (n == 0) break;
    if (_hdrMatch(line, "content-type:"))   { contentType = String(line + 13); contentType.trim(); }
    if (_hdrMatch(line, "content-length:")) contentLen = atoi(line + 15);
  }
}

static String _readLocalBody(WiFiClient &local, int contentLen) {
  String body; body.reserve(contentLen > 0 ? min(contentLen, _MAX_BODY_SIZE) : 256);
  uint8_t buf[256]; int got = 0;
  int maxRead = (contentLen > 0) ? min(contentLen, _MAX_BODY_SIZE) : _MAX_BODY_SIZE;
  unsigned long bt = millis();
  while (got < maxRead && millis() - bt < _LOCAL_TIMEOUT) {
    if (local.available()) {
      int chunk = min(local.available(), min((int)sizeof(buf), maxRead - got));
      int r = local.read(buf, chunk);
      if (r > 0) { body.concat((const char*)buf, r); got += r; bt = millis(); }
    } else {
      if (!local.connected()) break;
      _DELAY(1);
    }
  }
  return body;
}

// ---------------------------------------------------------------------------
// MARK: Include providers
// ---------------------------------------------------------------------------

#include "esp32tunnel_selfhosted.h"
#include "esp32tunnel_localtunnel.h"
#include "esp32tunnel_bore.h"

// ---------------------------------------------------------------------------
// MARK: Active provider tracker
// ---------------------------------------------------------------------------

static TunnelProvider _tunProvider = SELFHOST;

// ---------------------------------------------------------------------------
// MARK: Public API — dispatch to active provider
// ---------------------------------------------------------------------------

// MARK: Drive the active provider once
static inline void _tunService() {
  if (_tunProvider == SELFHOST)      _shLoop();
  else if (_tunProvider == BORE)     _boreLoop();
  else                               _ltLoop();
}

// MARK: Background task — owns the tunnel so the sketch's loop() stays free
#ifdef ESP32
static TaskHandle_t _tunTask = nullptr;
static volatile bool _tunTaskRun = false;

static void _tunTaskFn(void *) {
  while (_tunTaskRun) { _tunService(); vTaskDelay(pdMS_TO_TICKS(10)); }
  _tunTask = nullptr;
  vTaskDelete(nullptr);            // clean self-exit (never killed mid-TLS)
}

static inline void _tunStartTask() {
  if (_tunTask) return;
  _tunTaskRun = true;
  xTaskCreatePinnedToCore(_tunTaskFn, "esp32tunnel", TUN_TASK_STACK,
                          nullptr, TUN_TASK_PRIO, &_tunTask, TUN_TASK_CORE);
}
#endif

// MARK: tunnelSetup overloads

inline void tunnelSetup(TunnelProvider p, TunnelHandler handler,
                        const char *option, TunnelMode mode = TUN_FLEX) {
  _tunProvider = p;
  // MARK: Secure by default (self-hosted) — generate an access key unless the
  // sketch set its own auth or opted out. Sent on the handshake; the server
  // then requires it on every visitor request (see tunnelKey()).
  if (p == SELFHOST && !_tunKey.length() && _tunAutoAuth && !_tunRouteCount)
    _tunKey = _tunMakeKey();
  if (p == SELFHOST)      _shBegin(handler, option);
  else if (p == BORE)     _boreBegin(option, TUN_PORT);
  else                    _ltBegin(handler, option, mode);
#ifdef ESP32
  _tunStartTask();                 // no tunnelLoop() needed on ESP32
#endif
}

inline void tunnelSetup(TunnelProvider p, const char *option) {
  tunnelSetup(p, (TunnelHandler)nullptr, option, TUN_FLEX);
}

inline void tunnelSetup(TunnelProvider p, TunnelHandler handler, const char *option) {
  tunnelSetup(p, handler, option, TUN_FLEX);
}

inline void tunnelSetup(TunnelProvider p, const char *option, TunnelMode mode) {
  tunnelSetup(p, (TunnelHandler)nullptr, option, mode);
}

inline void tunnelSetup(TunnelProvider p) {
  tunnelSetup(p, (TunnelHandler)nullptr, nullptr, TUN_FLEX);
}

// MARK: tunnelSetup with a custom password — whole tunnel uses this key
inline void tunnelSetup(TunnelProvider p, const char *option, const char *password) {
  _tunAutoAuth = false;
  _tunKey = password ? password : "";
  tunnelSetup(p, (TunnelHandler)nullptr, option, TUN_FLEX);
}

// MARK: tunnelSetup with RouteConfig — per-route authentication
template <size_t N>
inline void tunnelSetup(TunnelProvider p, const char *option, const RouteConfig (&routes)[N]) {
  _tunRouteCount = (int)(N > TUN_MAX_ROUTES ? TUN_MAX_ROUTES : N);
  for (int i = 0; i < _tunRouteCount; i++) _tunRoutes[i] = routes[i];
  tunnelSetup(p, (TunnelHandler)nullptr, option, TUN_FLEX);
}

// MARK: tunnelP2P — register WebRTC engine (self-hosted mode only). When set,
// the server brokers signaling and visitors connect peer-to-peer; your server
// only relays the handshake, not the traffic. Unset = relay as before.
inline void tunnelP2P(P2PSignalHandler handler) { _p2pHandler = handler; }

// MARK: Access control (call BEFORE tunnelSetup, self-hosted only)
// Default = auto-generated key. tunnelPublic() = open access (e.g. host a public
// website on an RPi). Custom password: tunnelSetup(SELFHOST, server, "mypass").
inline void tunnelPublic() { _tunAutoAuth = false; _tunKey = ""; }
inline const char* tunnelKey() { return _tunKey.c_str(); }

// MARK: tunnelLog — toggle access logging (replaces tunnelLoop(true))
inline void tunnelLog(bool enable = true) { _tunAutoLog = enable; }

// MARK: tunnelLoop — only needed on ESP8266 (no FreeRTOS task). On ESP32 the
// background task already drives the tunnel, so this is a safe no-op (calling
// it anyway won't double-drive the socket). Kept for backward compatibility.
inline void tunnelLoop(bool log = false) {
  _tunAutoLog = log;
#ifdef ESP32
  if (_tunTask) return;
#endif
  _tunService();
}

inline void tunnelStop() {
#ifdef ESP32
  // Ask the task to exit and wait (≤1s) so sockets aren't torn down under it.
  if (_tunTask && _tunTask != xTaskGetCurrentTaskHandle()) {
    _tunTaskRun = false;
    for (int i = 0; i < 200 && _tunTask; i++) _DELAY(5);
  }
#endif
  if (_tunProvider == SELFHOST)      _shStop();
  else if (_tunProvider == BORE)     _boreStop();
  else                               _ltStop();
}

// MARK: Status getters

inline String tunnelURL() {
  if (_tunProvider == SELFHOST) return _sh.url;
  if (_tunProvider == BORE)     return _bore.url;
  return _lt.url;
}

inline bool tunnelReady() {
  if (_tunProvider == SELFHOST) return _sh.ready;
  if (_tunProvider == BORE)     return _bore.ready;
  return _lt.ready;
}

inline String tunnelLastIP() {
  if (_tunProvider == SELFHOST) return _sh.lastIP;
  if (_tunProvider == BORE)     return _bore.lastIP;
  return _lt.lastIP;
}

inline const char* tunnelProviderName() {
  if (_tunProvider == SELFHOST) return "self-hosted";
  if (_tunProvider == BORE)     return "bore";
  return "localtunnel";
}

// MARK: Optional TLS cert verification (self-hosted provider only)
inline void tunnelCACert(const char *pem) {
  if (!_shWsTLS) {
    _shWsTLS = new WiFiClientSecure();
  }
  if (pem && strlen(pem) > 0) _shWsTLS->setCACert(pem);
  else _shWsTLS->setInsecure();
}

#endif

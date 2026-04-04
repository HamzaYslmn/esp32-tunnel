/*
 * esp32tunnel_testpage.h — optional built-in test page HTML
 *
 * Include this header to get the test page HTML.
 * Routing stays in your .ino file.
 *
 * Usage:
 *   #include <esp32tunnel_testpage.h>
 *
 *   String onRequest(const String &method, const String &path) {
 *     if (path == "/") return String(TUN_TEST_HTML);
 *     if (path == "/ping") return "{\"pong\":true}";
 *     return "";
 *   }
 */

#pragma once
#include <Arduino.h>

static const char TUN_TEST_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32</title>
<style>
body{margin:0;background:#0d1117;color:#c9d1d9;font:16px system-ui;
display:flex;align-items:center;justify-content:center;height:100vh;flex-direction:column;gap:16px}
a{color:#58a6ff;text-decoration:none}
#ms{font:48px monospace;color:#f0883e}
</style></head><body>
<a href="https://github.com/HamzaYslmn">by HamzaYslmn</a>
<div id="ms">---</div>
<script>
let b=window.location.pathname.replace(/\/?$/,'/');
async function p(){try{let t=performance.now();await fetch(b+'ping');
document.getElementById('ms').textContent=Math.round(performance.now()-t)+' ms'}
catch(e){document.getElementById('ms').textContent='--'}}
p();setInterval(p,5000);
</script></body></html>
)rawliteral";

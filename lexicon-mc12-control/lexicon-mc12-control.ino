#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>  // tzapu/WiFiManager
#include <ESPmDNS.h>      // mDNS responder

/*************************************************
 * Lexicon MC-12HD Web Controller (ESP32-C3)
 *
 * Features:
 *  - WiFi provisioning via tzapu WiFiManager captive portal (AP: "Lexicon")
 *  - mDNS hostname: "lexicon"  (http://lexicon.local)
 *  - Web UI: Volume +/-1 dB, Mute toggle
 *  - Web UI: Effect +/-
 *  - Collapsible Menu drawer: Menu/Left/Right(Select)/Up/Down + Power
 *  - Collapsible Debug drawer: HOST_WAKEUP, Sync Now, Reset WiFi
 *
 * UART: 19200 8O1 on GPIO20/21
 *************************************************/

/******************** DEBUG ********************/
#define DEBUG_SERIAL 1
#if DEBUG_SERIAL
  #define DBGFLN(fmt, ...) Serial.printf((fmt "\n"), ##__VA_ARGS__)
  #define DBGLN(x) Serial.println(x)
#else
  #define DBGFLN(...)
  #define DBGLN(x)
#endif

/******************** WIFI MANAGER ********************/
static const char* PORTAL_AP_NAME = "Lexicon";
static const char* PORTAL_AP_PASS = "";            // set 8+ chars to protect portal, or leave "" open
static const uint16_t PORTAL_TIMEOUT_SEC = 300;    // seconds

/******************** mDNS ********************/
static const char* MDNS_HOSTNAME = "lexicon";      // lexicon.local

/******************** GPIO ********************/
#define WIFI_LED_PIN 8   // active-low

/******************** UART ********************/
#define LEXICON_RX_PIN 20
#define LEXICON_TX_PIN 21

/******************** PROTOCOL BYTES ********************/
static const uint8_t SOP = 0xF1;
static const uint8_t EOP = 0xF2;

/******************** COMMANDS ********************/
static const uint8_t DC_FPD          = 0x03;
static const uint8_t HOST_WAKEUP     = 0x11;
static const uint8_t DC_ACK          = 0xE0;
static const uint8_t DC_NACK         = 0xE1;
static const uint8_t DC_CMD_SET_MUTE = 0x31;
static const uint8_t MC_CMD_IR       = 0x39;

static const uint8_t MC_CMD_GET_SYS_STATUS = 0x3E;
static const uint8_t MC_RESP_SYS_STATUS    = 0x94;

// Observed volume notify on your wire
static const uint8_t OBS_PARAM_NOTIFY = 0x05;
static const uint8_t OBS_VOL_PARAM_ID = 0xE0;

/******************** IR KEYCODES ********************/
static const uint8_t IR_VOL_INCR  = 0x17;
static const uint8_t IR_VOL_DECR  = 0x16;
static const uint8_t IR_MODE_INCR = 0x1A;
static const uint8_t IR_MODE_DECR = 0x1B;

// Menu/navigation keycodes
static const uint8_t IR_NAV_MENU   = 0x09;
static const uint8_t IR_NAV_LEFT   = 0x0A;
static const uint8_t IR_NAV_RIGHT  = 0x08; // Right / Select
static const uint8_t IR_NAV_UP     = 0x01;
static const uint8_t IR_NAV_DOWN   = 0x1D;
static const uint8_t IR_NAV_POWER  = 0x9A;

// Input selection keycodes
static const uint8_t IR_MAIN_DVD1  = 0x20;
static const uint8_t IR_MAIN_DVD2  = 0x21;
static const uint8_t IR_MAIN_LD    = 0x22;
static const uint8_t IR_MAIN_TV    = 0x23;
static const uint8_t IR_MAIN_SAT   = 0x24;
static const uint8_t IR_MAIN_VCR   = 0x25;
static const uint8_t IR_MAIN_CD    = 0x26;
static const uint8_t IR_MAIN_PVR   = 0x27;
static const uint8_t IR_MAIN_GAME  = 0x28;
static const uint8_t IR_MAIN_TAPE  = 0x29;
static const uint8_t IR_MAIN_TUNER = 0x2A;
static const uint8_t IR_MAIN_AUX   = 0x2B;

/******************** WEB ********************/
WebServer server(80);

/******************** STATE ********************/
struct State {
  bool wifiOk = false;
  bool hostAck = false;
  bool sysSynced = false;

  uint8_t line1Raw[21]{};
  uint8_t line2Raw[21]{};
  String line1;
  String line2;
  String line1Hex;
  String line2Hex;

  int8_t volDb = 0;
  bool volKnown = false;

  bool muted = false;
  bool muteKnown = false;

  uint8_t inputId = 0;
  uint8_t effectId = 0;

  uint8_t lastCmd = 0;
  uint8_t lastDll = 0;
  uint8_t lastApp = 0;

  unsigned long lastWakeRetryMs = 0;
  unsigned long lastSysQueryMs = 0;
} st;

/******************** HELPERS ********************/
static String hex2(uint8_t b) {
  char buf[3];
  snprintf(buf, sizeof(buf), "%02X", b);
  return String(buf);
}

static String raw21ToHex(const uint8_t* raw21) {
  String s;
  for (int i = 0; i < 21; i++) {
    s += hex2(raw21[i]);
    if (i != 20) s += ' ';
  }
  return s;
}

static void setWifiLed(bool connected) {
  digitalWrite(WIFI_LED_PIN, connected ? LOW : HIGH); // active-low
}

static void startMDNS() {
  if (!MDNS.begin(MDNS_HOSTNAME)) {
    DBGLN("[mDNS] Error starting mDNS responder");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  DBGFLN("[mDNS] Started: http://%s.local", MDNS_HOSTNAME);
}

/******************** LCD GLYPH RENDERING ********************/
static void appendGlyph(String& out, uint8_t c) {
  if (c >= 0x20 && c <= 0x7E) { out += (char)c; return; }

  // Dolby Digital logo: 0x80 0x81 0x82 => "DD "
  if (c == 0x80) { out += 'D'; return; }
  if (c == 0x81) { out += 'D'; return; }
  if (c == 0x82) { out += ' '; return; }

  // dts prefix: 0x83 0x84 0x85
  if (c == 0x83) { out += 'd'; return; }
  if (c == 0x84) { out += 't'; return; }
  if (c == 0x85) { out += 's'; return; }

  // THX: 0x86 0x87 0x88
  if (c == 0x86) { out += 'T'; return; }
  if (c == 0x87) { out += 'H'; return; }
  if (c == 0x88) { out += 'X'; return; }

  // L7: 0x89 0x8A
  if (c == 0x89) { out += 'L'; return; }
  if (c == 0x8A) { out += '7'; return; }

  // dtsES: 0x8B 0x8C
  if (c == 0x8B) { out += 'E'; return; }
  if (c == 0x8C) { out += 'S'; return; }

  // 5.1mc: 0x8D => .1
  if (c == 0x8D) { out += ".1"; return; }

  // Volume bar chars 0x8E-0x93
  if (c == 0x8E) { out += ' '; return; }
  if (c == 0x8F) { out += "|"; return; }
  if (c == 0x90) { out += "░"; return; }
  if (c == 0x91) { out += "▒"; return; }
  if (c == 0x92) { out += "▓"; return; }
  if (c == 0x93) { out += "█"; return; }

  // dtsNEO6: 0x94..0x98
  if (c == 0x94) { out += 'N'; return; }
  if (c == 0x95) { out += 'E'; return; }
  if (c == 0x96) { out += 'O'; return; }
  if (c == 0x97) { out += '6'; return; }
  if (c == 0x98) { out += ' '; return; }

  out += '□';
}

static String renderLine(const uint8_t* raw21) {
  String s;
  for (int i = 0; i < 21; i++) {
    uint8_t c = raw21[i];
    if (c == 0x00) break;
    appendGlyph(s, c);
  }
  return s;
}

/******************** LEXICON TX ********************/
static void lexSend(uint8_t cmd, const uint8_t* data, uint8_t appLen) {
  uint8_t dllCount = (uint8_t)(1 + 1 + appLen + 1);
  Serial1.write(SOP);
  Serial1.write(dllCount);
  Serial1.write(cmd);
  Serial1.write(appLen);
  for (uint8_t i = 0; i < appLen; i++) Serial1.write(data[i]);
  Serial1.write(EOP);
}

static void sendHostWakeup() { lexSend(HOST_WAKEUP, nullptr, 0); }

static void sendGetSysStatus() {
  lexSend(MC_CMD_GET_SYS_STATUS, nullptr, 0);
  st.lastSysQueryMs = millis();
  DBGLN("[SYS] GET_SYS_STATUS sent");
}

static void sendIr(uint8_t keyCode) {
  uint8_t k = keyCode;
  lexSend(MC_CMD_IR, &k, 1);
}

static void sendMute(bool on) {
  uint8_t val = on ? 2 : 0;
  lexSend(DC_CMD_SET_MUTE, &val, 1);
  st.muted = on;
  st.muteKnown = true;
}

/******************** RX FRAMER ********************/
static uint8_t rxBuf[300];
static int rxNeed = 0;
static int rxHave = 0;
static bool rxSync = false;

static void handleFPD(const uint8_t* app, uint8_t appLen) {
  if (appLen < 42) return;
  memcpy(st.line1Raw, app, 21);
  memcpy(st.line2Raw, app + 21, 21);
  st.line1Hex = raw21ToHex(st.line1Raw);
  st.line2Hex = raw21ToHex(st.line2Raw);
  st.line1 = renderLine(st.line1Raw);
  st.line2 = renderLine(st.line2Raw);
  DBGFLN("[FPD] RAW2=%s", st.line2Hex.c_str());
}

static void handleSysStatus(const uint8_t* app, uint8_t appLen) {
  if (appLen == 10) {
    st.volDb = (int8_t)app[0]; st.volKnown = true;
    st.inputId = app[1];
    st.effectId = app[2];
    st.muted = (app[5] != 0); st.muteKnown = true;
    st.sysSynced = true;
    return;
  }
  if (appLen >= 4) {
    st.volDb = (int8_t)app[0]; st.volKnown = true;
    st.muted = (app[1] != 0); st.muteKnown = true;
    st.inputId = app[2];
    st.effectId = app[3];
    st.sysSynced = true;
    return;
  }
}

static void processPacket(const uint8_t* p, int len) {
  if (len < 5) return;
  if (p[0] != SOP || p[len - 1] != EOP) return;

  uint8_t cmd = p[2];
  uint8_t appLen = p[3];
  const uint8_t* app = &p[4];

  st.lastCmd = cmd;
  st.lastDll = p[1];
  st.lastApp = appLen;

  if (cmd == DC_ACK) {
    if (appLen >= 1 && app[0] == HOST_WAKEUP) {
      st.hostAck = true;
      DBGLN("[ACK] HOST_WAKEUP");
    }
    return;
  }

  if (cmd == DC_NACK) return;

  if (cmd == DC_FPD) { handleFPD(app, appLen); return; }
  if (cmd == MC_RESP_SYS_STATUS) { handleSysStatus(app, appLen); return; }

  if (cmd == OBS_PARAM_NOTIFY && appLen >= 4 && app[0] == OBS_VOL_PARAM_ID) {
    st.volDb = (int8_t)app[3];
    st.volKnown = true;
    return;
  }
}

static void readLexicon() {
  while (Serial1.available()) {
    uint8_t b = Serial1.read();

    if (!rxSync) {
      if (b == SOP) {
        rxSync = true;
        rxHave = 0;
        rxNeed = 0;
        rxBuf[rxHave++] = b;
      }
      continue;
    }

    rxBuf[rxHave++] = b;

    if (rxHave == 2) {
      rxNeed = 2 + rxBuf[1];
      if (rxNeed > (int)sizeof(rxBuf)) rxSync = false;
    }

    if (rxNeed > 0 && rxHave >= rxNeed) {
      processPacket(rxBuf, rxNeed);
      rxSync = false;
    }

    if (rxHave >= (int)sizeof(rxBuf)) rxSync = false;
  }
}

/******************** WiFi provisioning ********************/
static void wifiProvision() {
  WiFi.mode(WIFI_STA);

  WiFiManager wm;
  wm.setDebugOutput(DEBUG_SERIAL);
  wm.setClass("invert");
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_SEC);

  bool ok;
  if (PORTAL_AP_PASS && PORTAL_AP_PASS[0] != '\0') ok = wm.autoConnect(PORTAL_AP_NAME, PORTAL_AP_PASS);
  else ok = wm.autoConnect(PORTAL_AP_NAME);

  if (!ok) {
    DBGLN("[WiFi] Failed to connect or portal timed out. Rebooting...");
    delay(1000);
    ESP.restart();
  }

  st.wifiOk = true;
  setWifiLed(true);
  DBGFLN("[WiFi] Connected. SSID='%s' IP=%s RSSI=%d", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

static void resetWifiAndReboot() {
  WiFiManager wm;
  wm.resetSettings();
  delay(250);
  ESP.restart();
}

/******************** WEB UI ********************/
//was background:#021; color:#6fff6f -- green on green
//blue on blue background:#001a33; color:#64acff
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Lexicon MC-12HD</title>
<style>
  body { background:#111; color:#ddd; font-family:system-ui,Segoe UI,Arial,sans-serif; margin:24px; }
  .panel { width: 900px; background:#000; border:6px solid #333; padding:18px; box-shadow:0 0 30px #000; }
  .lcd { background:#001a33; color:#64acff; font-family: ui-monospace, Menlo, Consolas, monospace;
         font-size:20px; padding:6px 10px; margin-bottom:6px; white-space:pre; letter-spacing:2px; overflow:hidden; }
  .row { display:flex; gap:12px; align-items:center; margin-top:10px; flex-wrap:wrap; }
  label { min-width: 120px; }
  button, select { padding:10px 12px; background:#222; border:1px solid #444; color:#ddd; }
  button:hover { background:#2a2a2a; cursor:pointer; }
  .meta { margin-top:12px; font-family: ui-monospace, Menlo, Consolas, monospace; font-size:13px; color:#bbb; }
  .hex { margin-top:10px; font-family: ui-monospace, Menlo, Consolas, monospace; font-size:12px; color:#9aa; }
  details.drawer { border:1px solid #222; padding:10px; background:#0b0b0b; margin-top:12px; }
  details.drawer > summary { cursor:pointer; color:#bbb; font-family: ui-monospace, Menlo, Consolas, monospace; }
  details.drawer > summary { list-style: none; }
  details.drawer > summary::-webkit-details-marker { display:none; }
  .drawerButtons { display:flex; gap:12px; flex-wrap:wrap; margin-top:10px; }
  /* D-pad (diamond) layout for Up/Down/Left/Right */
  .dpad {
    display:grid;
    grid-template-columns: 52px 52px 52px;
    grid-template-rows:    52px 52px 52px;
    gap:10px;
    margin-top:10px;
    align-items:center;
    justify-items:center;
  }
  .dpad button { width:52px; height:52px; padding:0; }
  #navUp    { grid-column:2; grid-row:1; }
  #navLeft  { grid-column:1; grid-row:2; }
  #navRight { grid-column:3; grid-row:2; }
  #navDown  { grid-column:2; grid-row:3; }
</style>
</head>
<body>
  <div class="panel">
    <div id="l1" class="lcd"></div>
    <div id="l2" class="lcd"></div>
    <div id="vline" class="lcd"></div>

    <div class="row">
      <label>Volume</label>
      <button id="volDn">-1 dB</button>
      <button id="volUp">+1 dB</button>
      <button id="mutebtn">Mute</button>
      <span id="voltxt"></span>
    </div>

    <div class="row">
      <label>Effect</label>
      <button id="effDn">Effect -</button>
      <button id="effUp">Effect +</button>
    </div>

    <div class="row">
      <label for="inputSel">Input</label>
      <select id="inputSel">
        <option value="DVD1">DVD1 - Google TV</option>
        <option value="DVD2">DVD2 - BlueRay</option>
        <option value="LD">LD - Roku</option>
 
        <option value="TV">TV</option>
        <option value="SAT">SAT - PC</option>
        <option value="VCR">VCR</option>

        <option value="CD">CD</option>
        <option value="PVR">PVR - PC HDMI</option>
        <option value="GAME">GAME</option>
  
        <option value="TAPE">TAPE</option>
        <option value="TUNER">TUNER - Arcam</option>
        <option value="AUX">AUX</option>
      </select>
    </div>

    <details id="menu" class="drawer">
      <summary>Menu</summary>

    <div class="drawerButtons">
      <button id="navMenu">Menu</button>
    </div>
    <div class="dpad">
      <button id="navUp">Up</button>
      <button id="navLeft">Left</button>
      <button id="navRight">Right / Select</button>
      <button id="navDown">Down</button>
    </div>

      <div class="drawerButtons">
        <button id="navPower">Power</button>
      </div>
    </details>



    <details id="dbg" class="drawer">
      <summary>Debug</summary>
      <div class="drawerButtons">
        <button id="wakebtn">Send HOST_WAKEUP</button>
        <button id="syncbtn">Sync Now</button>
        <button id="wifiresetbtn">Reset WiFi</button>
      </div>
      <div class="meta" id="meta"></div>
      <div class="hex" id="hex1"></div>
      <div class="hex" id="hex2"></div>
    </details>
  </div>

<script>
async function refresh(){
  const j = await (await fetch('/status')).json();
  document.getElementById('l1').textContent = j.l1 || '';
  document.getElementById('l2').textContent = j.l2 || '';
  const db = (typeof j.volDb === 'number') ? j.volDb : 0;
  document.getElementById('vline').textContent = 'VOL ' + db.toFixed(0) + ' dB';
  document.getElementById('voltxt').textContent = db.toFixed(0) + ' dB';

  const mb = document.getElementById('mutebtn');
  mb.textContent = (j.muted ? 'Unmute' : 'Mute');

  document.getElementById('meta').innerHTML =
    'IP: ' + j.ip + '<br>' +
    'Host: ' + j.host + '<br>' +
    'MAC: ' + j.mac + '<br>' +
    'RSSI: ' + j.rssi + '<br>' +
    'WiFi: ' + (j.wifiOk ? 'OK' : 'DOWN') + '<br>' +
    'HostWakeAck: ' + j.hostAck + '  SysSynced: ' + j.sysSynced + '<br>' +
    'InputId: ' + j.inputId + '  EffectId: ' + j.effectId + '<br>' +
    'LastCmd: 0x' + j.lastCmdHex + '  Dll:' + j.lastDll + '  App:' + j.lastApp;

  document.getElementById('hex1').textContent = 'Line1 raw: ' + (j.line1Hex || '');
  document.getElementById('hex2').textContent = 'Line2 raw: ' + (j.line2Hex || '');
}

// volume/effect/mute

document.getElementById('volUp').addEventListener('click', ()=>{ fetch('/api?volstep=up'); });
document.getElementById('volDn').addEventListener('click', ()=>{ fetch('/api?volstep=down'); });
document.getElementById('effUp').addEventListener('click', ()=>{ fetch('/api?effect=up'); });
document.getElementById('effDn').addEventListener('click', ()=>{ fetch('/api?effect=down'); });
document.getElementById('mutebtn').addEventListener('click', ()=>{ fetch('/api?mute=toggle'); });

// input

document.getElementById('inputSel').addEventListener('change', (e)=>{ fetch('/api?input=' + encodeURIComponent(e.target.value)); });

// debug drawer actions

document.getElementById('wakebtn').addEventListener('click', ()=>{ fetch('/api?wakeup=1'); });
document.getElementById('syncbtn').addEventListener('click', ()=>{ fetch('/api?sync=1'); });
document.getElementById('wifiresetbtn').addEventListener('click', ()=>{
  if(confirm('Reset WiFi credentials and reboot?')){
    fetch('/api?wifireset=1');
  }
});

// menu drawer actions

document.getElementById('navMenu').addEventListener('click', ()=>{ fetch('/api?nav=menu'); });
document.getElementById('navLeft').addEventListener('click', ()=>{ fetch('/api?nav=left'); });
document.getElementById('navRight').addEventListener('click', ()=>{ fetch('/api?nav=right'); });
document.getElementById('navUp').addEventListener('click', ()=>{ fetch('/api?nav=up'); });
document.getElementById('navDown').addEventListener('click', ()=>{ fetch('/api?nav=down'); });
document.getElementById('navPower').addEventListener('click', ()=>{ fetch('/api?nav=power'); });

setInterval(refresh, 300);
refresh();
</script>
</body>
</html>
)HTML";

static void handleRoot() { server.send(200, "text/html", INDEX_HTML); }

static void handleStatus() {
  String json = "{";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"host\":\"" + String(MDNS_HOSTNAME) + ".local\",";
  json += "\"mac\":\"" + WiFi.macAddress() + "\",";
  json += "\"rssi\":" + String(WiFi.isConnected() ? WiFi.RSSI() : 0) + ",";
  json += "\"wifiOk\":" + String(WiFi.isConnected() ? "true" : "false") + ",";
  json += "\"hostAck\":" + String(st.hostAck ? "true" : "false") + ",";
  json += "\"sysSynced\":" + String(st.sysSynced ? "true" : "false") + ",";
  json += "\"l1\":\"" + st.line1 + "\",";
  json += "\"l2\":\"" + st.line2 + "\",";
  json += "\"line1Hex\":\"" + st.line1Hex + "\",";
  json += "\"line2Hex\":\"" + st.line2Hex + "\",";
  json += "\"volDb\":" + String((int)st.volDb) + ",";
  json += "\"muted\":" + String(st.muted ? "true" : "false") + ",";
  json += "\"inputId\":" + String(st.inputId) + ",";
  json += "\"effectId\":" + String(st.effectId) + ",";
  json += "\"lastCmdHex\":\"" + hex2(st.lastCmd) + "\",";
  json += "\"lastDll\":" + String(st.lastDll) + ",";
  json += "\"lastApp\":" + String(st.lastApp);
  json += "}";
  server.send(200, "application/json", json);
}

static void handleAPI() {
  if (server.hasArg("wakeup")) { sendHostWakeup(); server.send(200, "text/plain", "OK"); return; }
  if (server.hasArg("sync"))   { sendGetSysStatus(); server.send(200, "text/plain", "OK"); return; }

  if (server.hasArg("wifireset")) {
    server.send(200, "text/plain", "Resetting WiFi");
    delay(200);
    resetWifiAndReboot();
    return;
  }

  if (server.hasArg("volstep")) {
    String s = server.arg("volstep");
    sendIr((s == "up") ? IR_VOL_INCR : IR_VOL_DECR);
    server.send(200, "text/plain", "OK");
    return;
  }

  if (server.hasArg("effect")) {
    String e = server.arg("effect");
    sendIr((e == "up") ? IR_MODE_INCR : IR_MODE_DECR);
    server.send(200, "text/plain", "OK");
    return;
  }

  if (server.hasArg("mute")) {
    String m = server.arg("mute");
    if (m == "toggle") sendMute(!st.muted);
    else if (m == "1") sendMute(true);
    else sendMute(false);
    server.send(200, "text/plain", "OK");
    return;
  }

  if (server.hasArg("nav")) {
    String n = server.arg("nav");
    if      (n == "menu")  sendIr(IR_NAV_MENU);
    else if (n == "left")  sendIr(IR_NAV_LEFT);
    else if (n == "right") sendIr(IR_NAV_RIGHT);
    else if (n == "up")    sendIr(IR_NAV_UP);
    else if (n == "down")  sendIr(IR_NAV_DOWN);
    else if (n == "power") sendIr(IR_NAV_POWER);
    server.send(200, "text/plain", "OK");
    return;
  }

  if (server.hasArg("input")) {
    String in = server.arg("input");
    if      (in == "TV")    sendIr(IR_MAIN_TV);
    else if (in == "DVD1")  sendIr(IR_MAIN_DVD1);
    else if (in == "DVD2")  sendIr(IR_MAIN_DVD2);
    else if (in == "LD")    sendIr(IR_MAIN_LD);
    else if (in == "SAT")   sendIr(IR_MAIN_SAT);
    else if (in == "VCR")   sendIr(IR_MAIN_VCR);
    else if (in == "CD")    sendIr(IR_MAIN_CD);
    else if (in == "PVR")   sendIr(IR_MAIN_PVR);
    else if (in == "GAME")  sendIr(IR_MAIN_GAME);
    else if (in == "TAPE")  sendIr(IR_MAIN_TAPE);
    else if (in == "TUNER") sendIr(IR_MAIN_TUNER);
    else if (in == "AUX")   sendIr(IR_MAIN_AUX);
    server.send(200, "text/plain", "OK");
    return;
  }

  server.send(400, "text/plain", "Bad request");
}

/******************** SETUP/LOOP ********************/
void setup() {
  pinMode(WIFI_LED_PIN, OUTPUT);
  setWifiLed(false);

  Serial.begin(115200);
  delay(200);
  DBGLN("\n=== ESP32-C3 Lexicon MC-12HD Web Controller (v20 menu+power) ===");

  wifiProvision();

  Serial1.begin(19200, SERIAL_8O1, LEXICON_RX_PIN, LEXICON_TX_PIN);
  DBGLN("[UART] Serial1: 19200 8O1");

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/api", handleAPI);
  server.begin();
  DBGLN("[Web] Server started");

  startMDNS();

  sendHostWakeup();
}

void loop() {
  bool nowOk = (WiFi.status() == WL_CONNECTED);
  if (nowOk != st.wifiOk) {
    st.wifiOk = nowOk;
    setWifiLed(st.wifiOk);
  }

  server.handleClient();
  readLexicon();

  unsigned long now = millis();
  if (!st.hostAck && now - st.lastWakeRetryMs > 3000) {
    st.lastWakeRetryMs = now;
    sendHostWakeup();
  }

  if (st.hostAck && !st.sysSynced && (now - st.lastSysQueryMs > 1000)) {
    sendGetSysStatus();
  }
}
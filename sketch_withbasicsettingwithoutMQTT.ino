#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Update.h>
#include <Preferences.h>
#include <time.h>

#define RXD2 16
#define TXD2 17
#define MAX_OUTAGES 30

WebServer server(80);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;

struct AppConfig {
  String deviceId = "ziewnic-01";
  String mqttHost = "";
  int mqttPort = 1883;
  String mqttUser = "";
  String mqttPass = "";
  String baseTopic = "inverter/ziewnic-01";

  long baudRate = 2400;
  unsigned long pollMs = 5000;

  String cmdUtility = "POP00";
  String cmdSolar = "POP01";
  String cmdSbu = "POP02";
};

struct Slot {
  bool enabled = false;
  int startMin = 0;
  int endMin = 0;
  String mode = "NONE";    
  String endMode = "NONE"; 
};

struct NightGuard {
  bool enabled = false;
  int startMin = 1140; 
  int endMin = 360;    
  String nightAmps = "MNCHGC020"; 
  String dayAmps = "MNCHGC060";   
};

// === NEW: OUTAGE TRACKER CONFIG ===
struct OutageEvent {
  time_t start = 0;
  time_t end = 0;
  bool ongoing = false;
};

AppConfig cfg;
Slot slots[4];
NightGuard nightGuard;
OutageEvent outages[MAX_OUTAGES];

int outageHead = 0;
int outageCount = 0;
bool gridPresent = true; 

String logBuffer = "";
String lastQpigs = "";
String lastQpiri = "";
String lastQmod = "";
String lastQflag = "";
String lastAppliedMode = "";
String lastSchedulerAction = "None";
String lastNightGuardState = "NONE";

unsigned long lastPoll = 0;
unsigned long lastMqttTry = 0;
unsigned long lastScheduleCheck = 0;

bool quickTimerActive = false;
unsigned long quickTimerStartTime = 0;
unsigned long quickTimerDurationMs = 0;
String modeBeforeTimer = "";
String quickTimerMode = "";

String executeRawHex(String hexString, uint16_t timeoutMs = 2200);
void handleRawHex();

void addLog(String msg) {
  String line = "[" + String(millis() / 1000) + "s] " + msg + "\n";
  Serial.print(line);
  logBuffer += line;
  if (logBuffer.length() > 9000) logBuffer = logBuffer.substring(logBuffer.length() - 7500);
}

String h(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

String j(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\\n");
  s.replace("\r", "");
  return s;
}

String hx(uint8_t b) {
  char buf[4];
  sprintf(buf, "%02X", b);
  return String(buf);
}

uint16_t cal_crc_half(uint16_t crc, uint8_t ch) {
  crc ^= ((uint16_t)ch << 8);
  for (uint8_t i = 0; i < 8; i++) {
    if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
    else crc <<= 1;
  }
  return crc;
}

uint16_t calculateCRC(String cmd) {
  uint16_t crc = 0;
  for (int i = 0; i < cmd.length(); i++) crc = cal_crc_half(crc, cmd[i]);
  return crc;
}

String executeCommand(String cmd, uint16_t timeoutMs = 2200) {
  cmd.trim();
  if (cmd.length() == 0) return "";

  if (cmd == "POP02" || cmd == "SBU") {
    addLog("[SYSTEM] SBU detected. Redirecting directly to verified working raw hex sequence.");
    return executeRawHex("50 4F 50 30 32 E2 0B 0D", timeoutMs);
  }

  while (Serial2.available()) Serial2.read();

  uint16_t crc = calculateCRC(cmd);
  uint8_t hi = (crc >> 8) & 0xFF;
  uint8_t lo = crc & 0xFF;

  Serial2.print(cmd);
  Serial2.write(hi);
  Serial2.write(lo);
  Serial2.write('\r');

  String tx = "";
  for (int i = 0; i < cmd.length(); i++) tx += hx((uint8_t)cmd[i]) + " ";
  tx += hx(hi) + " " + hx(lo) + " 0D";

  addLog("TX -> " + cmd + " | " + tx);

  uint8_t buf[350];
  int len = 0;
  unsigned long start = millis();

  while (millis() - start < timeoutMs && len < 350) {
    while (Serial2.available() && len < 350) {
      buf[len++] = Serial2.read();
      start = millis();
    }
  }

  if (len == 0) {
    addLog("RX <- no response");
    return "";
  }

  String rxHex = "";
  String rxAsc = "";
  for (int i = 0; i < len; i++) {
    rxHex += hx(buf[i]) + " ";
    if (buf[i] >= 32 && buf[i] <= 126) rxAsc += (char)buf[i];
    else if (buf[i] == '\r') rxAsc += "<CR>";
    else rxAsc += "[" + String(buf[i]) + "]";
  }

  addLog("RX HEX <- " + rxHex);
  addLog("RX ASC <- " + rxAsc);

  int end = len;
  if (len >= 3 && buf[len - 1] == '\r') end = len - 3;

  String clean = "";
  for (int i = 0; i < end; i++) {
    if (buf[i] >= 32 && buf[i] <= 126) clean += (char)buf[i];
  }

  return clean;
}

bool splitTokens(String input, String tokens[], int maxTokens, int &count) {
  count = 0;
  input.trim();
  if (input.startsWith("(")) input.remove(0, 1);

  int start = 0;
  while (start < input.length() && count < maxTokens) {
    while (start < input.length() && input[start] == ' ') start++;
    int end = input.indexOf(' ', start);
    if (end == -1) end = input.length();

    String token = input.substring(start, end);
    token.trim();
    if (token.length() > 0) tokens[count++] = token;

    start = end + 1;
  }

  return count > 0;
}

String modeLabel(String m) {
  if (m == "P") return "Power On";
  if (m == "S") return "Standby";
  if (m == "L") return "Line";
  if (m == "B") return "Battery";
  if (m == "F") return "Fault";
  if (m == "H") return "Power Saving";
  return m;
}

String sourcePriorityLabel(String v) {
  if (v == "0") return "Utility First";
  if (v == "1") return "Solar First";
  if (v == "2") return "SBU First";
  return "Unknown (" + v + ")";
}

String chargerPriorityLabel(String v) {
  if (v == "0") return "Utility First";
  if (v == "1") return "Solar First";
  if (v == "2") return "Solar + Utility";
  if (v == "3") return "Only Solar";
  return "Unknown (" + v + ")";
}

String getToken(String source, int index) {
  String t[40];
  int c = 0;
  if (!splitTokens(source, t, 40, c)) return "";
  if (index < 0 || index >= c) return "";
  return t[index];
}

String parseQpigsJson(String r) {
  String t[35];
  int c = 0;
  if (!splitTokens(r, t, 35, c) || c < 15) return "";

  String json = "{";
  json += "\"gridVoltage\":" + t[0] + ",";
  json += "\"gridFrequency\":" + t[1] + ",";
  json += "\"outputVoltage\":" + t[2] + ",";
  json += "\"outputFrequency\":" + t[3] + ",";
  json += "\"outputVA\":" + t[4] + ",";
  json += "\"outputWatts\":" + t[5] + ",";
  json += "\"loadPercent\":" + t[6] + ",";
  json += "\"busVoltage\":" + t[7] + ",";
  json += "\"batteryVoltage\":" + t[8] + ",";
  json += "\"batteryCapacity\":" + t[10] + ",";
  json += "\"chargingCurrent\":" + t[11] + ",";
  json += "\"pvVoltage\":\"" + String(c > 14 ? t[14] : "") + "\",";
  json += "\"raw\":\"" + j(r) + "\"";
  json += "}";

  return json;
}

void loadConfig() {
  prefs.begin("gw", true);

  cfg.deviceId = prefs.getString("deviceId", "ziewnic-01");
  cfg.mqttHost = prefs.getString("mqttHost", "");
  cfg.mqttPort = prefs.getInt("mqttPort", 1883);
  cfg.mqttUser = prefs.getString("mqttUser", "");
  cfg.mqttPass = prefs.getString("mqttPass", "");
  cfg.baseTopic = prefs.getString("baseTopic", "inverter/" + cfg.deviceId);
  cfg.baudRate = prefs.getLong("baud", 2400);
  cfg.pollMs = prefs.getULong("pollMs", 5000);

  cfg.cmdUtility = prefs.getString("cmdUtility", "POP00");
  cfg.cmdSolar = prefs.getString("cmdSolar", "POP01");
  cfg.cmdSbu = prefs.getString("cmdSbu", "POP02");

  for (int i = 0; i < 4; i++) {
    String p = "s" + String(i);
    slots[i].enabled = prefs.getBool((p + "en").c_str(), false);
    slots[i].startMin = prefs.getInt((p + "st").c_str(), 0);
    slots[i].endMin = prefs.getInt((p + "ed").c_str(), 0);
    slots[i].mode = prefs.getString((p + "md").c_str(), "NONE");
    slots[i].endMode = prefs.getString((p + "em").c_str(), "NONE");
  }

  nightGuard.enabled = prefs.getBool("ngEn", false);
  nightGuard.startMin = prefs.getInt("ngSt", 1140);
  nightGuard.endMin = prefs.getInt("ngEd", 360);
  nightGuard.nightAmps = prefs.getString("ngNA", "MNCHGC020");
  nightGuard.dayAmps = prefs.getString("ngDA", "MNCHGC060");

  prefs.end();
}

void saveConfig() {
  prefs.begin("gw", false);

  prefs.putString("deviceId", cfg.deviceId);
  prefs.putString("mqttHost", cfg.mqttHost);
  prefs.putInt("mqttPort", cfg.mqttPort);
  prefs.putString("mqttUser", cfg.mqttUser);
  prefs.putString("mqttPass", cfg.mqttPass);
  prefs.putString("baseTopic", cfg.baseTopic);
  prefs.putLong("baud", cfg.baudRate);
  prefs.putULong("pollMs", cfg.pollMs);

  prefs.putString("cmdUtility", cfg.cmdUtility);
  prefs.putString("cmdSolar", cfg.cmdSolar);
  prefs.putString("cmdSbu", cfg.cmdSbu);

  for (int i = 0; i < 4; i++) {
    String p = "s" + String(i);
    prefs.putBool((p + "en").c_str(), slots[i].enabled);
    prefs.putInt((p + "st").c_str(), slots[i].startMin);
    prefs.putInt((p + "ed").c_str(), slots[i].endMin);
    prefs.putString((p + "md").c_str(), slots[i].mode);
    prefs.putString((p + "em").c_str(), slots[i].endMode);
  }

  prefs.putBool("ngEn", nightGuard.enabled);
  prefs.putInt("ngSt", nightGuard.startMin);
  prefs.putInt("ngEd", nightGuard.endMin);
  prefs.putString("ngNA", nightGuard.nightAmps);
  prefs.putString("ngDA", nightGuard.dayAmps);

  prefs.end();
}

String minToTime(int m) {
  int h = m / 60;
  int mm = m % 60;
  char buf[6];
  sprintf(buf, "%02d:%02d", h, mm);
  return String(buf);
}

int timeToMin(String s) {
  s.trim();
  if (s.length() < 5) return 0;
  int h = s.substring(0, 2).toInt();
  int m = s.substring(3, 5).toInt();
  if (h < 0) h = 0;
  if (h > 23) h = 23;
  if (m < 0) m = 0;
  if (m > 59) m = 59;
  return h * 60 + m;
}

// === NEW: OUTAGE DATE/TIME HELPERS ===
String formatTimeOutage(time_t t) {
  if (t == 0) return "-";
  struct tm *ti = localtime(&t);
  char buf[30];
  strftime(buf, sizeof(buf), "%I:%M %p (%b %d)", ti);
  return String(buf);
}

String formatDurationOutage(time_t start, time_t end) {
  long diff = (long)difftime(end, start);
  int h = diff / 3600;
  int m = (diff % 3600) / 60;
  char buf[20];
  if (h > 0) sprintf(buf, "%dh %dm", h, m);
  else sprintf(buf, "%dm", m);
  return String(buf);
}

bool slotActive(Slot s, int nowMin) {
  if (!s.enabled || s.mode == "NONE") return false;
  if (s.startMin == s.endMin) return false;

  if (s.startMin < s.endMin) {
    return nowMin >= s.startMin && nowMin < s.endMin;
  }

  return nowMin >= s.startMin || nowMin < s.endMin;
}

String commandForMode(String mode) {
  if (mode == "UTILITY") return cfg.cmdUtility;
  if (mode == "SOLAR") return cfg.cmdSolar;
  if (mode == "SBU") return cfg.cmdSbu;
  return "";
}

void checkNightGuard() {
  if (!nightGuard.enabled) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  int nowMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  bool isNight = false;

  if (nightGuard.startMin < nightGuard.endMin) {
    isNight = (nowMin >= nightGuard.startMin && nowMin < nightGuard.endMin);
  } else {
    isNight = (nowMin >= nightGuard.startMin || nowMin < nightGuard.endMin);
  }

  String desiredState = isNight ? "NIGHT" : "DAY";
  if (desiredState == lastNightGuardState) return;

  String cmd = isNight ? nightGuard.nightAmps : nightGuard.dayAmps;
  if (cmd != "") {
    executeCommand(cmd, 2200);
    addLog("🌙 Night Guard Shift: Applied " + cmd + " for " + desiredState + " mode.");
  }
  
  lastNightGuardState = desiredState;
}

void checkQuickTimer() {
  if (quickTimerActive) {
    if (millis() - quickTimerStartTime >= quickTimerDurationMs) {
      quickTimerActive = false;
      addLog("Timer expired. Reverting to: " + modeBeforeTimer);
      if (modeBeforeTimer != "" && modeBeforeTimer != "NONE") {
        String cmd = commandForMode(modeBeforeTimer);
        if (cmd != "") {
          executeCommand(cmd, 2200);
          lastAppliedMode = modeBeforeTimer;
        }
      }
    }
  }
}

void checkScheduler() {
  if (quickTimerActive) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  int nowMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;

  String desiredMode = "NONE";
  bool insideActiveSlot = false;

  for (int i = 0; i < 4; i++) {
    if (slotActive(slots[i], nowMin)) {
      desiredMode = slots[i].mode;
      insideActiveSlot = true;
      break;
    }
  }

  if (!insideActiveSlot) {
    for (int i = 0; i < 4; i++) {
      if (slots[i].enabled && nowMin == slots[i].endMin && slots[i].endMode != "NONE") {
        desiredMode = slots[i].endMode;
        break;
      }
    }
  }

  if (desiredMode == "NONE") return;
  if (desiredMode == lastAppliedMode) return;

  String cmd = commandForMode(desiredMode);
  if (cmd.length() == 0) return;

  String resp = executeCommand(cmd, 2200);

  lastSchedulerAction = minToTime(nowMin) + " -> " + desiredMode;

  if (resp.indexOf("ACK") >= 0 || resp.indexOf("ACK") >= 0) {
    lastAppliedMode = desiredMode;
    addLog("Scheduler mode applied: " + desiredMode);
  } else {
    addLog("Scheduler command failed: " + cmd + " resp=" + resp);
  }
}

void publishMqtt(String topic, String payload, bool retained = false) {
  if (mqtt.connected()) mqtt.publish(topic.c_str(), payload.c_str(), retained);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String cmd = "";
  for (unsigned int i = 0; i < length; i++) cmd += (char)payload[i];
  cmd.trim();

  addLog("MQTT CMD <- " + cmd);
  String resp = executeCommand(cmd, 2200);
  publishMqtt(cfg.baseTopic + "/command/result", "{\"command\":\"" + j(cmd) + "\",\"response\":\"" + j(resp) + "\"}");
}

void reconnectMqtt() {
  if (cfg.mqttHost.length() == 0 || mqtt.connected()) return;

  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1400);

  String clientId = "esp32-" + cfg.deviceId;

  bool ok = false;
  if (cfg.mqttUser.length() > 0) ok = mqtt.connect(clientId.c_str(), cfg.mqttUser.c_str(), cfg.mqttPass.c_str());
  else ok = mqtt.connect(clientId.c_str());

  if (ok) {
    addLog("MQTT connected");
    mqtt.subscribe((cfg.baseTopic + "/command/send").c_str());
    publishMqtt(cfg.baseTopic + "/status", "online", true);
  } else {
    addLog("MQTT failed rc=" + String(mqtt.state()));
  }
}

String header(String title) {
  String p = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  p += "<title>" + title + "</title><style>";
  p += "body{font-family:Arial;background:#eef2f3;margin:0;padding:12px;color:#263238}";
  p += ".box{max-width:1050px;margin:auto;background:#fff;padding:16px;border-radius:12px;box-shadow:0 3px 12px #0002}";
  p += "h2{color:#1976d2;margin:0 0 10px}.nav a{margin-right:12px;color:#1976d2;font-weight:bold;text-decoration:none}";
  p += ".sec{background:#f7f9fa;border-left:5px solid #1976d2;padding:12px;margin:12px 0;border-radius:8px}";
  p += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px}.card{background:#fff;padding:10px;border-radius:8px;border:1px solid #ddd}";
  p += ".k{font-size:12px;color:#607d8b}.v{font-size:22px;font-weight:bold}.console{background:#111;color:#00e676;padding:12px;height:310px;overflow:auto;white-space:pre-wrap;font-family:Consolas,monospace;font-size:13px;border-radius:8px}";
  p += "input,select,button{width:100%;padding:10px;margin-top:6px;box-sizing:border-box;font-size:15px}button{background:#1976d2;color:#fff;border:0;border-radius:6px;font-weight:bold}.red{background:#d32f2f}";
  p += "table{width:100%;border-collapse:collapse}td,th{border:1px solid #ddd;padding:8px;text-align:left}";
  p += "</style></head><body><div class='box'><div class='nav'>";
  p += "<a href='/'>Dashboard</a><a href='/settings'>Settings</a><a href='/scheduler'>Scheduler</a><a href='/outages'>Outages</a><a href='/gateway'>Gateway</a></div><h2>" + title + "</h2>";
  return p;
}

String footer() {
  return "</div></body></html>";
}

String card(String k, String v, String unit = "") {
  return "<div class='card'><div class='k'>" + k + "</div><div class='v'>" + v + "</div><div class='k'>" + unit + "</div></div>";
}

void refreshInverterCache() {
  lastQpigs = executeCommand("QPIGS", 2200);
  delay(100);
  lastQmod = executeCommand("QMOD", 1800);
  delay(100);
  lastQpiri = executeCommand("QPIRI", 2500);
  delay(100);
  lastQflag = executeCommand("QFLAG", 1800);
}

void handleRoot() {
  String t[35];
  int c = 0;
  splitTokens(lastQpigs, t, 35, c);

  String p = header("Live Inverter Dashboard");

  p += "<div class='sec'><button onclick=\"fetch('/refresh').then(()=>setTimeout(()=>location.reload(),800))\">Refresh Now</button></div>";

  p += "<div class='grid'>";
  p += card("Grid Voltage", c > 0 ? t[0] : "-", "V");
  p += card("Grid Frequency", c > 1 ? t[1] : "-", "Hz");
  p += card("Output Voltage", c > 2 ? t[2] : "-", "V");
  p += card("Output Frequency", c > 3 ? t[3] : "-", "Hz");
  p += card("Output Power", c > 5 ? t[5] : "-", "W");
  p += card("Load", c > 6 ? t[6] : "-", "%");
  p += card("Battery Voltage", c > 8 ? t[8] : "-", "V");
  p += card("Battery Capacity", c > 10 ? t[10] : "-", "%");
  p += card("Charging Current", c > 11 ? t[11] : "-", "A");
  p += card("Current Mode", modeLabel(getToken(lastQmod, 0)), "");
  p += card("MQTT", mqtt.connected() ? "Connected" : "Off", "");
  
  if (quickTimerActive) {
    long remainMins = (quickTimerDurationMs - (millis() - quickTimerStartTime)) / 60000;
    if (remainMins < 0) remainMins = 0;
    p += card("Scheduler", "Paused (Timer: " + String(remainMins) + "m left)", "");
  } else {
    p += card("Scheduler", lastSchedulerAction, "");
  }
  p += "</div>";

  p += "<div class='sec'><h3>⏱️ Quick Override Timer</h3>";
  if (quickTimerActive) {
    long remainMins = (quickTimerDurationMs - (millis() - quickTimerStartTime)) / 60000;
    if (remainMins < 0) remainMins = 0;
    p += "<p><b>Status:</b> Forcing " + quickTimerMode + " mode.<br>Reverting to " + modeBeforeTimer + " in approx " + String(remainMins) + " minutes.</p>";
    p += "<button class='red' onclick=\"fetch('/stop-timer').then(()=>setTimeout(()=>location.reload(),800))\">Cancel Timer & Revert Now</button>";
  } else {
    p += "<p style='font-size:13px; color:#607d8b; margin-top:0;'>Temporarily activate a mode. Automatically reverts to the previous mode.</p>";
    p += "<div style='display:flex; gap:10px;'>";
    p += "<input id='tMins' type='number' value='60' placeholder='Mins' style='width:30%;'>";
    p += "<select id='tMode' style='width:40%; margin-top:6px; padding:10px;'><option value='SBU'>SBU</option><option value='UTILITY'>UTILITY</option><option value='SOLAR'>SOLAR</option></select>";
    p += "<button style='width:30%;' onclick=\"fetch('/start-timer?mins='+document.getElementById('tMins').value+'&mode='+document.getElementById('tMode').value).then(()=>setTimeout(()=>location.reload(),800))\">Start Timer</button>";
    p += "</div>";
  }
  p += "</div>";

  p += "<div class='sec'>";
  p += "<h3>Manual Command</h3>";
  p += "<input id='cmd' value='QPI'>";
  p += "<button onclick='sendCmd()'>Send With CRC + CR</button>";
  p += "</div>";

  p += "<div class='sec'>";
  p += "<h3>Raw Hex Command</h3>";
  p += "<input id='rawhex' value='50 4F 50 30 32 E2 0B 0D'>";
  p += "<button onclick='sendRawHex()'>Send Raw Hex</button>";
  p += "</div>";

  p += "<div class='sec'><h3>Live Log</h3><div class='console' id='log'>Loading...</div><button class='red' onclick='clearLog()'>Clear Log</button></div>";
  p += "<div class='sec'><h3>OTA Firmware Upload</h3><form method='POST' action='/ota' enctype='multipart/form-data'><input type='file' name='update' accept='.bin' required><button class='red'>Upload Firmware</button></form></div>";

  p += "<script>";
  p += "function load(){fetch('/log').then(r=>r.text()).then(t=>{let e=document.getElementById('log');e.innerText=t;e.scrollTop=e.scrollHeight;});}";
  p += "setInterval(load,1000);load();";
  p += "function sendCmd(){fetch('/cmd?value='+encodeURIComponent(document.getElementById('cmd').value));}";
  p += "function sendRawHex(){fetch('/rawhex?hex='+encodeURIComponent(document.getElementById('rawhex').value));}";
  p += "function clearLog(){fetch('/clear');}";
  p += "</script>";

  p += footer();
  server.send(200, "text/html", p);
}

// === NEW: OUTAGES PAGE ENDPOINT ===
void handleOutagesPage() {
  String p = header("Load Shedding Tracker");
  
  p += "<div class='sec'>";
  p += "<h3>Grid Status: ";
  if (gridPresent) p += "<span style='color:#10b981;'>ONLINE ⚡</span></h3>";
  else p += "<span style='color:#d32f2f;'>OFFLINE ⚠️</span></h3>";
  p += "<p style='font-size:13px; color:#607d8b;'>This page actively tracks and records grid outages (up to 30 events) to help you map load shedding durations.</p>";
  p += "</div>";

  p += "<div class='sec'>";
  p += "<h3>Outage History</h3>";
  p += "<table><tr><th>Grid Disconnected</th><th>Grid Restored</th><th>Total Duration</th></tr>";

  if (outageCount == 0 && !outages[outageHead].ongoing) {
    p += "<tr><td colspan='3'>No outages recorded in current session history.</td></tr>";
  } else {
    if (outages[outageHead].ongoing) {
      time_t now; time(&now);
      p += "<tr style='background:#ffebee; color:#d32f2f; font-weight:bold;'>";
      p += "<td>" + formatTimeOutage(outages[outageHead].start) + "</td>";
      p += "<td>ONGOING</td>";
      p += "<td>" + formatDurationOutage(outages[outageHead].start, now) + "</td>";
      p += "</tr>";
    }
    
    int countToPrint = outageCount;
    int idx = outageHead - 1;
    while (countToPrint > 0) {
      if (idx < 0) idx = MAX_OUTAGES - 1;
      p += "<tr>";
      p += "<td>" + formatTimeOutage(outages[idx].start) + "</td>";
      p += "<td>" + formatTimeOutage(outages[idx].end) + "</td>";
      p += "<td>" + formatDurationOutage(outages[idx].start, outages[idx].end) + "</td>";
      p += "</tr>";
      idx--;
      countToPrint--;
    }
  }
  
  p += "</table></div>";
  
  p += footer();
  server.send(200, "text/html", p);
}

void handleStartTimer() {
  long mins = server.arg("mins").toInt();
  String mode = server.arg("mode");
  
  if (mins > 0 && mode != "NONE" && mode != "") {
    modeBeforeTimer = lastAppliedMode;
    if (modeBeforeTimer == "") modeBeforeTimer = "UTILITY"; 
    
    quickTimerMode = mode;
    quickTimerStartTime = millis();
    quickTimerDurationMs = mins * 60000UL;
    quickTimerActive = true;

    String cmd = commandForMode(quickTimerMode);
    executeCommand(cmd, 2200);
    lastAppliedMode = quickTimerMode;
    addLog("Timer started: Forced " + quickTimerMode + " for " + String(mins) + " mins.");
  }
  server.send(200, "text/plain", "OK");
}

void handleStopTimer() {
  if (quickTimerActive) {
    quickTimerActive = false;
    addLog("Timer cancelled manually. Reverting to: " + modeBeforeTimer);
    if (modeBeforeTimer != "" && modeBeforeTimer != "NONE") {
      String cmd = commandForMode(modeBeforeTimer);
      if (cmd != "") {
        executeCommand(cmd, 2200);
        lastAppliedMode = modeBeforeTimer;
      }
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleSaveNightFix() {
  nightGuard.enabled = server.arg("en") == "1";
  nightGuard.startMin = timeToMin(server.arg("st"));
  nightGuard.endMin = timeToMin(server.arg("ed"));
  nightGuard.nightAmps = server.arg("nAmps");
  nightGuard.dayAmps = server.arg("dAmps");
  
  saveConfig();
  
  lastNightGuardState = "FORCE_UPDATE"; 
  server.send(200, "text/html", "<meta charset='utf-8'><h2>🌙 Night Guard Rules Saved!</h2><script>setTimeout(()=>window.location.href='/settings', 1200);</script>");
}

void handleSettingsPage() {
  String outPr = getToken(lastQpiri, 16);
  String chPr = getToken(lastQpiri, 17);
  String maxTotalAmps = getToken(lastQpiri, 11); 
  String p = header("System Configuration");

  p += "<div class='sec'><button onclick=\"fetch('/refresh').then(()=>setTimeout(()=>location.reload(),800))\">Read Current Settings</button></div>";

  p += "<div class='grid'>";
  p += card("Output Priority", sourcePriorityLabel(outPr));
  p += card("Charger Priority", chargerPriorityLabel(chPr));
  p += card("Total Charge Max", maxTotalAmps, "A");
  p += card("Raw QFLAG", lastQflag);
  p += "</div>";

  p += "<form method='POST' action='/save-nightfix'>";
  p += "<div class='sec' style='border-left: 5px solid #6366f1;'><h3>🌙 Phantom Solar Bug Fix (Night Guard)</h3>";
  p += "<p style='font-size:12px; margin:0 0 10px 0;'>Ziewnic inverters often read fake solar voltage at night, keeping Total Charge Limit active instead of dropping to AC Grid Limit. This strictly forces the Total Charging Limit down at night to prevent massive grid drain, and restores it in the morning.</p>";
  
  p += "<table style='margin-bottom:10px;'><tr><th>Enable</th><th>Night Start</th><th>Morning Restore</th></tr>";
  p += "<tr><td><select name='en'><option value='0'>Off</option><option value='1'" + String(nightGuard.enabled ? " selected" : "") + ">On</option></select></td>";
  p += "<td><input name='st' value='" + minToTime(nightGuard.startMin) + "'></td>";
  p += "<td><input name='ed' value='" + minToTime(nightGuard.endMin) + "'></td></tr></table>";

  String ampVals[6] = {"MNCHGC010", "MNCHGC020", "MNCHGC030", "MNCHGC040", "MNCHGC050", "MNCHGC060"};
  String ampLabels[6] = {"10A", "20A", "30A", "40A", "50A", "60A"};

  p += "<label style='font-size:14px; font-weight:bold;'>Total Charge Limit at Night:</label>";
  p += "<select name='nAmps' style='margin-bottom:10px;'>";
  for (int i=0; i<6; i++) p += "<option value='" + ampVals[i] + "'" + String(nightGuard.nightAmps == ampVals[i] ? " selected" : "") + ">Force Total Limit to: " + ampLabels[i] + "</option>";
  p += "</select>";

  p += "<label style='font-size:14px; font-weight:bold;'>Restore Total Charge Limit in Day:</label>";
  p += "<select name='dAmps' style='margin-bottom:10px;'>";
  for (int i=0; i<6; i++) p += "<option value='" + ampVals[i] + "'" + String(nightGuard.dayAmps == ampVals[i] ? " selected" : "") + ">Restore Total Limit to: " + ampLabels[i] + "</option>";
  p += "</select>";

  p += "<button class='green'>Save Night Guard Rule</button>";
  p += "</div></form>";

  p += "<div class='sec'><h3>Update Output Source Priority</h3>";
  p += "<select id='sourceMode'><option value='UTILITY'>Utility First</option><option value='SOLAR'>Solar First</option><option value='SBU'>SBU First</option></select>";
  p += "<button onclick=\"fetch('/apply-source?mode='+document.getElementById('sourceMode').value).then(()=>setTimeout(()=>location.reload(),1000))\">Apply</button></div>";

  p += "<div class='sec'><h3>Update Charger Priority</h3>";
  p += "<select id='chargerMode'><option value='PCP00'>Utility First</option><option value='PCP01'>Solar First</option><option value='PCP02'>Solar + Utility</option><option value='PCP03'>Only Solar</option></select>";
  p += "<button onclick=\"fetch('/send-setting?cmd='+document.getElementById('chargerMode').value).then(()=>setTimeout(()=>location.reload(),1000))\">Apply</button></div>";

  p += "<div class='sec'><h3>Max Utility (AC) Charging Current</h3>";
  p += "<p style='font-size:12px; margin:0;'>Limit the load pulled from Grid when charging batteries.</p>";
  p += "<select id='acChargeRate'><option value='MUCHGC002'>2A</option><option value='MUCHGC010'>10A</option><option value='MUCHGC020'>20A</option><option value='MUCHGC030'>30A</option></select>";
  p += "<button onclick=\"fetch('/send-setting?cmd='+document.getElementById('acChargeRate').value).then(()=>setTimeout(()=>location.reload(),1000))\">Apply Limit</button></div>";

  p += "<div class='sec'><h3>Back to Utility (Grid) Voltage</h3>";
  p += "<p style='font-size:12px; margin:0;'>Example: 23.5 or 46.0</p>";
  p += "<input id='vUtility' placeholder='Voltage' value='24.0'>";
  p += "<button onclick=\"fetch('/send-setting?cmd=PBDV'+document.getElementById('vUtility').value).then(()=>setTimeout(()=>location.reload(),1000))\">Apply Voltage</button></div>";

  p += "<div class='sec'><h3>Back to Battery (Discharge) Voltage</h3>";
  p += "<p style='font-size:12px; margin:0;'>Example: 27.0 or 54.0</p>";
  p += "<input id='vBattery' placeholder='Voltage' value='27.0'>";
  p += "<button onclick=\"fetch('/send-setting?cmd=PBCV'+document.getElementById('vBattery').value).then(()=>setTimeout(()=>location.reload(),1000))\">Apply Voltage</button></div>";

  p += "<div class='sec'><h3>Flags</h3>";
  p += "<table><tr><th>Setting</th><th>Enable</th><th>Disable</th></tr>";
  p += "<tr><td>Buzzer silence/open buzzer</td><td><button onclick=\"send('PEA')\">Enable</button></td><td><button onclick=\"send('PDA')\">Disable</button></td></tr>";
  p += "<tr><td>Overload restart</td><td><button onclick=\"send('PEU')\">Enable</button></td><td><button onclick=\"send('PDU')\">Disable</button></td></tr>";
  p += "<tr><td>Over-temperature restart</td><td><button onclick=\"send('PEV')\">Enable</button></td><td><button onclick=\"send('PDV')\">Disable</button></td></tr>";
  p += "<tr><td>Backlight</td><td><button onclick=\"send('PEX')\">Enable</button></td><td><button onclick=\"send('PDX')\">Disable</button></td></tr>";
  p += "</table></div>";

  p += "<script>function send(c){fetch('/send-setting?cmd='+c).then(()=>setTimeout(()=>location.reload(),1000));}</script>";
  p += footer();

  server.send(200, "text/html", p);
}

void handleSchedulerPage() {
  String p = header("Time Based Scheduler");

  p += "<div class='sec'><b>Current scheduler action:</b> " + h(lastSchedulerAction) + "<br>";
  p += "<b>Last applied mode:</b> " + h(lastAppliedMode) + "</div>";

  p += "<form method='POST' action='/save-scheduler'>";
  p += "<table><tr><th>Enable</th><th>Start</th><th>End</th><th>Active Mode</th><th>Time-Expired End Mode</th></tr>";

  for (int i = 0; i < 4; i++) {
    p += "<tr>";
    p += "<td><select name='en" + String(i) + "'><option value='0'>No</option><option value='1'" + String(slots[i].enabled ? " selected" : "") + ">Yes</option></select></td>";
    p += "<td><input name='st" + String(i) + "' value='" + minToTime(slots[i].startMin) + "'></td>";
    p += "<td><input name='ed" + String(i) + "' value='" + minToTime(slots[i].endMin) + "'></td>";
    
    p += "<td><select name='md" + String(i) + "'>";
    String modes[4] = {"NONE", "UTILITY", "SOLAR", "SBU"};
    for (int m = 0; m < 4; m++) {
      p += "<option value='" + modes[m] + "'" + String(slots[i].mode == modes[m] ? " selected" : "") + ">" + modes[m] + "</option>";
    }
    p += "</select></td>";

    p += "<td><select name='em" + String(i) + "'>";
    for (int m = 0; m < 4; m++) {
      p += "<option value='" + modes[m] + "'" + String(slots[i].endMode == modes[m] ? " selected" : "") + ">" + modes[m] + "</option>";
    }
    p += "</select></td>";
    
    p += "</tr>";
  }

  p += "</table><button>Save Scheduler</button></form>";

  p += "<div class='sec'><b>Example:</b><br>06:00-09:00 UTILITY (End Mode: SOLAR)<br>09:00-17:00 SOLAR (End Mode: SBU)</div>";

  p += footer();
  server.send(200, "text/html", p);
}

void handleGatewayPage() {
  String p = header("Gateway / MQTT Config");

  p += "<form method='POST' action='/save-gateway'>";
  p += "<div class='sec'><h3>MQTT</h3>";
  p += "Device ID:<input name='deviceId' value='" + h(cfg.deviceId) + "'>";
  p += "MQTT Host:<input name='mqttHost' value='" + h(cfg.mqttHost) + "'>";
  p += "MQTT Port:<input name='mqttPort' value='" + String(cfg.mqttPort) + "'>";
  p += "MQTT Username:<input name='mqttUser' value='" + h(cfg.mqttUser) + "'>";
  p += "MQTT Password:<input name='mqttPass' type='password' value='" + h(cfg.mqttPass) + "'>";
  p += "Base Topic:<input name='baseTopic' value='" + h(cfg.baseTopic) + "'>";
  p += "</div>";

  p += "<div class='sec'><h3>Inverter / Commands</h3>";
  p += "Baud:<input name='baud' value='" + String(cfg.baudRate) + "'>";
  p += "Poll Interval ms:<input name='pollMs' value='" + String(cfg.pollMs) + "'>";
  p += "Utility Command:<input name='cmdUtility' value='" + h(cfg.cmdUtility) + "'>";
  p += "Solar Command:<input name='cmdSolar' value='" + h(cfg.cmdSolar) + "'>";
  p += "SBU Command:<input name='cmdSbu' value='" + h(cfg.cmdSbu) + "'>";
  p += "</div>";

  p += "<button>Save & Restart</button></form>";
  p += "<div class='sec'><a href='/reset-wifi'>Reset WiFi</a></div>";

  p += footer();
  server.send(200, "text/html", p);
}

void handleRefresh() {
  refreshInverterCache();
  String telemetry = parseQpigsJson(lastQpigs);
  if (telemetry.length() > 0) {
    publishMqtt(cfg.baseTopic + "/telemetry", telemetry);
    publishMqtt(cfg.baseTopic + "/raw", lastQpigs);
  }
  server.send(200, "text/plain", "OK");
}

void handleCmd() {
  if (server.hasArg("value")) {
    String cmd = server.arg("value");
    String resp = executeCommand(cmd, 2200);
    publishMqtt(cfg.baseTopic + "/command/result", "{\"command\":\"" + j(cmd) + "\",\"response\":\"" + j(resp) + "\"}");
  }
  server.send(200, "text/plain", "OK");
}

void handleApplySource() {
  String mode = server.arg("mode");
  String cmd = commandForMode(mode);
  String resp = executeCommand(cmd, 2200);
  addLog("Apply source " + mode + " => " + resp);
  delay(200);
  lastQpiri = executeCommand("QPIRI", 2500);
  server.send(200, "text/plain", resp);
}

void handleSendSetting() {
  String cmd = server.arg("cmd");
  String resp = executeCommand(cmd, 2200);
  addLog("Setting command " + cmd + " => " + resp);
  delay(200);
  lastQpiri = executeCommand("QPIRI", 2500);
  lastQflag = executeCommand("QFLAG", 1800);
  server.send(200, "text/plain", resp);
}

void handleSaveScheduler() {
  for (int i = 0; i < 4; i++) {
    slots[i].enabled = server.arg("en" + String(i)) == "1";
    slots[i].startMin = timeToMin(server.arg("st" + String(i)));
    slots[i].endMin = timeToMin(server.arg("ed" + String(i)));
    slots[i].mode = server.arg("md" + String(i));
    slots[i].endMode = server.arg("em" + String(i)); 
  }
  saveConfig();
  server.send(200, "text/html", "<meta charset='utf-8'><h2>Scheduler saved.</h2><a href='/scheduler'>Back</a>");
}

void handleSaveGateway() {
  cfg.deviceId = server.arg("deviceId");
  cfg.mqttHost = server.arg("mqttHost");
  cfg.mqttPort = server.arg("mqttPort").toInt();
  cfg.mqttUser = server.arg("mqttUser");
  cfg.mqttPass = server.arg("mqttPass");
  cfg.baseTopic = server.arg("baseTopic");
  cfg.baudRate = server.arg("baud").toInt();
  cfg.pollMs = server.arg("pollMs").toInt();
  cfg.cmdUtility = server.arg("cmdUtility");
  cfg.cmdSolar = server.arg("cmdSolar");
  cfg.cmdSbu = server.arg("cmdSbu");

  if (cfg.baseTopic == "") cfg.baseTopic = "inverter/" + cfg.deviceId;
  if (cfg.mqttPort <= 0) cfg.mqttPort = 1883;
  if (cfg.pollMs < 1000) cfg.pollMs = 1000;
  if (cfg.baudRate <= 0) cfg.baudRate = 2400;

  saveConfig();

  server.send(200, "text/html", "<meta charset='utf-8'><h2>Saved. Restarting...</h2>");
  delay(1200);
  ESP.restart();
}

void handleLog() { server.send(200, "text/plain", logBuffer); }
void handleClear() { logBuffer = ""; server.send(200, "text/plain", "OK"); }

void handleResetWifi() {
  server.send(200, "text/html", "<meta charset='utf-8'><h2>Resetting WiFi...</h2>");
  delay(1000);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

void handleOtaDone() {
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", Update.hasError() ? "OTA failed" : "<meta charset='utf-8'><h2>OTA success. Rebooting...</h2>");
  delay(1500);
  ESP.restart();
}

void handleOtaUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    addLog("OTA upload started: " + upload.filename);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) addLog("OTA complete. Size: " + String(upload.totalSize));
    else Update.printError(Serial);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  loadConfig();

  Serial2.begin(cfg.baudRate, SERIAL_8N1, RXD2, TXD2);

  WiFiManager wm;
  bool connected = wm.autoConnect("ESP32-Inverter-Setup", "12345678");
  if (!connected) {
    delay(2000);
    ESP.restart();
  }

  configTime(5 * 3600, 0, "pool.ntp.org", "time.google.com");

  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1400);

  server.on("/", handleRoot);
  server.on("/settings", handleSettingsPage);
  server.on("/save-nightfix", HTTP_POST, handleSaveNightFix); 
  server.on("/scheduler", handleSchedulerPage);
  server.on("/outages", handleOutagesPage); // === NEW: OUTAGES ROUTE ===
  server.on("/gateway", handleGatewayPage);
  server.on("/refresh", handleRefresh);
  server.on("/cmd", handleCmd);
  server.on("/apply-source", handleApplySource);
  server.on("/send-setting", handleSendSetting);
  server.on("/save-scheduler", HTTP_POST, handleSaveScheduler);
  server.on("/save-gateway", HTTP_POST, handleSaveGateway);
  server.on("/log", handleLog);
  server.on("/clear", handleClear);
  server.on("/reset-wifi", handleResetWifi);
  server.on("/ota", HTTP_POST, handleOtaDone, handleOtaUpload);
  server.on("/rawhex", handleRawHex);
  
  server.on("/start-timer", handleStartTimer);
  server.on("/stop-timer", handleStopTimer);

  server.begin();

  addLog("Gateway online");
  addLog("IP: http://" + WiFi.localIP().toString());
  addLog("UART RX=" + String(RXD2) + " TX=" + String(TXD2));
  addLog("Baud=" + String(cfg.baudRate));
  addLog("Voltronic/Axpert protocol active");

  refreshInverterCache();
}

void loop() {
  server.handleClient();

  if (cfg.mqttHost.length() > 0) {
    if (!mqtt.connected()) {
      if (millis() - lastMqttTry > 5000) {
        lastMqttTry = millis();
        reconnectMqtt();
      }
    } else {
      mqtt.loop();
    }
  }

  checkQuickTimer();

  if (millis() - lastPoll > cfg.pollMs) {
    lastPoll = millis();
    lastQpigs = executeCommand("QPIGS", 2200);
    lastQmod = executeCommand("QMOD", 1800);

    // === NEW: OUTAGE TRACKING LOGIC (Passive Observer) ===
    if (lastQpigs.length() > 10) {
      String t[35];
      int c = 0;
      splitTokens(lastQpigs, t, 35, c);
      if (c > 0) {
        float gridV = t[0].toFloat();
        bool currentGridState = (gridV > 50.0); // Safety threshold: >50V means Grid is ON
        
        static bool firstPollDone = false;
        if (!firstPollDone) {
          gridPresent = currentGridState;
          firstPollDone = true;
        } else {
          if (gridPresent && !currentGridState) {
            gridPresent = false;
            time_t now; time(&now);
            outages[outageHead].start = now;
            outages[outageHead].ongoing = true;
            addLog("⚠️ Grid offline. Load shedding started.");
          } else if (!gridPresent && currentGridState) {
            gridPresent = true;
            time_t now; time(&now);
            outages[outageHead].end = now;
            outages[outageHead].ongoing = false;
            outageHead = (outageHead + 1) % MAX_OUTAGES;
            if (outageCount < MAX_OUTAGES) outageCount++;
            addLog("⚡ Grid restored. Load shedding ended.");
          }
        }
      }
    }

    String telemetry = parseQpigsJson(lastQpigs);
    if (telemetry.length() > 0) {
      publishMqtt(cfg.baseTopic + "/telemetry", telemetry);
      publishMqtt(cfg.baseTopic + "/raw", lastQpigs);
    }
  }

  if (millis() - lastScheduleCheck > 30000) {
    lastScheduleCheck = millis();
    checkScheduler();
    checkNightGuard(); 
  }
}

String executeRawHex(String hexString, uint16_t timeoutMs) {
  while (Serial2.available()) Serial2.read();
  hexString.trim();
  addLog("RAW TX <- " + hexString);
  int start = 0;
  while (start < hexString.length()) {
    while (start < hexString.length() && hexString[start] == ' ')
      start++;
    int end = hexString.indexOf(' ', start);
    if (end == -1) end = hexString.length();
    String token = hexString.substring(start, end);
    token.trim();
    if (token.length()) {
      uint8_t b = (uint8_t)strtol(token.c_str(), nullptr, 16);
      Serial2.write(b);
    }
    start = end + 1;
  }
  uint8_t buf[350];
  int len = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs && len < sizeof(buf)) {
    while (Serial2.available() && len < sizeof(buf)) {
      buf[len++] = Serial2.read();
      t0 = millis();
    }
  }
  String rxHex = "";
  String rxAsc = "";
  for (int i = 0; i < len; i++) {
    rxHex += hx(buf[i]) + " ";
    if (buf[i] >= 32 && buf[i] <= 126)
      rxAsc += (char)buf[i];
    else if (buf[i] == '\r')
      rxAsc += "<CR>";
    else
      rxAsc += "[" + String(buf[i]) + "]";
  }
  addLog("RAW RX HEX <- " + rxHex);
  addLog("RAW RX ASC <- " + rxAsc);
  return rxAsc;
}

void handleRawHex() {
  String hex = server.arg("hex");
  String resp = executeRawHex(hex, 2500);
  server.send(200, "text/plain", resp);
}

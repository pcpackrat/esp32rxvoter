#include "WebUI.h"
#include "App.h"
#include "Config.h"
#include "Logger.h"
#include "NetManager.h"
#include "Version.h"
#include <ETH.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <math.h>
#include <time.h>

static WebServer webServer(80);

static const char *HTML_STYLE = R"rawliteral(
<style>
body{font-family:Arial,sans-serif;margin:0;padding:20px;background:#2c3e50;color:#ecf0f1}
.container{max-width:800px;margin:0 auto}
.card{background:#34495e;padding:20px;margin:10px 0;border-radius:8px;box-shadow:0 4px 6px rgba(0,0,0,0.3)}
h1{color:#3498db;margin-top:0}
h2{color:#ecf0f1;border-bottom:2px solid #3498db;padding-bottom:10px}
.btn{background:#3498db;color:#fff;border:none;padding:10px 20px;border-radius:4px;cursor:pointer;font-size:16px;margin:5px}
.btn:hover{background:#2980b9}
label{display:block;margin:15px 0 5px 0;font-weight:bold;color:#bdc3c7}
input[type="text"],input[type="password"],input[type="number"],select{width:100%;padding:10px;margin:5px 0;border-radius:4px;border:1px solid #7f8c8d;background:#ecf0f1;color:#2c3e50;box-sizing:border-box}
.nav{margin-bottom:20px}
.nav a{color:#3498db;text-decoration:none;margin-right:15px;font-weight:bold}
.nav a:hover{color:#ecf0f1}
.status{display:inline-block;padding:5px 10px;border-radius:4px}
.status.ok{background:#27ae60;color:#fff}
.status.warn{background:#f39c12;color:#fff}
table{width:100%;border-collapse:collapse}
td{padding:4px 0;vertical-align:top}
td:first-child{font-weight:bold;color:#bdc3c7;width:45%}
</style>
)rawliteral";

static String htmlEscape(const char *s) {
  String out;
  for (; *s; s++) {
    switch (*s) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    default:
      out += *s;
    }
  }
  return out;
}

static String jsonEscape(const char *s) {
  String out;
  for (; *s; s++) {
    if (*s == '"' || *s == '\\')
      out += '\\';
    if ((uint8_t)*s >= 0x20)
      out += *s;
  }
  return out;
}

static String htmlHeader(const char *title) {
  String html = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  html += "<title>RX Voter - " + String(title) + "</title>";
  html += HTML_STYLE;
  html += "<script>function toggleShow(id){var e=document.getElementById(id);e.type=(e.type==='password')?'text':'password';}</script>";
  html += "</head><body><div class=\"container\">";
  html += "<h1>" FIRMWARE_NAME "</h1>";
  html += "<div class=\"nav\"><a href=\"/\">Status</a><a href=\"/voter\">Voter</a><a href=\"/radio\">Radio</a><a href=\"/network\">Network</a><a href=\"/wifi\">WiFi</a><a href=\"/update\">Firmware</a></div>";
  return html;
}

static String htmlFooter() { return "</div></body></html>"; }

static void scheduleReboot() { g_rebootAt = millis() + 1000; }

// Post/Redirect/Get so a refresh doesn't resubmit the form
static void sendRedirectToRebooting() {
  webServer.sendHeader("Location", "/rebooting", true);
  webServer.send(302, "text/plain", "");
}

static String checkbox(const char *name, bool checked, const char *text) {
  return String("<label><input type=\"checkbox\" name=\"") + name + "\"" +
         (checked ? " checked" : "") + "> " + text + "</label>";
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------
static void handleRoot() {
  String html = htmlHeader("Status");
  html += "<div class=\"card\"><h2>Voter</h2><table>"
          "<tr><td>State</td><td id=\"voter\">-</td></tr>"
          "<tr><td>Host</td><td id=\"host\">-</td></tr>"
          "<tr><td>Audio frames sent</td><td id=\"sent\">-</td></tr>"
          "</table></div>";
  html += "<div class=\"card\"><h2>Receiver</h2><table>"
          "<tr><td>COS</td><td id=\"cos\">-</td></tr>"
          "<tr><td>RSSI byte</td><td id=\"rssi\">-</td></tr>"
          "<tr><td>DSP noise level</td><td id=\"noise\">-</td></tr>"
          "<tr><td>RSSI pin (raw ADC counts)</td><td id=\"rssiraw\">-</td></tr>"
          "<tr><td>ADC peak (1s)</td><td id=\"adcpk\">-</td></tr>"
          "<tr><td>Encoder peak (1s, 0 dB = uLaw clip)</td><td id=\"encpk\">-</td></tr>"
          "<tr><td>Sample clock vs GPS</td><td id=\"ppm\">-</td></tr>"
          "<tr><td>Frames captured / dropped / ADC errors / overruns</td><td id=\"frames\">-</td></tr>"
          "<tr><td>Input bias (OK: 1000-2400)</td><td id=\"dc\">-</td></tr>"
          "</table></div>";
  html += "<div class=\"card\"><h2>GPS</h2><table>"
          "<tr><td>Status</td><td id=\"gps\">-</td></tr>"
          "<tr><td>Satellites</td><td id=\"sats\">-</td></tr>"
          "<tr><td>UTC</td><td id=\"utc\">-</td></tr>"
          "<tr><td>PPS jitter</td><td id=\"jit\">-</td></tr>"
          "<tr><td>Location</td><td id=\"loc\">-</td></tr>"
          "</table></div>";
  html += "<div class=\"card\"><h2>Network</h2><table>"
          "<tr><td>Interface</td><td id=\"iface\">-</td></tr>"
          "<tr><td>IP</td><td id=\"ip\">-</td></tr>"
          "<tr><td>Link</td><td id=\"link\">-</td></tr>"
          "</table></div>";
  html += "<div class=\"card\"><h2>System</h2><table>"
          "<tr><td>Firmware</td><td>" FIRMWARE_VERSION " (" BUILD_DATE ")</td></tr>"
          "<tr><td>Uptime</td><td id=\"up\">-</td></tr>"
          "<tr><td>Free heap</td><td id=\"heap\">-</td></tr>"
          "</table></div>";
  html += "<script>"
          "function st(id,t,ok){var e=document.getElementById(id);"
          "if(ok===undefined){e.textContent=t;return;}"
          "e.innerHTML='<span class=\"status '+(ok?'ok':'warn')+'\">'+t+'</span>';}"
          "function db(v){return v<=-99?'none':v.toFixed(1)+' dB';}"
          "function poll(){fetch('/api/status').then(function(r){return r.json();}).then(function(s){"
          "st('voter',s.voter,s.voter.indexOf('CONNECTED')==0);st('host',s.host);st('sent',s.sent);"
          "st('cos',s.cos?'OPEN':'closed',s.cos);st('rssi',s.rssi+(s.hwrssi?' (hardware)':' (DSP)'));"
          "st('noise',s.noise);st('rssiraw',s.rssiraw);"
          "st('adcpk',db(s.adcpk)+' FS');st('encpk',db(s.encpk),s.encpk<-1);"
          "st('ppm',s.ppmok?s.ppm.toFixed(2)+' ppm':'measuring (needs GPS lock, ~60s)');"
          "st('frames',s.cap+' / '+s.drop+' / '+s.adcerr+' / '+s.ovr);"
          "st('dc',s.dc+' counts',s.dc>1000&&s.dc<2400);"
          "st('gps',s.gps,s.gps=='LOCKED');st('sats',s.sats);st('utc',s.utc);st('jit',s.jit+' us');"
          "st('loc',s.lat+', '+s.lon+' (elev '+s.elev+' m)');"
          "st('iface',s.iface,s.iface=='ETH');st('ip',s.ip);st('link',s.link);"
          "st('up',s.up+' s');st('heap',s.heap+' bytes');"
          "}).catch(function(){}).finally(function(){setTimeout(poll,1000);});}"
          "poll();</script>";
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

static float peakDb(int16_t peak, float ref) {
  if (peak <= 0)
    return -99.0f;
  return 20.0f * log10f((float)peak / ref);
}

static void handleApiStatus() {
  char voterState[96];
  voter.statusText(voterState, sizeof(voterState));
  char lat[16] = "", lon[16] = "", elev[16] = "";
  gps.getGPSStrings(lat, lon, elev);

  char utc[24] = "-";
  if (gps.isTimeSet()) {
    time_t tt = (time_t)gps.getEpoch();
    struct tm tm;
    gmtime_r(&tt, &tm);
    strftime(utc, sizeof(utc), "%Y-%m-%d %H:%M:%S", &tm);
  }

  String link = "-";
  if (net.mode() == NetMode::ETH_ACTIVE) {
    link = String(ETH.linkSpeed()) + " Mbps " + (ETH.fullDuplex() ? "full" : "half");
  } else if (net.mode() == NetMode::WIFI_ACTIVE) {
    link = String(WiFi.RSSI()) + " dBm";
  } else if (net.mode() == NetMode::AP_SETUP) {
    link = "Setup hotspot \"" AP_SSID "\", " + String(net.apSecondsLeft()) + " s left";
  }

  String host = (cfg.data.hostname[0] != '\0') ? String(cfg.data.hostname) + " (" + voter.getHostIP().toString() + ")"
                                               : voter.getHostIP().toString();
  host += ":" + String(cfg.data.hostPort);

  char buf[1024];
  snprintf(buf, sizeof(buf),
           "{\"voter\":\"%s\",\"host\":\"%s\",\"sent\":%lu,"
           "\"cos\":%s,\"rssi\":%u,\"hwrssi\":%s,\"rssiraw\":%u,\"noise\":%u,\"adcpk\":%.1f,\"encpk\":%.1f,"
           "\"ppm\":%.2f,\"ppmok\":%s,\"cap\":%lu,\"drop\":%lu,\"adcerr\":%lu,\"ovr\":%lu,\"dc\":%d,"
           "\"gps\":\"%s\",\"sats\":%lu,\"utc\":\"%s\",\"jit\":%lu,"
           "\"lat\":\"%s\",\"lon\":\"%s\",\"elev\":\"%s\","
           "\"iface\":\"%s\",\"ip\":\"%s\",\"link\":\"%s\",\"up\":%lu,\"heap\":%lu}",
           jsonEscape(voterState).c_str(), jsonEscape(host.c_str()).c_str(),
           (unsigned long)voter.getAudioFramesSent(), g_status.cos ? "true" : "false",
           (unsigned)g_status.rssi, cfg.data.useHwRSSI ? "true" : "false",
           (unsigned)g_status.rssiRaw, (unsigned)g_status.noise, peakDb(g_status.adcPeak, 32768.0f),
           peakDb(g_status.encPeak, (float)ULAW_CLIP_LEVEL), (double)g_status.clockPpm,
           g_status.clockPpmValid ? "true" : "false", (unsigned long)g_status.framesCaptured,
           (unsigned long)g_status.framesDropped, (unsigned long)g_status.readErrors,
           (unsigned long)g_adcOverruns(), (int)g_status.adcDc,
           GPSManager::statusStr(gps.getLockStatus()),
           (unsigned long)gps.getSatellites(), utc, (unsigned long)gps.getPpsJitter(), lat, lon,
           jsonEscape(elev).c_str(), net.modeStr(), net.localIP().toString().c_str(),
           jsonEscape(link.c_str()).c_str(),
           (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap());
  webServer.send(200, "application/json", buf);
}

static void handleRebootingPage() {
  String html = htmlHeader("Rebooting");
  html += "<meta http-equiv=\"refresh\" content=\"10;url=/\">";
  html += "<div class=\"card\"><h2>Saved</h2><p>Rebooting... this page returns to Status in 10 s.</p></div>";
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

// ---------------------------------------------------------------------------
// Voter
// ---------------------------------------------------------------------------
static void handleVoter() {
  String hostDisplay = (cfg.data.hostname[0] != '\0') ? String(cfg.data.hostname)
                                                     : cfg.hostIP().toString();
  String html = htmlHeader("Voter");
  html += "<div class=\"card\"><h2>Voter Settings</h2><form method=\"POST\" action=\"/voter\">";
  html += "<label>Host Address (IP or hostname):</label><input type=\"text\" name=\"host\" value=\"" + htmlEscape(hostDisplay.c_str()) + "\">";
  html += "<label>Host Port:</label><input type=\"number\" name=\"port\" min=\"1\" max=\"65535\" value=\"" + String(cfg.data.hostPort) + "\">";
  html += "<label>Client Password:</label><input type=\"password\" id=\"clientPwd\" name=\"clientPwd\" maxlength=\"19\" value=\"" + htmlEscape(cfg.data.clientPwd) + "\">";
  html += "<label><input type=\"checkbox\" onclick=\"toggleShow('clientPwd')\"> Show</label>";
  html += "<label>Host Password:</label><input type=\"password\" id=\"hostPwd\" name=\"hostPwd\" maxlength=\"19\" value=\"" + htmlEscape(cfg.data.hostPwd) + "\">";
  html += "<label><input type=\"checkbox\" onclick=\"toggleShow('hostPwd')\"> Show</label>";
  html += "<label>Timing Offset (ms):</label><input type=\"number\" name=\"timingOffset\" min=\"-1000\" max=\"1000\" value=\"" + String(cfg.data.timingOffsetMs) + "\">";
  html += "<input type=\"submit\" value=\"Save &amp; Reboot\" class=\"btn\"></form></div>";
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

static void handleVoterPost() {
  // IP literal sets hostIP and clears hostname; anything else is a hostname
  IPAddress hostIp;
  String hostArg = webServer.arg("host");
  hostArg.trim();
  if (hostIp.fromString(hostArg)) {
    cfg.data.hostIP = (uint32_t)hostIp;
    cfg.data.hostname[0] = '\0';
  } else {
    strlcpy(cfg.data.hostname, hostArg.c_str(), sizeof(cfg.data.hostname));
  }
  cfg.data.hostPort = (uint16_t)constrain(webServer.arg("port").toInt(), 1, 65535);
  strlcpy(cfg.data.clientPwd, webServer.arg("clientPwd").c_str(), sizeof(cfg.data.clientPwd));
  strlcpy(cfg.data.hostPwd, webServer.arg("hostPwd").c_str(), sizeof(cfg.data.hostPwd));
  cfg.data.timingOffsetMs = (int16_t)constrain(webServer.arg("timingOffset").toInt(), -1000, 1000);
  cfg.save();
  scheduleReboot();
  sendRedirectToRebooting();
}

// ---------------------------------------------------------------------------
// Radio
// ---------------------------------------------------------------------------
static void handleRadio() {
  String html = htmlHeader("Radio");
  html += "<div class=\"card\"><h2>Squelch</h2><form method=\"POST\" action=\"/radio\">";
  html += "<label>COS Mode:</label><select name=\"cosMode\">";
  html += String("<option value=\"0\"") + (cfg.data.cosMode == COS_MODE_ALWAYS_ON ? " selected" : "") + ">Always On (testing)</option>";
  html += String("<option value=\"1\"") + (cfg.data.cosMode == COS_MODE_HARDWARE ? " selected" : "") + ">Hardware (COS input)</option>";
  html += String("<option value=\"2\"") + (cfg.data.cosMode == COS_MODE_DSP ? " selected" : "") + ">DSP noise squelch</option>";
  html += "</select>";
  html += checkbox("cosInvert", cfg.data.cosInvert, "Invert COS (checked = active HIGH)");
  html += "<label>DSP Squelch Threshold (0-255, noise level above this = closed):</label><input type=\"number\" min=\"0\" max=\"255\" name=\"dspSquelch\" value=\"" + String(cfg.data.dspSquelchThresh) + "\">";
  html += "<label>DSP RSSI Calibration (higher = more noise before RSSI drops):</label><input type=\"number\" step=\"0.1\" min=\"1\" max=\"1000\" name=\"dspCalib\" value=\"" + String(cfg.data.dspCalib, 1) + "\">";

  html += "<h2>RSSI</h2>";
  html += checkbox("useHwRssi", cfg.data.useHwRSSI, "Use Hardware RSSI (IO39 ADC). Unchecked = DSP noise RSSI");
  html += "<label>RSSI Min (raw counts at no signal, 0-4095):</label><input type=\"number\" min=\"0\" max=\"4095\" name=\"rssiMin\" value=\"" + String(cfg.data.rssiMin) + "\">";
  html += "<label>RSSI Max (raw counts at full quieting, 0-4095):</label><input type=\"number\" min=\"0\" max=\"4095\" name=\"rssiMax\" value=\"" + String(cfg.data.rssiMax) + "\">";
  html += "<p>Read the raw counts from the Status page with no signal and with a strong signal.</p>";

  html += "<h2>Receive Audio</h2>";
  html += "<p>Set the analog level with the input divider first, then trim with Digital Gain.</p>";
  html += "<label>Digital Gain (%):</label><input type=\"number\" min=\"0\" max=\"255\" name=\"rxDigitalGain\" value=\"" + String(cfg.data.rxDigitalGainPct) + "\">";
  html += checkbox("deemp", cfg.data.enableDeemp, "De-emphasis (use with flat/discriminator audio)");
  html += checkbox("plFilter", cfg.data.enablePLFilter, "PL Filter (removes CTCSS tones below 290 Hz)");
  html += "<input type=\"submit\" value=\"Save &amp; Reboot\" class=\"btn\"></form></div>";
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

static void handleRadioPost() {
  cfg.data.cosMode = (uint8_t)constrain(webServer.arg("cosMode").toInt(), 0, 2);
  cfg.data.cosInvert = webServer.hasArg("cosInvert");
  cfg.data.dspSquelchThresh = (uint8_t)constrain(webServer.arg("dspSquelch").toInt(), 0, 255);
  float calib = webServer.arg("dspCalib").toFloat();
  cfg.data.dspCalib = (calib < 1.0f) ? 1.0f : calib;
  cfg.data.useHwRSSI = webServer.hasArg("useHwRssi");
  cfg.data.rssiMin = (uint16_t)constrain(webServer.arg("rssiMin").toInt(), 0, 4095);
  cfg.data.rssiMax = (uint16_t)constrain(webServer.arg("rssiMax").toInt(), 0, 4095);
  cfg.data.rxDigitalGainPct = (uint8_t)constrain(webServer.arg("rxDigitalGain").toInt(), 0, 255);
  cfg.data.enableDeemp = webServer.hasArg("deemp");
  cfg.data.enablePLFilter = webServer.hasArg("plFilter");
  cfg.save();
  scheduleReboot();
  sendRedirectToRebooting();
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------
static void handleNetwork() {
  String html = htmlHeader("Network");
  html += "<div class=\"card\"><h2>Ethernet Addressing</h2><form method=\"POST\" action=\"/network\">";
  html += checkbox("useStaticIP", cfg.data.useStaticIP, "Use Static IP (unchecked = DHCP)");
  html += "<label>Static IP:</label><input type=\"text\" name=\"staticIP\" value=\"" + IPAddress(cfg.data.staticIP).toString() + "\">";
  html += "<label>Subnet Mask:</label><input type=\"text\" name=\"subnetMask\" value=\"" + IPAddress(cfg.data.subnetMask).toString() + "\">";
  html += "<label>Gateway:</label><input type=\"text\" name=\"gateway\" value=\"" + IPAddress(cfg.data.gateway).toString() + "\">";
  html += "<label>DNS (used with Static IP):</label><input type=\"text\" name=\"staticDNS\" value=\"" + IPAddress(cfg.data.staticDNS).toString() + "\">";
  html += "<label>DNS Override (used with DHCP; 0.0.0.0 = DHCP default):</label><input type=\"text\" name=\"dns\" value=\"" + IPAddress(cfg.data.dnsServerIP).toString() + "\">";
  html += "<h2>Syslog</h2>";
  html += checkbox("useSyslog", cfg.data.useSyslog, "Enable Syslog");
  html += "<label>Syslog Host (IP or hostname):</label><input type=\"text\" name=\"syslogHost\" value=\"" + htmlEscape(cfg.data.syslogHost) + "\">";
  html += "<label>Syslog Port:</label><input type=\"number\" name=\"syslogPort\" min=\"1\" max=\"65535\" value=\"" + String(cfg.data.syslogPort) + "\">";
  html += "<input type=\"submit\" value=\"Save &amp; Reboot\" class=\"btn\"></form></div>";
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

static void handleNetworkPost() {
  cfg.data.useStaticIP = webServer.hasArg("useStaticIP");
  IPAddress ip;
  if (ip.fromString(webServer.arg("staticIP")))
    cfg.data.staticIP = (uint32_t)ip;
  if (ip.fromString(webServer.arg("subnetMask")))
    cfg.data.subnetMask = (uint32_t)ip;
  if (ip.fromString(webServer.arg("gateway")))
    cfg.data.gateway = (uint32_t)ip;
  if (ip.fromString(webServer.arg("staticDNS")))
    cfg.data.staticDNS = (uint32_t)ip;
  if (ip.fromString(webServer.arg("dns")))
    cfg.data.dnsServerIP = (uint32_t)ip;
  cfg.data.useSyslog = webServer.hasArg("useSyslog");
  String sh = webServer.arg("syslogHost");
  sh.trim();
  strlcpy(cfg.data.syslogHost, sh.c_str(), sizeof(cfg.data.syslogHost));
  cfg.data.syslogPort = (uint16_t)constrain(webServer.arg("syslogPort").toInt(), 1, 65535);
  cfg.save();
  scheduleReboot();
  sendRedirectToRebooting();
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static void handleWiFi() {
  String html = htmlHeader("WiFi");
  if (net.mode() == NetMode::AP_SETUP) {
    html += "<div class=\"card\"><h2><span class=\"status warn\">Setup Hotspot Active</span></h2>";
    html += "<p>No Ethernet or working WiFi was found, so this device is broadcasting its own \"" AP_SSID "\" network. "
            "Enter your network below and Save - the device will reboot and try to connect.</p>";
    html += "<p>Gives up in " + String(net.apSecondsLeft()) + "s if nothing is saved.</p></div>";
  }
  html += "<div class=\"card\"><h2>WiFi Fallback</h2>";
  html += "<p>Used only if Ethernet has no cable link for " + String(NetManager::ETH_LINK_DOWN_FAILOVER_MS / 1000) +
          "s, or has link but no IP for " + String(NetManager::ETH_NO_IP_FAILOVER_MS / 1000) +
          "s. Ethernet is always preferred.</p>";
  html += "<form method=\"POST\" action=\"/wifi\">";
  html += "<button type=\"button\" class=\"btn\" onclick=\"scanWifi()\">Scan for Networks</button>";
  html += "<div id=\"scanStatus\"></div>";
  html += "<select id=\"scanResults\" style=\"display:none\" onchange=\"document.getElementById('ssidInput').value=this.value\"></select>";
  html += "<label>SSID:</label><input type=\"text\" id=\"ssidInput\" name=\"ssid\" maxlength=\"32\" value=\"" + htmlEscape(cfg.data.wifiSSID) + "\">";
  html += "<label>Password:</label><input type=\"password\" id=\"wifiPass\" name=\"pass\" maxlength=\"64\" value=\"" + htmlEscape(cfg.data.wifiPass) + "\">";
  html += "<label><input type=\"checkbox\" onclick=\"toggleShow('wifiPass')\"> Show</label>";
  html += "<input type=\"submit\" value=\"Save &amp; Reboot\" class=\"btn\"></form></div>";
  html += "<script>";
  html += "function scanWifi(){document.getElementById('scanStatus').innerText='Scanning...';fetch('/wifi/scan',{method:'POST'}).then(pollScan);}";
  html += "function pollScan(){fetch('/wifi/scan_results').then(function(r){return r.text();}).then(function(text){"
          "if(text==='running'){setTimeout(pollScan,1000);return;}"
          "var networks=text.split('\\n').filter(function(s){return s.length>0;});"
          "var sel=document.getElementById('scanResults');"
          "sel.innerHTML='<option value=\"\">-- Select a network --</option>';"
          "networks.forEach(function(ssid){var opt=document.createElement('option');opt.value=ssid;opt.text=ssid;sel.appendChild(opt);});"
          "sel.style.display=networks.length?'block':'none';"
          "document.getElementById('scanStatus').innerText=networks.length?(networks.length+' networks found'):'No networks found';"
          "});}";
  html += "</script>";
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

static void handleWifiScanStart() {
  if (net.mode() == NetMode::AP_SETUP) {
    WiFi.mode(WIFI_AP_STA); // keep the hotspot up while scanning
  } else if (WiFi.getMode() == WIFI_OFF) {
    WiFi.mode(WIFI_STA);
  }
  WiFi.scanNetworks(true); // async
  webServer.send(200, "text/plain", "started");
}

static void handleWifiScanResults() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    webServer.send(200, "text/plain", "running");
    return;
  }
  String out;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0)
      continue;
    out += ssid + "\n";
  }
  WiFi.scanDelete();
  if (net.mode() == NetMode::AP_SETUP) {
    WiFi.mode(WIFI_AP);
  } else if (net.mode() != NetMode::WIFI_ACTIVE) {
    WiFi.mode(WIFI_OFF);
  }
  webServer.send(200, "text/plain", out);
}

static void handleWiFiPost() {
  strlcpy(cfg.data.wifiSSID, webServer.arg("ssid").c_str(), sizeof(cfg.data.wifiSSID));
  strlcpy(cfg.data.wifiPass, webServer.arg("pass").c_str(), sizeof(cfg.data.wifiPass));
  cfg.save();
  scheduleReboot();
  sendRedirectToRebooting();
}

// ---------------------------------------------------------------------------
// Firmware update
// ---------------------------------------------------------------------------
static void handleUpdate() {
  String html = htmlHeader("Firmware");
  html += "<div class=\"card\"><h2>Firmware Update</h2><table>"
          "<tr><td>Running</td><td>" FIRMWARE_VERSION " (" BUILD_DATE ")</td></tr></table>";
  html += "<form method=\"POST\" action=\"/update\" enctype=\"multipart/form-data\" "
          "onsubmit=\"document.getElementById('upBtn').disabled=true;"
          "document.getElementById('upMsg').innerText='Uploading - do not power off...';\">";
  html += "<label>Firmware file (.pio/build/&lt;env&gt;/firmware.bin):</label>"
          "<input type=\"file\" name=\"firmware\" accept=\".bin\" required>";
  html += "<input type=\"submit\" id=\"upBtn\" value=\"Upload &amp; Reboot\" class=\"btn\"></form>";
  html += "<p id=\"upMsg\"></p></div>";
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

// Called per upload chunk. Writes to the inactive OTA slot; the running
// firmware stays bootable until Update.end() verifies the new image.
static void handleUpdateUpload() {
  HTTPUpload &up = webServer.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (Update.isRunning())
      Update.abort();
    logger.info("OTA", "Upload start: %s", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
      logger.error("OTA", "Begin failed: %s", Update.errorString());
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.isRunning() && Update.write(up.buf, up.currentSize) != up.currentSize)
      logger.error("OTA", "Write failed: %s", Update.errorString());
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.isRunning()) {
      if (Update.end(true))
        logger.info("OTA", "Upload OK, %u bytes", (unsigned)up.totalSize);
      else
        logger.error("OTA", "Verify failed: %s", Update.errorString());
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    logger.warn("OTA", "Upload aborted");
  }
}

static void handleUpdateDone() {
  if (Update.isFinished() && !Update.hasError()) {
    scheduleReboot();
    sendRedirectToRebooting();
    return;
  }
  String reason = Update.hasError() ? String(Update.errorString()) : String("No file received");
  String html = htmlHeader("Firmware");
  html += "<div class=\"card\"><h2><span class=\"status warn\">Update Failed</span></h2>";
  html += "<p>" + htmlEscape(reason.c_str()) + "</p><p>The running firmware is unchanged.</p>";
  html += "<a href=\"/update\" class=\"btn\">Back</a></div>";
  html += htmlFooter();
  webServer.send(500, "text/html", html);
}

// ---------------------------------------------------------------------------

static void redirectToSetup() {
  webServer.sendHeader("Location", "http://" + net.apIP().toString() + "/wifi", true);
  webServer.send(302);
}

void webBegin() {
  webServer.on("/", handleRoot);
  webServer.on("/api/status", HTTP_GET, handleApiStatus);
  webServer.on("/voter", HTTP_GET, handleVoter);
  webServer.on("/voter", HTTP_POST, handleVoterPost);
  webServer.on("/radio", HTTP_GET, handleRadio);
  webServer.on("/radio", HTTP_POST, handleRadioPost);
  webServer.on("/network", HTTP_GET, handleNetwork);
  webServer.on("/network", HTTP_POST, handleNetworkPost);
  webServer.on("/wifi", HTTP_GET, handleWiFi);
  webServer.on("/wifi", HTTP_POST, handleWiFiPost);
  webServer.on("/wifi/scan", HTTP_POST, handleWifiScanStart);
  webServer.on("/wifi/scan_results", HTTP_GET, handleWifiScanResults);
  webServer.on("/rebooting", HTTP_GET, handleRebootingPage);
  webServer.on("/update", HTTP_GET, handleUpdate);
  webServer.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);

  // Captive-portal probes - make phones/laptops pop up the setup page
  webServer.on("/generate_204", redirectToSetup);
  webServer.on("/gen_204", redirectToSetup);
  webServer.on("/hotspot-detect.html", redirectToSetup);
  webServer.on("/library/test/success.html", redirectToSetup);
  webServer.on("/connecttest.txt", []() { webServer.send(200, "text/plain", "Microsoft Connect Test"); });
  webServer.on("/ncsi.txt", []() { webServer.send(200, "text/plain", "Microsoft NCSI"); });
  webServer.onNotFound([]() {
    if (net.mode() == NetMode::AP_SETUP) {
      redirectToSetup();
    } else {
      webServer.send(404, "text/plain", "Not found");
    }
  });

  webServer.begin();
  Serial.println("[Web] Server started on port 80");
}

void webHandle() { webServer.handleClient(); }

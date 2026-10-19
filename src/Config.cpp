#include "Config.h"
#include "Defaults.h"
#include <Preferences.h>

ConfigManager cfg;

static Preferences prefs;

void ConfigManager::begin() {
  prefs.begin("rxvoter", false);
  size_t len = prefs.getBytesLength("cfg");
  if (len == sizeof(data)) {
    prefs.getBytes("cfg", &data, sizeof(data));
  }
  if (len != sizeof(data) || data.magic != CONFIG_MAGIC ||
      data.version != CONFIG_VERSION) {
    Serial.println("[Config] No valid config in NVS - loading defaults");
    resetDefaults();
  } else {
    Serial.println("[Config] Loaded from NVS");
  }
}

void ConfigManager::save() {
  data.magic = CONFIG_MAGIC;
  data.version = CONFIG_VERSION;
  prefs.putBytes("cfg", &data, sizeof(data));
  Serial.println("[Config] Saved to NVS");
}

void ConfigManager::resetDefaults() {
  memset(&data, 0, sizeof(data));

  // All values come from Defaults.h (site values from Site.h)

  // Voter. DEF_HOST: an IP literal sets hostIP, anything else is a hostname (as the web UI)
  IPAddress hostIp;
  if (hostIp.fromString(DEF_HOST)) {
    data.hostIP = (uint32_t)hostIp;
  } else {
    strlcpy(data.hostname, DEF_HOST, sizeof(data.hostname));
  }
  data.hostPort = DEF_HOST_PORT;
  strlcpy(data.clientPwd, DEF_CLIENT_PWD, sizeof(data.clientPwd));
  strlcpy(data.hostPwd, DEF_HOST_PWD, sizeof(data.hostPwd));
  data.timingOffsetMs = DEF_TIMING_OFFSET_MS;

  // Radio RX
  data.cosMode = DEF_COS_MODE;
  data.cosInvert = DEF_COS_INVERT;
  data.dspSquelchThresh = DEF_DSP_SQUELCH_THRESH;
  data.dspCalib = DEF_DSP_CALIB;
  data.useHwRSSI = DEF_USE_HW_RSSI;
  data.rssiMin = DEF_RSSI_MIN;
  data.rssiMax = DEF_RSSI_MAX;
  data.rxDigitalGainPct = DEF_RX_DIGITAL_GAIN_PCT;
  data.enablePLFilter = DEF_PL_FILTER;
  data.enableDeemp = DEF_DEEMPHASIS;

  // Network
  data.useStaticIP = DEF_USE_STATIC_IP;
  data.staticIP = (uint32_t)IPAddress(DEF_STATIC_IP);
  data.subnetMask = (uint32_t)IPAddress(DEF_SUBNET_MASK);
  data.gateway = (uint32_t)IPAddress(DEF_GATEWAY);
  data.staticDNS = (uint32_t)IPAddress(DEF_STATIC_DNS);
  data.dnsServerIP = 0;
  strlcpy(data.wifiSSID, DEF_WIFI_SSID, sizeof(data.wifiSSID));
  strlcpy(data.wifiPass, DEF_WIFI_PASS, sizeof(data.wifiPass));

  // Syslog
  data.useSyslog = DEF_USE_SYSLOG;
  strlcpy(data.syslogHost, DEF_SYSLOG_HOST, sizeof(data.syslogHost));
  data.syslogPort = DEF_SYSLOG_PORT;

  save();
  Serial.println("[Config] Reset to defaults");
}

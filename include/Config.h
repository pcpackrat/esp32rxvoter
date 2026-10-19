#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <IPAddress.h>

#define CONFIG_MAGIC 0x52585654 // "RXVT"
#define CONFIG_VERSION 2

// COS/Squelch Modes
#define COS_MODE_ALWAYS_ON 0 // Always send (testing/no squelch)
#define COS_MODE_HARDWARE 1  // COS input pin
#define COS_MODE_DSP 2       // DSP noise squelch

struct SysConfig {
  uint32_t magic;
  uint32_t version;

  // Voter
  char hostname[64]; // Takes priority over hostIP when set
  uint32_t hostIP;
  uint16_t hostPort;
  char clientPwd[20];
  char hostPwd[20];
  int16_t timingOffsetMs;

  // Radio RX
  uint8_t cosMode;
  bool cosInvert;
  uint8_t dspSquelchThresh;
  float dspCalib;
  bool useHwRSSI;    // RSSI from the IO39 ADC instead of DSP noise
  uint16_t rssiMin;  // Raw ADC counts (0-4095) mapped to RSSI 0
  uint16_t rssiMax;  // Raw ADC counts mapped to RSSI 127
  uint8_t rxDigitalGainPct;
  bool enablePLFilter;
  bool enableDeemp;

  // Network
  bool useStaticIP;
  uint32_t staticIP;
  uint32_t subnetMask;
  uint32_t gateway;
  uint32_t staticDNS;
  uint32_t dnsServerIP; // DNS override when using DHCP (0 = DHCP default)
  char wifiSSID[33];
  char wifiPass[65];

  // Syslog
  bool useSyslog;
  char syslogHost[64]; // IP or hostname
  uint16_t syslogPort;
};

class ConfigManager {
public:
  void begin();
  void save();
  void resetDefaults();

  IPAddress hostIP() const { return IPAddress(data.hostIP); }

  SysConfig data;
};

extern ConfigManager cfg;

#endif

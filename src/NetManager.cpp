#include "NetManager.h"
#include "Config.h"
#include "Pins.h"
#include <DNSServer.h>
#include <ETH.h>
#include <WiFi.h>

NetManager net;

static DNSServer dnsServer;

static const uint32_t WIFI_RETRY_MS = 30000;
static const uint32_t ETH_LINK_UP_DEBOUNCE_MS = 500;
static const uint32_t ETH_FLAP_WINDOW_MS = 20000;
static const uint8_t ETH_FLAP_THRESHOLD = 5;
static const uint32_t ETH_REINIT_MAX = 3;
static const uint32_t AP_TRIGGER_MS = 15000;  // No network this long -> setup hotspot
static const uint32_t AP_TIMEOUT_MS = 300000; // 5 minutes, then give up

// Set from the network event task
static volatile uint32_t s_ethFlapCount = 0;
static volatile bool s_ethReinitRequested = false;
static volatile uint32_t s_flapWindowStart = 0;

void NetManager::begin() {
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.printf("[ETH-EVT] %lu START\r\n", millis());
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.printf("[ETH-EVT] %lu CONNECTED (link up)\r\n", millis());
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED: {
      Serial.printf("[ETH-EVT] %lu DISCONNECTED (link down)\r\n", millis());
      uint32_t now = millis();
      if (now - s_flapWindowStart > ETH_FLAP_WINDOW_MS) {
        s_flapWindowStart = now;
        s_ethFlapCount = 0;
      }
      s_ethFlapCount = s_ethFlapCount + 1;
      if (s_ethFlapCount >= ETH_FLAP_THRESHOLD) {
        s_ethReinitRequested = true;
      }
      break;
    }
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.printf("[ETH-EVT] %lu GOT_IP %s\r\n", millis(), ETH.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.printf("[ETH-EVT] %lu LOST_IP\r\n", millis());
      break;
    default:
      break;
    }
  });

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH); // off (active-low)

  _startEth();
  _lastEthUp = millis(); // failover grace period starts here
  Serial.println("[NET] Ethernet started");
}

void NetManager::_startEth() {
  // Explicit LAN8720 reset pulse. The IDF driver's own pulse is only 150us,
  // too short after a warm reset (PHY power never dropped) - caused link
  // flapping after reflash on the TeensyVoter WT32-ETH01.
  pinMode(WT32_ETH_PHY_POWER, OUTPUT);
  digitalWrite(WT32_ETH_PHY_POWER, LOW);
  delay(50);
  digitalWrite(WT32_ETH_PHY_POWER, HIGH);
  delay(10);

  ETH.begin(ETH_PHY_LAN8720, WT32_ETH_PHY_ADDR, WT32_ETH_PHY_MDC, WT32_ETH_PHY_MDIO,
            WT32_ETH_PHY_POWER, ETH_CLOCK_GPIO0_IN);

  if (cfg.data.useStaticIP) {
    ETH.config(IPAddress(cfg.data.staticIP), IPAddress(cfg.data.gateway),
               IPAddress(cfg.data.subnetMask), IPAddress(cfg.data.staticDNS));
  }
}

void NetManager::_startApSetup() {
  Serial.println("[NET] No network for a while - starting WiFi setup hotspot");
  _mode = NetMode::AP_SETUP;
  _apStartTime = millis();

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAPConfig(_apIP, _apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, "", 1, false, 4); // open, channel 1, max 4 clients

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", _apIP); // captive portal

  digitalWrite(PIN_LED, LOW); // on
}

void NetManager::_stopApSetup(NetMode nextMode) {
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  if (nextMode != NetMode::WIFI_ACTIVE) {
    WiFi.mode(WIFI_OFF);
  }
  _mode = nextMode;
  digitalWrite(PIN_LED, HIGH); // off
}

void NetManager::update() {
  // Flap self-heal: full PHY re-init, capped
  if (s_ethReinitRequested) {
    s_ethReinitRequested = false;
    s_ethFlapCount = 0;
    if (_ethReinitCount < ETH_REINIT_MAX) {
      _ethReinitCount++;
      Serial.printf("[NET] ETH link flapping (#%lu) - re-initializing PHY\r\n",
                    (unsigned long)_ethReinitCount);
      ETH.end();
      delay(200);
      _startEth();
      _lastEthUp = millis();
    } else {
      Serial.println("[NET] ETH flap recovery attempts exhausted - WiFi fallback will take over if configured");
    }
  }

  bool phyLinkUp = ETH.linkUp();
  if (phyLinkUp) {
    if (_phyLinkUpSince == 0)
      _phyLinkUpSince = millis();
    _linkDownSince = 0;
  } else {
    _phyLinkUpSince = 0;
    if (_linkDownSince == 0)
      _linkDownSince = millis();
  }

  bool eUp = phyLinkUp && (uint32_t)ETH.localIP() != 0;
  if (eUp)
    _lastEthUp = millis();

  // ETH link back: drop WiFi right away (don't run both radios at once)
  if (_mode == NetMode::WIFI_ACTIVE && phyLinkUp &&
      (millis() - _phyLinkUpSince > ETH_LINK_UP_DEBOUNCE_MS)) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    _mode = NetMode::NONE;
    _lastEthUp = millis(); // fresh DHCP grace period
    Serial.println("[NET] Ethernet link restored - dropped WiFi fallback");
  }

  if (_mode == NetMode::AP_SETUP && eUp) {
    Serial.println("[NET] Ethernet available - exiting WiFi setup hotspot");
    _stopApSetup(NetMode::ETH_ACTIVE);
  }

  if (eUp) {
    _mode = NetMode::ETH_ACTIVE;
  } else {
    if (_mode == NetMode::ETH_ACTIVE) {
      _mode = NetMode::NONE;
    }
    if (_mode == NetMode::WIFI_ACTIVE && WiFi.status() != WL_CONNECTED) {
      _mode = NetMode::NONE;
    }
    if (_mode == NetMode::NONE && WiFi.status() == WL_CONNECTED) {
      _mode = NetMode::WIFI_ACTIVE;
      Serial.printf("[NET] WiFi connected: %s\r\n", WiFi.localIP().toString().c_str());
    }

    bool linkDownFailover = !phyLinkUp && (millis() - _linkDownSince > ETH_LINK_DOWN_FAILOVER_MS);
    bool noIpFailover = phyLinkUp && (millis() - _lastEthUp > ETH_NO_IP_FAILOVER_MS);

    if (_mode == NetMode::NONE && cfg.data.wifiSSID[0] != '\0' &&
        (linkDownFailover || noIpFailover) &&
        (!_wifiAttempted || millis() - _lastWifiAttempt > WIFI_RETRY_MS)) {
      _wifiAttempted = true;
      _lastWifiAttempt = millis();
      Serial.println("[NET] Ethernet down, attempting WiFi fallback...");
      WiFi.persistent(false); // don't write credentials to NVS on every retry
      WiFi.disconnect(true);
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(false); // modem sleep adds latency/jitter
      WiFi.begin(cfg.data.wifiSSID, cfg.data.wifiPass);
    }
  }

  // Setup hotspot if there is no network at all for a while. One-shot per boot.
  if (!_apGivenUp) {
    if (_mode == NetMode::NONE) {
      if (_noNetworkSince == 0) {
        _noNetworkSince = millis();
      } else if (millis() - _noNetworkSince > AP_TRIGGER_MS) {
        _startApSetup();
      }
    } else if (_mode != NetMode::AP_SETUP) {
      _noNetworkSince = 0;
    }

    if (_mode == NetMode::AP_SETUP) {
      dnsServer.processNextRequest();
      if (millis() - _apStartTime > AP_TIMEOUT_MS) {
        Serial.println("[NET] WiFi setup hotspot timed out, giving up");
        _stopApSetup(NetMode::NONE);
        _apGivenUp = true;
      }
    }
  }
}

const char *NetManager::modeStr() const {
  switch (_mode) {
  case NetMode::ETH_ACTIVE:
    return "ETH";
  case NetMode::WIFI_ACTIVE:
    return "WIFI";
  case NetMode::AP_SETUP:
    return "AP_SETUP";
  default:
    return "NONE";
  }
}

IPAddress NetManager::localIP() const {
  if (_mode == NetMode::ETH_ACTIVE)
    return ETH.localIP();
  if (_mode == NetMode::WIFI_ACTIVE)
    return WiFi.localIP();
  if (_mode == NetMode::AP_SETUP)
    return _apIP;
  return IPAddress(0, 0, 0, 0);
}

uint32_t NetManager::apSecondsLeft() const {
  if (_mode != NetMode::AP_SETUP)
    return 0;
  uint32_t elapsed = millis() - _apStartTime;
  return elapsed >= AP_TIMEOUT_MS ? 0 : (AP_TIMEOUT_MS - elapsed) / 1000;
}

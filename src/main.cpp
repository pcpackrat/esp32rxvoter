/*
  ESP32RXVoter - receive-only voter client
  Hardware: WT32-ETH01 (ESP32 + LAN8720) + GPS w/ PPS. Audio in on the ESP32 ADC1.

  Ported from TeensyVoter (DSP, GPS timing, voter protocol) and its Spirit
  ESP32 firmware (Ethernet/WiFi failover, web UI). No transmit path.

  Tasks:
  - audioTask (core 1, high priority): ADC read -> DSP -> uLaw -> COS -> frame queue
  - loop (core 1): GPS, network, voter protocol, web UI, syslog, serial commands
*/

#include <Arduino.h>
#include <WiFi.h>
#include <lwip/dns.h>

#include "App.h"
#include "AudioInput.h"
#include "Config.h"
#include "DSPProcessor.h"
#include "GPSManager.h"
#include "Logger.h"
#include "NetManager.h"
#include "Pins.h"
#include "Version.h"
#include "VoterClient.h"
#include "WebUI.h"

#define VOTER_LOCAL_PORT 8888
#define FRAME_QUEUE_DEPTH 25 // 500ms of audio
#define TAIL_FRAMES 10       // 200ms of RSSI=0 frames after unkey

GPSManager gps;
VoterClient voter;
RxStatus g_status;
volatile uint32_t g_rebootAt = 0;

static DSPProcessor dsp;
static AudioInput audioIn;

struct AudioFrame {
  uint8_t ulaw[AUDIO_FRAME_SAMPLES];
  uint8_t rssi;
  VTIME time;
};
static QueueHandle_t s_frameQueue = nullptr;

// Sample counter for measuring the ADC sample clock against GPS PPS
static portMUX_TYPE s_audioMux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_sampleCount = 0;
static int64_t s_lastReadUs = 0;

uint32_t g_adcOverruns() { return audioIn.getOverruns(); }

static bool cosInputActive() {
  int level = digitalRead(PIN_COS);
  // Default (cosInvert = false): LOW = signal present
  return cfg.data.cosInvert ? (level == HIGH) : (level == LOW);
}

// -----------------------------------------------------------------------------
// Audio task
// -----------------------------------------------------------------------------
static void audioTask(void *arg) {
  static int16_t pcm[AUDIO_FRAME_SAMPLES];
  static AudioFrame frame;
  int tailCount = 0;
  int16_t adcPeak = 0;
  int16_t encPeak = 0;
  int peakFrames = 0;

  for (;;) {
    int64_t readUs;
    if (!audioIn.readFrame(pcm, &readUs)) {
      g_status.readErrors = g_status.readErrors + 1;
      continue;
    }

    portENTER_CRITICAL(&s_audioMux);
    s_sampleCount += AUDIO_FRAME_SAMPLES;
    s_lastReadUs = readUs;
    portEXIT_CRITICAL(&s_audioMux);
    g_status.framesCaptured = g_status.framesCaptured + 1;

    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
      int16_t a = (pcm[i] == -32768) ? 32767 : (int16_t)abs(pcm[i]);
      if (a > adcPeak)
        adcPeak = a;
    }

    // The host can ask for flat audio / no PL filter (voter.conf nodeemp,
    // hostdeemp, noplfilter) - its request wins over the local setting
    uint8_t baseRSSI = dsp.process(pcm, cfg.data.enablePLFilter && !voter.hostNoPlFilter(),
                                   cfg.data.enableDeemp && !voter.hostFlatAudio());

    uint16_t rssiRaw = audioIn.getRssiRaw();
    g_status.rssiRaw = rssiRaw;
    if (cfg.data.useHwRSSI) {
      // Hardware RSSI: map calibrated ADC range to 0-255, the same scale as the
      // VOTER boards (chan_voter compares the raw byte)
      long r = constrain((long)rssiRaw, (long)cfg.data.rssiMin, (long)cfg.data.rssiMax);
      baseRSSI = (cfg.data.rssiMax > cfg.data.rssiMin)
                     ? (uint8_t)map(r, cfg.data.rssiMin, cfg.data.rssiMax, 0, 255)
                     : 0;
    }

    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
      int16_t a = (pcm[i] == -32768) ? 32767 : (int16_t)abs(pcm[i]);
      if (a > encPeak)
        encPeak = a;
    }
    if (++peakFrames >= 50) { // publish once per second
      g_status.adcPeak = adcPeak;
      g_status.adcDc = audioIn.getDcLevel();
      g_status.encPeak = encPeak;
      adcPeak = 0;
      encPeak = 0;
      peakFrames = 0;
    }

    dsp.encodeULaw(pcm, frame.ulaw, AUDIO_FRAME_SAMPLES);

    // Squelch. RSSI 0 means squelched (no audio), so an open squelch always
    // sends at least 1.
    const uint8_t openRSSI = baseRSSI ? baseRSSI : 1;
    uint8_t finalRSSI = openRSSI;
    bool active = true;
    switch (cfg.data.cosMode) {
    case COS_MODE_HARDWARE:
      active = cosInputActive();
      finalRSSI = active ? openRSSI : 0;
      break;
    case COS_MODE_DSP:
      active = dsp.getNoiseLevel() < cfg.data.dspSquelchThresh;
      finalRSSI = active ? openRSSI : 0;
      break;
    default: // COS_MODE_ALWAYS_ON
      break;
    }

    g_status.cos = active;
    g_status.rssi = finalRSSI;
    g_status.noise = dsp.getNoiseLevel();

    bool sending = (finalRSSI > 0);
    if (sending) {
      tailCount = TAIL_FRAMES;
    }
    // Master (host option SENDALWAYS): a frame every 20 ms, RSSI 0 + silence
    // when squelched - the host's timebase comes from this stream.
    bool sendAlways = voter.sendAlways();
    if (sending || tailCount > 0 || sendAlways) {
      if (!sending) {
        finalRSSI = 0; // Tail frame clears the server decoder
        if (tailCount > 0)
          tailCount--;
        else
          memset(frame.ulaw, 0xFF, sizeof(frame.ulaw)); // uLaw silence
      }
      frame.rssi = finalRSSI;
      frame.time = {0, 0};
      if (gps.isTimeSet()) {
        gps.getNetworkTime(&frame.time, true);
      }
      if (xQueueSend(s_frameQueue, &frame, 0) == pdTRUE) {
        g_status.framesQueued = g_status.framesQueued + 1;
      } else {
        g_status.framesDropped = g_status.framesDropped + 1;
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Sample clock vs GPS PPS (report only)
// -----------------------------------------------------------------------------
static void measureSampleClock(int64_t ppsUs) {
  static bool haveRef = false;
  static double refSamples = 0;
  static int64_t refPpsUs = 0;

  if (!gps.isLocked() || !g_status.audioOk) {
    haveRef = false;
    return;
  }

  portENTER_CRITICAL(&s_audioMux);
  uint64_t sc = s_sampleCount;
  int64_t lastRead = s_lastReadUs;
  portEXIT_CRITICAL(&s_audioMux);

  // Samples completed at the PPS edge (interpolated from the last DMA read)
  double samplesAtPps = (double)sc + (double)(ppsUs - lastRead) * (AUDIO_SAMPLE_RATE / 1e6);

  if (!haveRef) {
    haveRef = true;
    refSamples = samplesAtPps;
    refPpsUs = ppsUs;
    return;
  }

  int64_t secs = (ppsUs - refPpsUs + 500000) / 1000000;
  if (secs >= 60) {
    double expected = (double)secs * AUDIO_SAMPLE_RATE;
    g_status.clockPpm = (float)((samplesAtPps - refSamples - expected) / expected * 1e6);
    g_status.clockPpmValid = true;
    refSamples = samplesAtPps;
    refPpsUs = ppsUs;
  }
}

// -----------------------------------------------------------------------------
// Network up/down handling
// -----------------------------------------------------------------------------
static bool resolveHost(const char *name, IPAddress &out) {
  if (out.fromString(name))
    return true;
  return WiFi.hostByName(name, out) == 1;
}

static void onNetworkUp() {
  Serial.printf("[NET] Up via %s, IP %s\r\n", net.modeStr(), net.localIP().toString().c_str());

  // DNS override (DHCP only)
  if (!cfg.data.useStaticIP && cfg.data.dnsServerIP != 0) {
    IPAddress d(cfg.data.dnsServerIP);
    ip_addr_t addr;
    IP_ADDR4(&addr, d[0], d[1], d[2], d[3]);
    dns_setserver(0, &addr);
    Serial.printf("[DNS] Override: %s\r\n", d.toString().c_str());
  }

  // Voter host
  IPAddress hostIP = cfg.hostIP();
  if (cfg.data.hostname[0] != '\0') {
    IPAddress resolved;
    if (resolveHost(cfg.data.hostname, resolved)) {
      hostIP = resolved;
      Serial.printf("[DNS] %s -> %s\r\n", cfg.data.hostname, hostIP.toString().c_str());
    } else {
      Serial.printf("[DNS] Could not resolve %s, using stored IP %s\r\n", cfg.data.hostname,
                    hostIP.toString().c_str());
    }
  }
  voter.setHost(hostIP, cfg.data.hostPort);
  Serial.printf("[System] Voter Target: %s:%u\r\n", hostIP.toString().c_str(), cfg.data.hostPort);

  // Syslog
  if (cfg.data.useSyslog && cfg.data.syslogHost[0] != '\0') {
    IPAddress sysIP;
    if (resolveHost(cfg.data.syslogHost, sysIP)) {
      logger.setTarget(sysIP, cfg.data.syslogPort);
      Serial.printf("[Syslog] Target %s:%u\r\n", sysIP.toString().c_str(), cfg.data.syslogPort);
    } else {
      Serial.printf("[Syslog] Could not resolve %s\r\n", cfg.data.syslogHost);
    }
  }

  static bool voterStarted = false;
  if (!voterStarted) {
    voterStarted = true;
    voter.begin(&gps, cfg.data.clientPwd, cfg.data.hostPwd, VOTER_LOCAL_PORT);
    logger.info("SYS", "%s v%s up, IP %s", FIRMWARE_NAME, FIRMWARE_VERSION,
                net.localIP().toString().c_str());
  }
}

// -----------------------------------------------------------------------------
// Serial commands
// -----------------------------------------------------------------------------
static void printStatus() {
  Serial.println("----------------------------------------");
  Serial.printf(" %s v%s (%s)\r\n", FIRMWARE_NAME, FIRMWARE_VERSION, BUILD_DATE);
  Serial.printf(" Network : %s %s\r\n", net.modeStr(), net.localIP().toString().c_str());
  char voterState[96];
  voter.statusText(voterState, sizeof(voterState));
  Serial.printf(" Voter   : %s -> %s:%u, frames sent %lu\r\n",
                voterState, voter.getHostIP().toString().c_str(),
                cfg.data.hostPort, (unsigned long)voter.getAudioFramesSent());
  Serial.printf(" GPS     : %s, sats %lu, PPS jitter %lu us\r\n",
                GPSManager::statusStr(gps.getLockStatus()), (unsigned long)gps.getSatellites(),
                (unsigned long)gps.getPpsJitter());
  Serial.printf(" Audio   : ADC %s, bias %d counts, COS %s, RSSI %u (%s), noise %u\r\n",
                g_status.audioOk ? "OK" : "FAIL", (int)g_status.adcDc,
                g_status.cos ? "OPEN" : "closed", (unsigned)g_status.rssi,
                cfg.data.useHwRSSI ? "HW" : "DSP", (unsigned)g_status.noise);
  Serial.printf(" RSSI pin: %u counts (min %u, max %u)\r\n", (unsigned)g_status.rssiRaw,
                (unsigned)cfg.data.rssiMin, (unsigned)cfg.data.rssiMax);
  Serial.printf(" Peaks   : ADC %d, encoder %d (uLaw clip %d)\r\n", g_status.adcPeak,
                g_status.encPeak, ULAW_CLIP_LEVEL);
  Serial.printf(" Frames  : captured %lu, queued %lu, dropped %lu, ADC errors %lu, overruns %lu\r\n",
                (unsigned long)g_status.framesCaptured, (unsigned long)g_status.framesQueued,
                (unsigned long)g_status.framesDropped, (unsigned long)g_status.readErrors,
                (unsigned long)audioIn.getOverruns());
  if (g_status.clockPpmValid) {
    Serial.printf(" Clock   : %.2f ppm vs GPS\r\n", (double)g_status.clockPpm);
  }
  Serial.printf(" Heap    : %lu free\r\n", (unsigned long)ESP.getFreeHeap());
  Serial.println("----------------------------------------");
}

static void handleSerial() {
  static char line[32];
  static int len = 0;
  static uint32_t resetAskedAt = 0; // 0 = no confirmation pending
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      // Answer to the factory reset question. Expires after 30 s; a bare
      // Enter or anything but y/yes means No.
      if (resetAskedAt != 0 && millis() - resetAskedAt > 30000)
        resetAskedAt = 0;
      if (resetAskedAt != 0) {
        line[len] = '\0';
        len = 0;
        resetAskedAt = 0;
        Serial.println();
        if (strcasecmp(line, "y") == 0 || strcasecmp(line, "yes") == 0) {
          Serial.println("Factory reset - rebooting...");
          cfg.resetDefaults();
          delay(100);
          ESP.restart();
        }
        Serial.println("Factory reset cancelled");
        continue;
      }
      if (len == 0)
        continue;
      line[len] = '\0';
      len = 0;
      Serial.println();
      if (strcmp(line, "status") == 0) {
        printStatus();
      } else if (strcmp(line, "reboot") == 0) {
        Serial.println("Rebooting...");
        delay(100);
        ESP.restart();
      } else if (strcmp(line, "reset") == 0) {
        Serial.print("Erase all settings and restore factory defaults? Are you sure? (y/N): ");
        resetAskedAt = millis();
      } else {
        Serial.println("Commands: status, reboot, reset (factory defaults)");
      }
    } else if (len < (int)sizeof(line) - 1) {
      line[len++] = c;
    }
  }
}

// -----------------------------------------------------------------------------
// Setup / Loop
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("========================================");
  Serial.printf("%s v%s\r\n", FIRMWARE_NAME, FIRMWARE_VERSION);
  Serial.printf("Build Date: %s\r\n", BUILD_DATE);
  Serial.println("========================================");

  cfg.begin();

  // COS: direct from radio (active low) via 1k series, internal pull-up
  pinMode(PIN_COS, INPUT_PULLUP);

  // Audio + RSSI: ADC1 continuous mode
  g_status.audioOk = audioIn.begin(PIN_AUDIO_ADC, PIN_RSSI_ADC);

  dsp.begin();
  dsp.setRxDigitalGainPct(cfg.data.rxDigitalGainPct);
  dsp.setCalib(cfg.data.dspCalib);

  voter.setTimingOffset(cfg.data.timingOffsetMs);

  // GPS
  gps.begin(&Serial2, PIN_GPS_RX, PIN_GPS_PPS);
  Serial.println("[GPS] Started");

  // Network
  net.begin();

  // Audio task
  s_frameQueue = xQueueCreate(FRAME_QUEUE_DEPTH, sizeof(AudioFrame));
  if (g_status.audioOk) {
    xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 10, nullptr, 1);
  } else {
    Serial.println("[Audio] ERROR: ADC failed to start - no audio will be sent");
  }

  Serial.println("[System] Boot complete. Type 'status' for status.");
}

void loop() {
  static bool webStarted = false;
  static bool wasUp = false;

  // 1. GPS
  gps.update();
  int64_t ppsUs;
  if (gps.checkPPS(&ppsUs)) {
    measureSampleClock(ppsUs);
  }

  // 2. Network
  net.update();
  if (net.anyInterface() && !webStarted) {
    webBegin();
    webStarted = true;
  }
  bool up = net.isUp();
  if (up && !wasUp) {
    onNetworkUp();
  }
  wasUp = up;

  // 3. Voter protocol + queued audio frames
  voter.update();
  AudioFrame frame;
  while (xQueueReceive(s_frameQueue, &frame, 0) == pdTRUE) {
    voter.processAudioFrame(frame.ulaw, frame.rssi, frame.time);
  }

  // 4. Web UI, syslog, serial
  if (webStarted) {
    webHandle();
  }
  logger.update();
  handleSerial();

  if (g_rebootAt != 0 && (int32_t)(millis() - g_rebootAt) >= 0) {
    Serial.println("[System] Rebooting to apply settings...");
    delay(100);
    ESP.restart();
  }

  delay(1); // yield
}

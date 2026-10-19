#ifndef GPS_MANAGER_H
#define GPS_MANAGER_H

#include "VoterProtocol.h"
#include <Arduino.h>
#include <TinyGPSPlus.h>

class GPSManager {
public:
  GPSManager();

  void begin(HardwareSerial *port, int rxPin, int ppsPin);

  // Main loop update (NMEA parse + lock status tracking)
  void update();

  enum GPSLockStatus {
    GPS_NO_FIX,      // No valid time/location
    GPS_LOCKED,      // Time valid, PPS active, Serial active
    GPS_LOST_PPS,    // Time valid, but PPS missing
    GPS_LOST_SERIAL, // Time valid, PPS active, but Serial missing
  };

  bool isLocked() { return _stableStatus == GPS_LOCKED; }
  GPSLockStatus getLockStatus() { return _stableStatus; }
  bool isTimeSet() { return _validTime; }

  // True if PPS fired since last check (clears flag). ppsTimeUs receives the
  // esp_timer time of that edge.
  bool checkPPS(int64_t *ppsTimeUs = nullptr);

  // Current network time. quantize snaps to the 20ms grid relative to PPS.
  // Safe to call from any task.
  void getNetworkTime(VTIME *t, bool quantize = true);

  uint32_t getEpoch();
  uint32_t getPpsJitter();
  void getGPSStrings(char *lat, char *lon, char *elev);
  uint32_t getSatellites() { return _gpsParser.satellites.value(); }
  void getTimestamp(char *buf); // "[HH:MM:SS.mmm]", buf >= 20 bytes

  static const char *statusStr(GPSLockStatus s);

private:
  HardwareSerial *_gpsSerial;
  TinyGPSPlus _gpsParser;

  // PPS state (written by ISR)
  volatile int64_t _lastPpsUs;
  volatile bool _ppsTriggered;
  volatile uint32_t _ppsPeriod;

  // Time state
  volatile uint32_t _currentEpoch;
  volatile bool _validTime;

  GPSLockStatus _stableStatus;
  GPSLockStatus _pendingStatus;
  uint32_t _pendingStartTime;
  GPSLockStatus _lastLoggedStatus;

  static void _ppsISR();
  static GPSManager *_instance;

  GPSLockStatus _getRawLockStatus();
};

#endif

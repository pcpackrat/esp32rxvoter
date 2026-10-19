#include "GPSManager.h"
#include "Logger.h"
#include "Pins.h"
#include <esp_timer.h>
#include <time.h>

GPSManager *GPSManager::_instance = nullptr;

// Guards _lastPpsUs/_currentEpoch/_validTime: written by the PPS ISR and
// update() (loop task), read by the audio task.
static portMUX_TYPE s_gpsMux = portMUX_INITIALIZER_UNLOCKED;

// Days since 1970-01-01 for a civil date (proleptic Gregorian).
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

GPSManager::GPSManager() {
  _gpsSerial = nullptr;
  _lastPpsUs = 0;
  _ppsTriggered = false;
  _ppsPeriod = 1000000;
  _currentEpoch = 0;
  _validTime = false;
  _stableStatus = GPS_NO_FIX;
  _pendingStatus = GPS_NO_FIX;
  _pendingStartTime = 0;
  _lastLoggedStatus = GPS_NO_FIX;
  _instance = this;
}

void IRAM_ATTR GPSManager::_ppsISR() {
  GPSManager *g = _instance;
  if (!g)
    return;
  int64_t now = esp_timer_get_time();

  portENTER_CRITICAL_ISR(&s_gpsMux);
  int64_t delta = now - g->_lastPpsUs;
  // Debounce: ignore edges less than 900ms apart
  if (delta > 900000) {
    g->_ppsPeriod = (uint32_t)delta;
    g->_lastPpsUs = now;
    g->_ppsTriggered = true;
    if (g->_validTime) {
      g->_currentEpoch = g->_currentEpoch + 1;
    }
  }
  portEXIT_CRITICAL_ISR(&s_gpsMux);
}

bool GPSManager::checkPPS(int64_t *ppsTimeUs) {
  bool fired = false;
  portENTER_CRITICAL(&s_gpsMux);
  if (_ppsTriggered) {
    _ppsTriggered = false;
    fired = true;
    if (ppsTimeUs)
      *ppsTimeUs = _lastPpsUs;
  }
  portEXIT_CRITICAL(&s_gpsMux);
  return fired;
}

void GPSManager::begin(HardwareSerial *port, int rxPin, int ppsPin) {
  _gpsSerial = port;
  _gpsSerial->setRxBufferSize(2048);
  _gpsSerial->begin(GPS_BAUD, SERIAL_8N1, rxPin, -1);

  pinMode(ppsPin, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(ppsPin), _ppsISR, RISING);
}

void GPSManager::update() {
  // 1. Parse serial data
  while (_gpsSerial && _gpsSerial->available() > 0) {
    _gpsParser.encode(_gpsSerial->read());
  }

  // 2. New NMEA time -> epoch
  if (_gpsParser.time.isUpdated() && _gpsParser.date.isUpdated()) {
    if (_gpsParser.location.isValid()) {
      int64_t days = daysFromCivil(_gpsParser.date.year(), _gpsParser.date.month(),
                                   _gpsParser.date.day());
      uint32_t gpsTime = (uint32_t)(days * 86400 + _gpsParser.time.hour() * 3600 +
                                    _gpsParser.time.minute() * 60 + _gpsParser.time.second());

      bool firstSync = !_validTime;
      if (firstSync || gpsTime != _currentEpoch) {
        portENTER_CRITICAL(&s_gpsMux);
        _currentEpoch = gpsTime;
        _validTime = true;
        // Seed the PPS marker from NMEA only on the very first sync, before a
        // real PPS edge has been seen. After that the ISR is the sole owner,
        // so PPS loss is still detected while NMEA keeps flowing.
        if (firstSync) {
          _lastPpsUs = esp_timer_get_time() - (int64_t)_gpsParser.time.age() * 1000;
        }
        portEXIT_CRITICAL(&s_gpsMux);
      }
    }
  }

  // 3. Status transitions with 500ms hysteresis
  GPSLockStatus currentRaw = _getRawLockStatus();
  if (currentRaw != _pendingStatus) {
    _pendingStatus = currentRaw;
    _pendingStartTime = millis();
  } else if (millis() - _pendingStartTime > 500) {
    _stableStatus = currentRaw;
  }

  if (_stableStatus != _lastLoggedStatus) {
    char ts[20];
    getTimestamp(ts);
    Serial.printf("%s [GPS] Status Change: %s -> %s (Sats: %u)\r\n", ts,
                  statusStr(_lastLoggedStatus), statusStr(_stableStatus),
                  (unsigned)getSatellites());
    logger.warn("GPS", "Status Change: %s -> %s (Sats: %u)",
                statusStr(_lastLoggedStatus), statusStr(_stableStatus),
                (unsigned)getSatellites());
    _lastLoggedStatus = _stableStatus;
  }
}

const char *GPSManager::statusStr(GPSLockStatus s) {
  switch (s) {
  case GPS_NO_FIX:
    return "NO_FIX";
  case GPS_LOCKED:
    return "LOCKED";
  case GPS_LOST_PPS:
    return "LOST_PPS";
  case GPS_LOST_SERIAL:
    return "LOST_SERIAL";
  }
  return "UNKNOWN";
}

GPSManager::GPSLockStatus GPSManager::_getRawLockStatus() {
  if (!_validTime) {
    return GPS_NO_FIX;
  }

  portENTER_CRITICAL(&s_gpsMux);
  int64_t lastPps = _lastPpsUs;
  portEXIT_CRITICAL(&s_gpsMux);

  if (esp_timer_get_time() - lastPps > 2000000) {
    return GPS_LOST_PPS;
  }
  if (_gpsParser.time.age() > 2000) {
    return GPS_LOST_SERIAL;
  }
  // At least 3 satellites before connecting (2D fix is enough for time)
  if (_gpsParser.satellites.value() < 3) {
    return GPS_NO_FIX;
  }
  return GPS_LOCKED;
}

uint32_t GPSManager::getPpsJitter() {
  uint32_t p = _ppsPeriod;
  return (p > 1000000) ? (p - 1000000) : (1000000 - p);
}

void GPSManager::getNetworkTime(VTIME *t, bool quantize) {
  if (!t)
    return;

  portENTER_CRITICAL(&s_gpsMux);
  int64_t lastPps = _lastPpsUs;
  uint32_t epoch = _currentEpoch;
  portEXIT_CRITICAL(&s_gpsMux);

  int64_t deltaUs = esp_timer_get_time() - lastPps;
  if (deltaUs < 0)
    deltaUs = 0;

  // Past one second without a PPS edge: carry whole seconds forward
  if (deltaUs >= 1000000) {
    epoch += (uint32_t)(deltaUs / 1000000);
    deltaUs %= 1000000;
  }

  if (quantize) {
    // Snap to nearest 20ms slot
    deltaUs = ((deltaUs + 10000) / 20000) * 20000;
  }

  if (deltaUs >= 1000000) {
    epoch++;
    deltaUs -= 1000000;
  }

  t->vtime_sec = epoch;
  t->vtime_nsec = (uint32_t)deltaUs * 1000;
}

uint32_t GPSManager::getEpoch() {
  portENTER_CRITICAL(&s_gpsMux);
  uint32_t e = _currentEpoch;
  portEXIT_CRITICAL(&s_gpsMux);
  return e;
}

void GPSManager::getTimestamp(char *buf) {
  if (!buf)
    return;
  VTIME vt;
  getNetworkTime(&vt, false);
  time_t tt = (time_t)vt.vtime_sec;
  struct tm tm;
  gmtime_r(&tt, &tm);
  snprintf(buf, 20, "[%02d:%02d:%02d.%03lu]", tm.tm_hour, tm.tm_min, tm.tm_sec,
           (unsigned long)(vt.vtime_nsec / 1000000));
}

void GPSManager::getGPSStrings(char *lat, char *lon, char *elev) {
  if (lat) {
    // DDMM.mmN
    double rawLat = _gpsParser.location.lat();
    char ns = (rawLat >= 0) ? 'N' : 'S';
    rawLat = fabs(rawLat);
    int deg = (int)rawLat;
    if (deg > 90)
      deg = 90;
    double mins = (rawLat - deg) * 60.0;
    snprintf(lat, 9, "%02d%05.2f%c", deg, mins, ns);
  }
  if (lon) {
    // DDDMM.mmW
    double rawLon = _gpsParser.location.lng();
    char ew = (rawLon >= 0) ? 'E' : 'W';
    rawLon = fabs(rawLon);
    int deg = (int)rawLon;
    if (deg > 180)
      deg = 180;
    double mins = (rawLon - deg) * 60.0;
    snprintf(lon, 10, "%03d%05.2f%c", deg, mins, ew);
  }
  if (elev) {
    // Elevation in meters, %4.1f, 7-byte protocol field
    snprintf(elev, 7, "%4.1f", _gpsParser.altitude.meters());
  }
}

#ifndef AUDIO_INPUT_H
#define AUDIO_INPUT_H

#include <Arduino.h>

#define AUDIO_SAMPLE_RATE 8000
#define AUDIO_FRAME_SAMPLES 160 // 20ms

// ESP32 continuous ADC can't run below 20 kHz, so sample at 32 kHz and
// decimate by 4 with a FIR low-pass (~3.4 kHz).
#define ADC_SAMPLE_RATE 32000
#define ADC_DECIMATION (ADC_SAMPLE_RATE / AUDIO_SAMPLE_RATE)
#define ADC_DECIM_TAPS 63

// Receive audio (and RSSI voltage) from the ESP32's internal ADC1
// (continuous / DMA mode). Pattern alternates audio, RSSI, so the total
// conversion rate is 2 x ADC_SAMPLE_RATE.
class AudioInput {
public:
  bool begin(int audioPin, int rssiPin);

  // Blocks until one 20ms frame (160 samples at 8 kHz) is ready.
  // readTimeUs = esp_timer time when the frame completed.
  bool readFrame(int16_t *out, int64_t *readTimeUs);

  // DC level of the input in raw ADC counts (0-4095). Mid-scale bias check.
  float getDcLevel() const { return _dc; }

  // Average RSSI pin reading over the last frame, raw counts (0-4095)
  uint16_t getRssiRaw() const { return _rssiRaw; }

  // Times the driver's buffer overflowed (audio task too slow)
  uint32_t getOverruns() const;

private:
  uint16_t _rssiRaw = 0;
  float _coeffs[ADC_DECIM_TAPS];
  float _hist[128]; // ring buffer, power of 2 > ADC_DECIM_TAPS
  unsigned _histIdx = 0;
  int _phase = 0;
  float _dc = 2048.0f;
  bool _dcInit = false;
};

#endif

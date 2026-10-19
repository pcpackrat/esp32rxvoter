#ifndef APP_H
#define APP_H

#include "GPSManager.h"
#include "VoterClient.h"
#include <Arduino.h>

// Live receive status. Written by the audio task, read by the web UI.
struct RxStatus {
  volatile uint8_t rssi;            // RSSI byte of the last frame (0 = squelched)
  volatile bool cos;                // Squelch open
  volatile uint8_t noise;           // DSP noise level (0-255)
  volatile uint16_t rssiRaw;        // RSSI pin, raw ADC counts (0-4095)
  volatile int16_t adcPeak;         // Max |sample| over last second, before DSP
  volatile int16_t encPeak;         // Max |sample| over last second, after DSP
  volatile uint32_t framesCaptured; // 20ms frames read from the ADC
  volatile uint32_t framesQueued;   // Frames handed to the network loop
  volatile uint32_t framesDropped;  // Frame queue full
  volatile uint32_t readErrors;     // ADC read timeouts/errors
  volatile float adcDc;             // Input bias in raw ADC counts (0-4095)
  volatile float clockPpm;          // Sample clock error vs GPS PPS
  volatile bool clockPpmValid;
  bool audioOk;
};

extern RxStatus g_status;
extern GPSManager gps;
extern VoterClient voter;
extern volatile uint32_t g_rebootAt; // millis() to restart at, 0 = none
uint32_t g_adcOverruns();

// uLaw encoder clips at this magnitude (13-bit, see DSPProcessor.cpp)
#define ULAW_CLIP_LEVEL 8159

#endif

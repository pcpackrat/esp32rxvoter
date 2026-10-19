#include "AudioInput.h"
#include <esp_adc/adc_continuous.h>
#include <esp_timer.h>
#include <math.h>

// One 20ms frame of raw samples (audio + RSSI) per DMA conversion frame
#define ADC_PATTERN_LEN 2
#define RAW_PER_FRAME (AUDIO_FRAME_SAMPLES * ADC_DECIMATION * ADC_PATTERN_LEN)
#define CONV_FRAME_BYTES (RAW_PER_FRAME * SOC_ADC_DIGI_RESULT_BYTES)

static adc_continuous_handle_t s_adc = nullptr;
static adc_channel_t s_channel;
static adc_channel_t s_rssiChannel;
static uint8_t s_raw[CONV_FRAME_BYTES];
static uint32_t s_rawLen = 0;
static uint32_t s_rawPos = 0;
static volatile uint32_t s_overruns = 0;

static bool IRAM_ATTR onPoolOverflow(adc_continuous_handle_t handle,
                                     const adc_continuous_evt_data_t *edata, void *user) {
  s_overruns = s_overruns + 1;
  return false;
}

uint32_t AudioInput::getOverruns() const { return s_overruns; }

bool AudioInput::begin(int audioPin, int rssiPin) {
  // Decimation low-pass: windowed sinc (Blackman), cutoff 3.4 kHz, unity DC gain
  const float fc = 3400.0f / (float)ADC_SAMPLE_RATE;
  const int M = ADC_DECIM_TAPS - 1;
  float sum = 0.0f;
  for (int n = 0; n < ADC_DECIM_TAPS; n++) {
    float x = (float)n - (float)M / 2.0f;
    float sinc = (x == 0.0f) ? 2.0f * fc : sinf(2.0f * (float)M_PI * fc * x) / ((float)M_PI * x);
    float w = 0.42f - 0.5f * cosf(2.0f * (float)M_PI * n / M) + 0.08f * cosf(4.0f * (float)M_PI * n / M);
    _coeffs[n] = sinc * w;
    sum += _coeffs[n];
  }
  for (int n = 0; n < ADC_DECIM_TAPS; n++) {
    _coeffs[n] /= sum;
  }
  memset(_hist, 0, sizeof(_hist));

  adc_unit_t unit;
  if (adc_continuous_io_to_channel(audioPin, &unit, &s_channel) != ESP_OK || unit != ADC_UNIT_1) {
    Serial.printf("[Audio] GPIO%d is not an ADC1 pin\r\n", audioPin);
    return false;
  }
  if (adc_continuous_io_to_channel(rssiPin, &unit, &s_rssiChannel) != ESP_OK || unit != ADC_UNIT_1) {
    Serial.printf("[Audio] GPIO%d is not an ADC1 pin\r\n", rssiPin);
    return false;
  }

  adc_continuous_handle_cfg_t hcfg = {};
  hcfg.max_store_buf_size = CONV_FRAME_BYTES * 8; // 160ms of slack
  hcfg.conv_frame_size = CONV_FRAME_BYTES;
  esp_err_t err = adc_continuous_new_handle(&hcfg, &s_adc);
  if (err != ESP_OK) {
    Serial.printf("[Audio] adc_continuous_new_handle failed: %d\r\n", err);
    return false;
  }

  adc_digi_pattern_config_t pattern[ADC_PATTERN_LEN] = {};
  pattern[0].atten = ADC_ATTEN_DB_12; // ~0.15-2.45 V input range
  pattern[0].channel = s_channel;
  pattern[0].unit = ADC_UNIT_1;
  pattern[0].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;
  pattern[1] = pattern[0];
  pattern[1].channel = s_rssiChannel;

  adc_continuous_config_t ccfg = {};
  ccfg.pattern_num = ADC_PATTERN_LEN;
  ccfg.adc_pattern = pattern;
  ccfg.sample_freq_hz = ADC_SAMPLE_RATE * ADC_PATTERN_LEN; // total conversions/s
  ccfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  ccfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;
  err = adc_continuous_config(s_adc, &ccfg);
  if (err != ESP_OK) {
    Serial.printf("[Audio] adc_continuous_config failed: %d\r\n", err);
    return false;
  }

  adc_continuous_evt_cbs_t cbs = {};
  cbs.on_pool_ovf = onPoolOverflow;
  adc_continuous_register_event_callbacks(s_adc, &cbs, nullptr);

  err = adc_continuous_start(s_adc);
  if (err != ESP_OK) {
    Serial.printf("[Audio] adc_continuous_start failed: %d\r\n", err);
    return false;
  }

  Serial.printf("[Audio] ADC1 audio CH%d (GPIO%d) at %d Hz, decimate x%d -> %d Hz; RSSI CH%d (GPIO%d)\r\n",
                (int)s_channel, audioPin, ADC_SAMPLE_RATE, ADC_DECIMATION, AUDIO_SAMPLE_RATE,
                (int)s_rssiChannel, rssiPin);
  return true;
}

bool AudioInput::readFrame(int16_t *out, int64_t *readTimeUs) {
  int outCount = 0;
  uint32_t rssiSum = 0;
  uint32_t rssiCount = 0;

  while (outCount < AUDIO_FRAME_SAMPLES) {
    if (s_rawPos >= s_rawLen) {
      uint32_t got = 0;
      esp_err_t err = adc_continuous_read(s_adc, s_raw, sizeof(s_raw), &got, 100);
      if (err != ESP_OK) {
        return false;
      }
      s_rawLen = got;
      s_rawPos = 0;
    }

    const adc_digi_output_data_t *p = (const adc_digi_output_data_t *)&s_raw[s_rawPos];
    s_rawPos += SOC_ADC_DIGI_RESULT_BYTES;
    if (p->type1.channel == s_rssiChannel) {
      rssiSum += p->type1.data;
      rssiCount++;
      continue;
    }
    if (p->type1.channel != s_channel) {
      continue;
    }
    float x = (float)p->type1.data;

    // DC removal (bias point), ~2.5 Hz corner at 32 kHz
    if (!_dcInit) {
      _dc = x;
      _dcInit = true;
    }
    _dc += (x - _dc) * 0.0005f;

    // 12-bit counts -> 16-bit scale
    _hist[_histIdx & 127] = (x - _dc) * 16.0f;
    _histIdx++;

    if (++_phase >= ADC_DECIMATION) {
      _phase = 0;
      float acc = 0.0f;
      unsigned idx = _histIdx - 1;
      for (int k = 0; k < ADC_DECIM_TAPS; k++) {
        acc += _coeffs[k] * _hist[(idx - k) & 127];
      }
      if (acc > 32767.0f)
        acc = 32767.0f;
      if (acc < -32768.0f)
        acc = -32768.0f;
      out[outCount++] = (int16_t)acc;
    }
  }

  if (rssiCount > 0) {
    _rssiRaw = (uint16_t)(rssiSum / rssiCount);
  }
  if (readTimeUs)
    *readTimeUs = esp_timer_get_time();
  return true;
}

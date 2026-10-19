#ifndef PINS_H
#define PINS_H

// WT32-ETH01 header pin assignments.
// This board variant does not break out IO32/IO33. Pins reserved by the
// LAN8720 PHY: 0, 16, 18, 19, 21, 22, 23, 25, 26, 27. UART0 (IO1/IO3) is the
// console and flashing port.

// --- ESP32 internal ADC1 (both sampled in the same DMA stream) ---
#define PIN_AUDIO_ADC 36 // ADC1_CH0 (SENSOR_VP). Bias to ~1.3V, AC-couple audio in.
#define PIN_RSSI_ADC 39  // ADC1_CH3 (SENSOR_VN). 0-2.4V max - divide down higher RSSI voltages.

// --- GPS ---
#define PIN_GPS_RX 35 // GPS TX -> ESP32. Input-only pin.
#define PIN_GPS_PPS 4 // Interrupt input
#define GPS_BAUD 9600

// --- Radio ---
// COS direct from the radio (active low), internal pull-up. IO14 outputs a
// short pulse during boot - wire it through a 1k series resistor.
#define PIN_COS 14

// --- Onboard LED (IO17 / TX2, active-low). Lit while the setup hotspot runs. ---
#define PIN_LED 17

// Free header pins: IO2, IO12, IO15 (all strap pins)

// --- LAN8720 PHY (fixed by the WT32-ETH01) ---
#define WT32_ETH_PHY_ADDR 1
#define WT32_ETH_PHY_POWER 16
#define WT32_ETH_PHY_MDC 23
#define WT32_ETH_PHY_MDIO 18

#endif

# ESP32RXVoter - Wiring

WT32-ETH01 by itself + GPS + Motorola CDM (16-pin rear accessory connector).

## 1. Block diagram

```
                 CDM 1250 16-pin accessory
          ┌──────────────┬──────────────┬───────────────┬────────────┬─────────┐
          │ SW B+        │ RX audio     │ COS (prog I/O)│ RSSI volts │ GND     │
          ▼              ▼              ▼               ▼            ▼
   ┌─────────────┐ ┌───────────┐ ┌────────────┐ ┌────────────┐     │
   │ Fuse / TVS  │ │ AC couple │ │ 1k series  │ │ Divider    │     │
   │ Rev. diode  │ │ + bias    │ │ (internal  │ │ + RC       │     │
   │ TSR 1-2450  │ │ + RC LPF  │ │  pull-up)  │ │            │     │
   └──────┬──────┘ └─────┬─────┘ └─────┬──────┘ └─────┬──────┘     │
          │ 5V           │ IO36        │ IO14         │ IO39       │
          ▼              ▼             ▼              ▼            ▼
   ┌──────────────────────────────────────────────────────────────────┐
   │                    WT32-ETH01                                    │
   │  5V   3V3   IO36   IO14   IO39   IO35   IO4    GND    RJ45 ──────┼──► LAN
   └────────┬─────────────────────────▲──────▲───────┬────────────────┘
            │                         │ TX   │ PPS   │
            │ 3V3             ┌───────┴──────┴───────┴──┐
            └───────────────► │  VCC   GPS module   GND │
                              └─────────────────────────┘
```

## 2. WT32-ETH01 pin table

| WT32 pin | Direction | Signal | Connects to |
|---|---|---|---|
| 5V | in | Power | TSR 1-2450 output (section 5) |
| GND | — | Ground | Common ground (section 6) |
| 3V3 | out | 3.3 V | GPS VCC (section 7), audio bias divider |
| IO36 | in (ADC1_CH0) | RX audio | Audio input network (4.1) |
| IO14 | in | COS | CDM pin 12 through 1k series (4.2) |
| IO39 | in (ADC1_CH3) | RSSI voltage | RSSI input network (4.3) |
| IO35 | in | GPS NMEA | GPS TXD (section 7) |
| IO4 | in | GPS PPS | GPS PPS / 1PPS (section 7) |
| IO17 | out | LED | Onboard (lit = setup hotspot) |
| IO1 / IO3 / IO0 | — | Flashing | USB-serial (section 3) |
| IO2, IO12, IO15 | — | Unused | Leave open. All three are boot strap pins |

Do not use IO0, 16, 18, 19, 21, 22, 23, 25, 26, 27. They belong to the LAN8720 Ethernet PHY.

## 3. Flashing header

```
USB-serial (3.3 V)        WT32-ETH01
  TX  ─────────────────►  RX0 (IO3)
  RX  ◄─────────────────  TX0 (IO1)
  GND ──────────────────  GND
                          IO0 ──[jumper]── GND   (fit for flashing, remove to run)
```
The WT32 has no auto-reset. Fit the jumper, power-cycle or reset, flash, remove the jumper, then reset.

## 4. Radio interface

### 4.1 RX audio → IO36

```
                                     3V3
                                      │
                                     R5 2.2k
                                      │
                                      ├────────┬──── (bias node, ~1.34 V)
                                     R6 1.5k  C4 10µF
                                      │        │
                                     GND      GND
                                      (bias node)
                                           │
                                          R4 4.7k
                                           │
CDM RX audio ──C1 10µF NP──R1 4.7k────────┼──────── IO36 (ADC1_CH0)
                                           │
                                          C3 10nF
                                           │
CDM GND ─────────────────────────────────GND
```

| Part | Value | Purpose |
|---|---|---|
| R5 / R6 | 2.2k / 1.5k | Bias ≈ 1.34 V, the middle of the ADC range |
| C4 | 10 µF | Filters 3V3 noise off the bias (≈18 Hz corner) |
| R4 | 4.7k | Feeds bias to the pin. Also the shunt leg of the level divider |
| C1 | 10 µF non-polar, ≥16 V | Blocks DC both ways. Low corner ≈1.7 Hz with R1 + R4 |
| R1 | 4.7k to start | Level set. R1 = R4 gives -6 dB. Lower it (down to 0 Ω) if the radio level is low |
| C3 | 10 nF | Anti-alias low-pass (~7 kHz with R1‖R4) and charge reservoir for the ADC sample cap |

**Level target:** full-deviation voice gives ADC peak about -6 to -8 dBFS on the status page. That is roughly ±0.75 V at the pin. Then trim Digital Gain for Encoder peak -3 to -6 dB.

Check: with no audio, the status page "Input bias" reads 1000-2400 counts.

### 4.2 COS → IO14

Direct connection, same as the TeensyVoter. The CDM COS output is active low.

```
CDM pin 12 ──R7 1k── IO14     (firmware enables the internal ~45k pull-up)
CDM pin 7  ───────── GND
```

- R7 (1k) protects IO14, which drives a short pulse during boot, in case the radio is holding COS low at that moment.
- It doesn't affect reading. With the radio pulling low, IO14 sits at about 0.07 V (3.3 V × 1k / 46k), well below the ~0.8 V LOW threshold.
- Radio COS **low** = signal present → firmware sees "active" with Invert COS **off** (default).
- Check on the status page: COS should show OPEN only while a signal is present.

### 4.3 Hardware RSSI → IO39 (optional)

CDM 1250 accessory **pin 15** = RSSI. Measure its voltage range (no signal and full quieting) before fitting the divider. If it never exceeds 2.4 V, use R12 = 1k and leave R13 out. With "Use Hardware RSSI" off, the DSP noise RSSI is used instead.

```
CDM pin 15  ──R12 10k──┬──────────┬──────── IO39 (ADC1_CH3)
                       │          │
                      R13 8.2k   C5 100nF
                       │          │
GND ───────────────────┴──────────┴──────── GND
```

| Part | Value | Notes |
|---|---|---|
| R12 / R13 | 10k / 8.2k | 5 V max RSSI → 2.25 V at the pin. ADC range is about 0.15-2.45 V. For RSSI ≤ 2.4 V, fit R12 = 1k and leave R13 out |
| C5 | 100 nF | Smooths the RSSI voltage (~0.5 ms). The firmware also averages over each 20 ms frame |

**Calibrate:** on the Status page, read "RSSI pin (raw counts)" with no signal → enter it as RSSI Min. Read it again with a full-quieting signal → enter it as RSSI Max. Then tick "Use Hardware RSSI" (Radio page). Readings between Min and Max map to RSSI 0-127.

### 4.4 CDM 1250 accessory connector

| CDM pin | Name | Goes to |
|---|---|---|
| 7 | GROUND | System ground |
| 11 | FLAT/FILTERED_RX_AUDIO, set to **Flat Unsquelched** in CPS | 4.1 input (C1) |
| 12 | DIG_IN_OUT_7, programmed in CPS as channel activity / carrier detect | 4.2 COS input (R7) |
| 13 | SWB+ (follows radio power switch) | Power input, section 5 |
| 15 | RSSI | 4.3 RSSI input (R12) |

Pin 14 (DIG_IN_OUT_8) also works for COS. Avoid 6 and 8, which double as Sel5/MDC/MPT UART.

Not used: 1/16 speaker, 2 mic, 3 DIG_IN1, 4 alarm, 5 TX audio, 9 emergency, 10 ignition.

## 5. Power from CDM switched B+

Regulator: **TRACO TSR 1-2450** (5 V, 1 A, ~6.5-36 V in, 3-pin SIP: 1 = +Vin, 2 = GND, 3 = +Vout).

```
CDM 13 SWB+  ──F1──D1──►──┬──────────┬──────────┬──── pin 1    ┌────────────┐
 (13.8 V)      PTC  SS34  │          │          │              │ TSR 1-2450 │
              500mA      TVS1      C1 10µF    C2 100nF         │            │
                         SMBJ18A   50V cer.     │      ┌─pin 2 │            │
                          │          │          │      │       └─────┬──────┘
CDM 7 GND ────────────────┴──────────┴──────────┴──────┘             │ pin 3
                                                                     │
                          ┌──────────────────────────────────────────┘
                          │
                          ├── C3 22µF ──┬── C4 100nF ──FB1──► WT32 5V
                          │             │                     (onboard AMS1117 makes 3V3)
GND (pin 2) ──────────────┴─────────────┴──────────────────► WT32 GND
```

| Part | Value | Notes |
|---|---|---|
| F1 | PTC 500 mA hold | Protects the radio's switched B+ line |
| D1 | SS34 Schottky, series | Reverse-polarity protection (~0.4 V drop) |
| TVS1 | SMBJ18A | 18 V standoff, clamps ~29 V. Below the TSR's 36 V input maximum |
| C1 / C2 | 10 µF 50 V ceramic / 100 nF | TSR input, right at pin 1-2 |
| C3 / C4 | 22 µF 10 V / 100 nF | TSR output, right at pin 3-2 |
| FB1 | Ferrite bead, 600 Ω @ 100 MHz | Keeps switching noise off the WT32 rail |

The TSR runs without external caps. C1-C4 are there to keep its switching noise off the radio cable and the WT32 rail. Check the TRACO datasheet for the recommended values.

**Load budget at 5 V:**

| Load | Current |
|---|---|
| WT32-ETH01 (Ethernet) | ~150-200 mA |
| WT32-ETH01 (WiFi fallback active) | ~300 mA peaks |
| GPS | ~30-50 mA |
| Bias divider | ~1 mA |
| **Total** | **~250 mA typical, 350 mA peak** |

That is about 100-140 mA from 13.8 V through the TSR (about 90% efficient), well inside a CDM switched B+ output. It also uses only a quarter to a third of the TSR's 1 A rating.

## 6. Grounding and layout

- One ground point: radio GND, TSR GND, WT32 GND and GPS GND meet at one spot near the WT32 GND pin.
- Keep the audio input network (section 4.1) right at IO36, with C3 and the bias parts within a few cm of the pin.
- Route RX audio and its ground return as a twisted pair or shielded cable from the CDM connector. Ground the shield at the radio end only.
- Keep the TSR away from IO36 and the audio wiring.
- Keep the GPS antenna cable away from the TSR.

## 7. GPS

Module: GT-U7 class (u-blox NEO-6M compatible) with a PPS pin. 9600 baud NMEA, 8N1.

```
WT32-ETH01                             GPS module
  3V3  ──────────┬──────┬────────────►  VCC
                 │      │
               C6 10µF C7 100nF
                 │      │
  GND  ──────────┴──────┴────────────   GND
  IO35 ◄─────────────────────────────   TXD
  IO4  ◄─────────────────────────────   PPS
                                        RXD   (not connected)
```

| GPS pin | WT32 pin | Notes |
|---|---|---|
| VCC | 3V3 | From the WT32 onboard AMS1117. 30-50 mA |
| GND | GND | Run it with the VCC wire, back to the single ground point (section 6) |
| TXD | IO35 | NMEA out. IO35 is input-only, no pull-up needed |
| PPS | IO4 | 1 pulse/s, rising edge. Firmware enables the internal pull-down |
| RXD | — | Leave open. Firmware never sends to the GPS |

| Part | Value | Notes |
|---|---|---|
| C6 / C7 | 10 µF / 100 nF | At the GPS VCC pin. Covers the acquisition current peaks |

- Power from 3V3, not the TSR 5V. The linear 3V3 rail is quieter, and the GPS logic stays at 3.3 V.
- A module rated 5 V only (no onboard 3.3 V regulator) must not be used here. Its TXD/PPS would exceed the ESP32 3.3 V input limit.
- Most GT-U7 boards take 3.3-5 V on VCC through an onboard LDO. Running them from 3V3 leaves the LDO with almost no headroom; if the module browns out or loses lock at power-up, move VCC to the WT32 5V pin. TXD/PPS stay 3.3 V either way.
- Keep the TXD/PPS wires short (under ~30 cm) and away from the TSR.
- Check on the status page: GPS = LOCKED, sats ≥ 3, PPS jitter in the tens of µs (BUILD_PLAN.md step 3).

## 8. Option: ES8311 codec for RX audio

Replaces the ESP32 ADC audio path (section 4.1) with an ES8311 codec module. **Firmware does not support this yet.** The current build reads audio on IO36 with the internal ADC.

### 8.1 Pin changes

The codec needs 5 pins: I2C (2) and I2S (3). The header has only 8 free signal pins, so hardware RSSI (4.3) is dropped and COS moves to IO39.

| WT32 pin | Section 4 use | ES8311 option | Notes |
|---|---|---|---|
| IO15 | — | I2C SDA | 4.7k pull-up to 3V3. Strap pin, high at boot is normal |
| IO14 | COS | I2C SCL | 4.7k pull-up to 3V3. The IO14 boot pulse is not a valid I2C start, so it is harmless |
| IO2 | — | I2S BCLK → ES8311 SCLK | Do not pull up. IO2 must be low or floating to flash |
| IO12 | — | I2S WS → ES8311 LRCK | Do not pull up. IO12 high at boot selects 1.8 V flash and the board will not boot. Fit R20 10k to GND if the module has a pull-up on LRCK |
| IO36 | RX audio (ADC) | I2S DIN ← ES8311 ASDOUT | Input-only pin, fine for data in |
| IO39 | RSSI (ADC) | COS | External 10k pull-up to 3V3 (IO39 has no internal pull-up). Keep the 1k series |
| IO35 | GPS TXD | GPS TXD | No change |
| IO4 | GPS PPS | GPS PPS | No change |

- **No MCLK.** The ESP32 can only output MCLK on IO0, IO1 or IO3. IO0 is the Ethernet clock and IO1/IO3 are the console. The ES8311 takes its clock from BCLK instead (firmware sets MCLK source = SCLK). Leave the module's MCLK pin open, or tie it to GND if it floats.
- **I2C address:** 0x18 with the ES8311 CE/AD0 pin low.
- **Hardware RSSI:** not available in this option. Use the DSP noise RSSI.

### 8.2 COS → IO39

```
                  3V3
                   │
                  R21 10k
                   │
CDM pin 12 ──R7 1k─┴──── IO39
CDM pin 7  ───────────── GND
```

With the radio pulling low, IO39 sits at about 0.3 V (3.3 V × 1k / 11k). That is below the LOW threshold of ~0.8 V.

### 8.3 RX audio → ES8311 MIC1P / MIC1N

```
CDM 11 ──C1 10µF NP──R1 10k──┬─────────┬──────C2 1µF──── MIC1P
                             │         │
                            R2 10k    C4 1nF
                             │         │
CDM 7 (audio return) ────────┴─────────┴──────C3 1µF──── MIC1N
```

| Part | Value | Purpose |
|---|---|---|
| C1 | 10 µF non-polar, ≥16 V | Blocks radio DC |
| R1 / R2 | 10k / 10k | Level divider, -6 dB to start. Smaller R2 = quieter |
| C4 | 1 nF | RF and hiss filter (~32 kHz corner with R1‖R2) |
| C2 / C3 | 1 µF film or X7R | Codec input coupling. The ES8311 biases MIC1P/N internally. Omit these if the module already has coupling caps on MIC1P/N |

- MIC1N goes to the radio's audio return, not to local GND. The codec input is differential, so ground noise between the radio and the WT32 is rejected.
- Run CDM 11 and the audio return as a twisted pair or shielded cable. Ground the shield at the radio end only.
- If the module has an onboard mic, remove it or cut its traces to MIC1P/N. Firmware keeps the mic bias off.
- **Level:** full-deviation voice should give "ADC peak" about -6 to -10 dBFS. Adjust R2 first, then the codec PGA (0-30 dB), then Digital Gain. Trim Digital Gain for "Encoder peak" -3 to -6 dB.

### 8.4 Codec power

```
WT32 3V3 ──FB2──┬────────┬──────── ES8311 module VCC (AVDD/DVDD)
                │        │
              C5 10µF  C6 100nF
                │        │
GND ────────────┴────────┴──────── ES8311 module GND
```

- FB2 (600 Ω @ 100 MHz) keeps WT32 digital noise off the codec analog supply. The ES8311 draws about 10-20 mA.
- Bring the module GND to the single ground point (section 6), right next to the WT32 GND pin.
- Keep the I2S wires short (under ~10 cm). BCLK is only about 0.5-1 MHz, but a long BCLK wire still couples into the audio.

# ESP32RXVoter - Build Plan

Receive-only voter client on a **WT32-ETH01 by itself**. No codec, no Teensy, no SPI link.
Receive audio goes straight into the ESP32's ADC1, the same way the original VOTER board used its MCU's ADC.

## 1. What's in the firmware

| Area | Source | Notes |
|---|---|---|
| Voter protocol (auth, GPS packets, audio packets, CRC32 digests) | TeensyVoter `VoterClient` | Host audio ignored, jitter buffer removed |
| Timing (anchor at keyup + 20 ms frame counting + offset) | TeensyVoter `VoterClient` / `TIMING_AUTHORITY.md` | Offset configurable (default -120 ms, same as Teensy) |
| GPS (NMEA + PPS, lock status, holdover) | TeensyVoter `GPSManager` | `esp_timer` instead of `micros()`, spinlock instead of `noInterrupts()` |
| Audio capture | New | ADC1 continuous (DMA) at 32 kHz → 63-tap FIR → decimate ×4 → 8 kHz. DC removed in software |
| DSP (noise RSSI, 65-tap PL filter, de-emphasis, digital gain, uLaw) | TeensyVoter `DSPProcessor` / `VoiceFilter` | Plain C instead of CMSIS |
| COS: hardware / DSP / always-on, 200 ms tail frames | TeensyVoter `main.cpp` | Same logic |
| Ethernet first, WiFi fallback, setup hotspot, PHY flap recovery | Spirit ESP32 firmware | Same thresholds |
| Web UI: Status / Voter / Radio / Network / WiFi | Spirit ESP32 firmware | Settings go straight to NVS. Status page refreshes every 1 s |
| Syslog | TeensyVoter `Logger` | Hostname or IP |
| Serial console | New | `status`, `reboot`, `reset` (factory defaults) |

**Removed (TX side):** PTT, jitter buffer, upsampler, uLaw decode, TX gain, test tone, PTT tail.
**RSSI:** selectable on the Radio page.
- **DSP noise RSSI** (default) needs nothing extra. Same method as Voter2.
- **Hardware RSSI** reads a voltage on IO39 (ADC1_CH3), sampled in the same DMA stream as audio, averaged per 20 ms frame, and mapped from the calibrated Min/Max to 0-255. On the CDM 1250 this is accessory pin 15.

### Code layout
```
ESP32RXVoter/
  platformio.ini        env: wt32-eth01
  include/Pins.h        all pin assignments
  src/main.cpp          setup, loop, audio task, serial commands
  src/AudioInput.cpp    ADC1 DMA capture (audio + RSSI) + decimation to 8 kHz
  src/DSPProcessor.cpp  RSSI / PL filter / de-emphasis / uLaw
  src/GPSManager.cpp    NMEA + PPS
  src/VoterClient.cpp   voter protocol
  src/NetManager.cpp    ETH / WiFi / hotspot
  src/WebUI.cpp         web pages + /api/status
  src/Config.cpp        NVS settings
  src/Logger.cpp        serial + syslog
```

### Task layout
```
core 1  audioTask  (prio 10)  ADC read (20 ms) -> decimate -> DSP -> uLaw -> COS -> frame queue (25 deep)
core 1  loop       (prio 1)   GPS, network, voter protocol (drains queue), web, syslog, serial
core 0  WiFi / ETH / lwIP     (IDF)
```

## 2. Parts list

| Qty | Part | Notes |
|---|---|---|
| 1 | WT32-ETH01 | |
| 1 | GPS module with PPS output | 3.3 V logic. Existing GT-U7 class module is fine |
| 1 | TRACO TSR 1-2450 | 5 V switching regulator (WIRING.md section 5) |
| 1 | PTC fuse 500 mA, SS34 Schottky, SMBJ18A TVS | Input protection |
| - | Resistors, capacitors, ferrite bead | See WIRING.md |
| 1 | 3.3 V USB-serial adapter + jumper | Flashing (IO0 to GND) |
| 1 | CDM 16-pin accessory plug | |

## 3. Build steps

### Step 1 - Power and flashing
1. Build the power circuit (WIRING.md section 5). Check for 5.0 V at the output before connecting the WT32.
2. Flash: USB-serial TX→RX0 (IO3), RX→TX0 (IO1), GND. Hold IO0 to GND, then power up or reset.
   ```
   pio run -t upload --upload-port COMx
   ```
   Remove the IO0 jumper and reset.
3. Serial console at 115200: check for the banner, `[Audio] ADC1 audio CH0 (GPIO36) at 32000 Hz ... RSSI CH3 (GPIO39)`, `[NET] Ethernet started`, `[ETH-EVT] GOT_IP`.
4. Browse to the IP.
   - **Pass:** Status page loads, Network = ETH.

### Step 2 - Audio input
1. Build the RX audio input network (WIRING.md section 4.1) on IO36.
2. Status page, Receiver card: **Input bias** should read 1000-2400 counts (green) with nothing connected.
3. Set Radio → COS Mode = **Always On**. Feed a 1 kHz tone through the input network.
4. "ADC peak" tracks the tone level. Clipping on the ADC side is around -1 dBFS.
5. Check the frame counters: dropped = 0, ADC errors = 0, overruns = 0.
   - **Pass:** clean level tracking, counters at zero.

### Step 3 - GPS
1. Wire GPS TX → IO35, PPS → IO4, power from WT32 3V3. Antenna with sky view.
2. Status page: GPS = LOCKED, sats ≥ 3, PPS jitter in the tens of µs.
3. After about 60 s locked: "Sample clock vs GPS" shows a ppm value.
   - **Expect:** within about ±50 ppm.
   - **If about +100% (frames twice as fast) or -50%:** the driver treats the sample rate per channel rather than total (64 kHz is set for audio + RSSI). Tell me.
   - **If hundreds of ppm:** the ADC DMA clock divider doesn't land exactly on 32 kHz. Tell me the number. Fixes are a different `ADC_SAMPLE_RATE` (e.g. 40000 with decimation ×5) or a fractional decimator.
   - **Pass:** LOCKED, ppm small and stable.

### Step 4 - Voter server
1. Add the client to `voter.conf` with the client password from the Voter page (default `esp32rx1`).
2. Set host, port and both passwords on the Voter page. Save (reboots).
3. Serial: `Host Password Accepted` → `Authentication Successful! Connected to Host.` Status page: Voter = CONNECTED.
4. COS Mode = Always On with the test tone: check the host receives this client's audio.
   - **Pass:** connected, the host shows RSSI for this client, and the tone is clean.

### Step 5 - CDM radio
1. CPS programming (names per the CDM CPS Accessories tab; check against your CPS version):
   - RX audio on the accessory pin: **Flat Unsquelched**. DSP RSSI and DSP squelch need the noise above 2.4 kHz.
   - Program accessory pin 12 (DIG_IN_OUT_7) as **channel activity / carrier detect** for COS.
2. Wire the CDM connector (WIRING.md section 4).
3. Radio page: COS Mode = Hardware. Key a test radio and check the status page COS shows OPEN only while keyed. If it's reversed, toggle Invert COS.
4. Set the level with full-deviation voice:
   1. Choose the input series resistor (R1) so **ADC peak ≈ -6 to -8 dBFS**. This uses most of the ADC range for best SNR.
   2. Lower **Digital Gain** until **Encoder peak ≈ -3 to -6 dB** (0 dB = uLaw clip).
5. Leave PL Filter and De-emphasis **on** (flat audio).
6. RSSI. This matters most, because the voter selects the receiver on this value.
   - **DSP RSSI:** no signal → low RSSI byte; full-quieting signal → high. Adjust "DSP RSSI Calibration" until weak and strong signals spread across the range.
   - **Hardware RSSI** (if wired, WIRING.md 4.3): read "RSSI pin (raw counts)" on the Status page with no signal and with a full-quieting signal. Enter those as RSSI Min / Max, then tick "Use Hardware RSSI".
   - **Pass:** clean audio on the host. The voter picks this receiver on strong signals and drops it on weak ones.

### Step 6 - Multi-receiver test
1. Run alongside the Teensy unit or another receiver on the same signal.
2. Check the host aligns both streams with no echo or phasing. If audio is offset, adjust Timing Offset (Voter page) in 20 ms steps.

## 4. Open items / risks

| Item | Risk | Action |
|---|---|---|
| ESP32 ADC audio quality | About 9-10 effective bits, some nonlinearity. Adequate for FM voice (the original VOTER also used an MCU ADC), but not hi-fi | Step 2 / 5 listening check. Keep the input near mid-range (Input bias reading) |
| ADC DMA sample rate accuracy | The ESP32 derives the ADC rate from an I2S divider. 64 kHz total (32 kHz audio + 32 kHz RSSI) may not be exact | Step 3 shows the error in ppm |
| uLaw clip at 8159 | Inherited from TeensyVoter: the encoder skips the standard `>> 2`, so it clips at -12 dBFS of the int16 range. Kept for parity | Use Digital Gain to set "Encoder peak"; the ADC itself can run hotter |
| Ethernet noise on the ADC | The LAN8720 and 50 MHz clock sit next to the ESP32 | Short input wiring, filter cap at the pin, single-point ground (WIRING.md) |
| No OTA | Updates need the serial adapter + IO0 jumper | — |

## 5. Default settings (factory reset: serial `reset`)

| Setting | Default |
|---|---|
| Host / port | 10.10.10.40 : 1667 |
| Client / host password | esp32rx1 / (same host password as TeensyVoter) |
| Timing offset | -120 ms |
| COS mode / invert | Hardware / off (active low at IO14) |
| RSSI source / min / max | DSP / 200 / 3000 raw counts |
| DSP squelch / calibration | 30 / 50.0 |
| Digital gain | 100 % |
| PL filter / de-emphasis | on / on |
| Network | DHCP, WiFi fallback blank |
| Setup hotspot | `ESP32RXVoter_Config` at 10.5.5.5, after 15 s with no network, 5 min timeout |

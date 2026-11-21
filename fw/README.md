# Kame32 ESP32-C6 firmware

Standalone PlatformIO Arduino project for a Waveshare ESP32-C6-Zero, eight servos
on PCA9685 channels 0–4 and 12–14, BLE control, a shared I2C display, and a VL53L0X distance sensor.

| Leg | Coxa | Femur |
| --- | --- | --- |
| Right front | RFC (0) | RFF (13) |
| Left front | LFC (1) | LFF (4) |
| Right back | RBC (12) | RBF (14) |
| Left back | LBC (2) | LBF (3) |
The old `../code` project is independent.

Open `kame32.code-workspace` in VS Code. It reuses the editor preferences and
PlatformIO task style from your Dual_Display_ESP32 workspace. Build, Upload,
Monitor and Build and Flash tasks are included. From a terminal:

```sh
pio run
pio run -t upload
pio device monitor
```

Hold BOOT while resetting if the first USB upload needs download mode.
The Waveshare ESP32-C6-Zero profile uses PlatformIO Espressif32 8.0.0
with Arduino support for the C6 and NimBLE-Arduino 2.5.2 for core compatibility.
A conservative 4 MB flash layout works on
both the older 4 MB and newer 8 MB revisions; no PSRAM is required.
SDA is GPIO0, SCL is GPIO1 and PCA9685 OE is GPIO2.
Pins, addresses, servo limits and timeout are in
`include/config.h`. Wiring is in [doc/schematic.md](doc/schematic.md).

Build with `pio run -e waveshare-esp32-c6-zero-pwm-debug` to start PWM at
1500 µs on all eight channels, clamped to each servo's saved limits, and hold the current pose through BLE idle and
disconnect. STOP, including the Android app's initial STOP on connection,
releases the outputs until a new pose or SET command. Firmware updates and
receive faults also release the outputs.

## Wi-Fi servo controls

Connect to Kame32 in the Android app over BLE and open **Wi-Fi settings**.
Enter the name and password of your 2.4 GHz Wi-Fi network, then tap **Save and
connect**. Accept the Android Bluetooth pairing prompt if it appears. The
ESP32 stores the credentials in nonvolatile flash and reconnects after a reboot.
Leave the password blank for an open network.

Once the ESP32 joins, the app shows its local address and enables **Open servo
web page**. Use **Check Wi-Fi status** after reconnecting over BLE. The page
has eight sliders and a STOP button. Use it on a trusted local network, where
devices that can reach the ESP32 can use the servo controls.

Open **Servo limits** on the web page to set each servo's minimum and maximum
pulse width. Select **Save limits** to apply them and store them in nonvolatile
flash across reboots. Each pair must satisfy 500 µs ≤ minimum ≤ maximum ≤
2500 µs. The control sliders use the saved bounds. If a new limit excludes a
currently applied pulse, saving releases all servo outputs.

Keep the page visible to hold the outputs with the standard firmware. They
release after two seconds without a keepalive. The PWM debug profile holds
the last pulse widths until STOP. BLE and Wi-Fi share the same outputs, so
use one controller at a time.

## Startup RGB LED

The onboard RGB LED (GPIO8) shows dim blue during initialization. Once BLE
advertising starts, it shows green if PCA9685 and all enabled I2C peripherals
initialized, or yellow if any are missing/faulty. After 1.5 seconds it turns
off without delaying command processing. Red stays on for command-queue
allocation failure or failure to start BLE advertising. Battery wiring and
voltage are not validated by the startup LED.

Set `StartupLedEnabled`, `StartupLedBrightness` and `StartupLedHoldMs` in
`include/config.h` to configure this indication. No extra LED wiring is needed.

## Display

The project uses a **0.91-inch 128x32 I2C OLED**, configured with the SSD1306
driver at **0x3C**. Set `DisplayEnabled = false` to omit the screen.
A missing display does not block BLE/servo operation. Two four-line pages
alternate every three seconds: BLE connection and battery voltage, PCA presence
and output state, distance, and the first 21 characters of the last response;
then all eight channel pulse widths. When PWM values change, the next OLED
refresh shows the pulse widths and keeps that page visible for three seconds
after the last change. Full responses remain available over BLE and Serial.

## Distance sensor

A VL53L0X breakout at 0x29 shares GPIO0/SDA and GPIO1/SCL with the display
and PCA9685. See the wiring schematic for power and XSHUT connections.
`RangeEnabled`, `RangeAddress`, `RangePeriodMs` (100 ms) and `RangeStaleMs`
(500 ms) are configured in `include/config.h`. Continuous measurements are
polled without waiting for a conversion. Only sensor status 0 is accepted;
invalid or stale readings appear as `ToF --` on the OLED. A missing or faulty
sensor does not prevent BLE/servo operation. Disable it with `RangeEnabled = false`.
This adds distance telemetry; it does not implement obstacle avoidance.

## Battery voltage

Battery sensing uses GPIO3/ADC1_CH3 and an **external voltage divider**; see
[the wiring diagram](doc/schematic.md#battery-voltage-measurement) before connecting
power. The pack is 2S, assuming standard Li-ion/LiPo cells: 8.4 V fully charged.
Use 100 kΩ from battery positive to GPIO3, 22 kΩ from GPIO3 to GND, and
100 nF from GPIO3 to GND. At 8.4 V the ADC sees approximately 1.515 V.
Configure `BatteryEnabled`, `BatteryPin`, `BatteryTopOhms`, `BatteryBottomOhms`
and `BatteryCalibration` in `include/config.h` to match the hardware.

One calibrated ADC sample is taken every 25 ms, and eight samples are averaged
for a voltage update about every 200 ms. The OLED shows volts; `BATTERY` returns
millivolts. Before the first average or when an ADC sample reaches the configured
2.4 V measurement ceiling, the reading is unavailable. A disconnected battery
with a fitted divider reads near zero. Set `BatteryEnabled = false` without
sensing hardware. No charge percentage or low-voltage cutoff is implemented.

## BLE protocol

Advertised name: `Kame32`. Nordic UART-style UUIDs:

| Characteristic | UUID | Properties |
| --- | --- | --- |
| Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | service |
| RX | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` | write with response |
| TX | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` | read, notify |
| Pose | `6e400004-b5a3-f393-e0a9-e50e24dcca9e` | write with response |
| Firmware update | `6e400005-b5a3-f393-e0a9-e50e24dcca9e` | write with response |

Write exactly eight raw bytes to Pose in channel order **0, 1, 2, 3, 4, 12,
13, 14**. Eight zero bytes disable all outputs and report `OK STOP` on TX.
For any other frame, each byte selects a position across the 500–2500 µs pulse
range. Byte values 0 and 1 both select the minimum, 128 selects the midpoint,
and 255 selects the maximum. Values 1–255 are spaced evenly across the range.
The firmware clamps each resulting pulse to that servo's saved limits.
A successful pose reports `OK POSE` on TX. Resend the pose within two seconds
to maintain PWM output.

RX also accepts the existing text commands:

Use a BLE GATT client, subscribe to TX, and write one ASCII command per RX
write (at most 63 bytes; optional trailing LF). BLE pairing is not required;
any nearby client can connect and command the servos. No Bluetooth Classic/SPP.

| Command | Action |
| --- | --- |
| `SET 0 1500` | Set channel 0 to 1500 microseconds and enable outputs |
| `SET 14 1000` | Set channel 14 to 1000 microseconds |
| `PING` | Keep active outputs alive; response `OK PONG` |
| `RANGE` | Read cached distance: `OK RANGE <n> mm`; ERR if disabled, missing/faulty, or unavailable; does not refresh servo timeout |
| `BATTERY` | Read cached voltage: `OK BATTERY <n> mV`; ERR if disabled or unavailable; does not refresh servo timeout |
| `STOP` | Disable all outputs immediately when processed |

Commands use physical PCA9685 channels 0–4 and 12–14 with PWM at 50 Hz.
`SET` rejects pulse widths outside the selected servo's saved limits. Calibrate
limits to the servos/linkage before expanding them. Send Pose, SET or PING more
often than every two seconds while holding a pose. Boot, disconnect, timeout,
queue overflow and malformed BLE frames disable outputs. Invalid text commands
return ERR without refreshing the timeout. Reconnection does not restore a pose.
Disabled PWM removes holding torque; it does not disconnect servo power.
Pose writes command all eight configured channels. Legacy SET commands update a
single channel. Channels 5–11 and 15 remain off.

## BLE firmware updates

Flash an OTA-capable build over USB once with `pio run -t upload`. For later
updates, build with `pio run` and select
`.pio/build/waveshare-esp32-c6-zero/firmware.bin` in the Android controller.
The 4 MB partition table has two 1920 KiB app slots. Install the Wi-Fi build
over USB once to write the larger partition table, then use an updated Android
app for later BLE firmware updates.

Connect to Kame32, tap **Install firmware over BLE**, and hold the onboard
**BOOT** button while confirming **Install**. Release BOOT when the transfer
starts. Keep the app open until it reports success, then reconnect after the
board restarts. The firmware disables servo PWM during the transfer, writes
to the inactive slot, and checks the ESP image before selecting it for boot.
An interrupted transfer before verification leaves the current boot slot
selected. Use a trusted Kame32 build and keep USB available for recovery if
the new firmware fails to boot.

The update characteristic accepts three binary operations. Use write with
response and wait for the corresponding TX notification before sending the
next operation.

| Payload | TX notification |
| --- | --- |
| `01` followed by the image size as an unsigned 32-bit little-endian integer | `OTA READY` |
| `02` followed by image bytes in order, up to the smaller of MTU minus 4 and 243 bytes | `OTA OK` |
| `03` after sending the declared number of bytes | `OTA DONE`, followed by reboot |

The board checks BOOT on the first operation and reports `OTA HOLD BOOT`
if the button is released. Errors report `OTA ERR ...`. A disconnect or
15 seconds of inactivity aborts an unfinished update.

## Bench verification

Build first. Check blue during startup, then green (or yellow with an enabled
I2C peripheral unplugged), followed by LED off after 1.5 seconds. With linkage removed, check OE remains high at boot and the
screen reports PCA OK. Connect BLE and send `SET 0 1500`; measure roughly
50 Hz / 1.5 ms on channel 0 before attaching a servo. Check channels 4, 12, 13 and 14 and
rejection of channels 5–11 and 15, 999 µs, 2001 µs and extra tokens. Check a Pose write with eight bytes of value 128 for 1.5 ms pulses on all
eight channels. Verify that eight bytes of value 1 command minimum pulses and
that an all-zero frame or a short frame disables the outputs. Check STOP,
two-second timeout, disconnect and reconnection with all outputs off.
Check `RANGE` against a target at several measured distances and confirm the
OLED agrees. Check unavailable readings with no usable target, boot with the
sensor unplugged, and unplug it during operation to check stale/fault handling.
Confirm `RANGE` alone does not keep servo outputs active.
Before connecting GPIO3, measure the divider midpoint with a multimeter at the
maximum intended battery voltage and confirm it stays below 2.4 V. Compare
`BATTERY` and OLED voltage with a multimeter at several battery voltages.
Adjust `BatteryCalibration` by `actual voltage / reported voltage` (multiply the
existing factor by this ratio). Verify `BATTERY` alone does not keep servos active.
Repeat with the display unplugged. Firmware compilation does not verify
physical pinout, power sizing, PWM calibration or display compatibility.

## Driver references

- [Waveshare ESP32-C6-Zero documentation and pinout](https://docs.waveshare.com/ESP32-C6-Zero)
- [PlatformIO Espressif32 8.0.0](https://github.com/platformio/platform-espressif32/releases/tag/v8.0.0)
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)
- [Adafruit PCA9685 driver](https://github.com/adafruit/Adafruit-PWM-Servo-Driver-Library)
- [Adafruit SSD1306 driver](https://github.com/adafruit/Adafruit_SSD1306)

- [Adafruit VL53L0X driver](https://github.com/adafruit/Adafruit_VL53L0X)

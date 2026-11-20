#pragma once
#include <Arduino.h>

namespace config {
constexpr bool StartupLedEnabled = true;
constexpr uint8_t StartupLedBrightness = 16; // 0..255
constexpr uint32_t StartupLedHoldMs = 1500;
constexpr int Sda = 0;
constexpr int Scl = 1;
constexpr int OutputEnable = 2; // PCA9685 OE, active low; external 10k pull-up
constexpr int BootButton = 9; // Active-low onboard BOOT button
constexpr uint8_t PcaAddress = 0x40;
constexpr uint8_t DisplayAddress = 0x3C;
constexpr bool DisplayEnabled = true; // 0.91-inch SSD1306 128x32 I2C display
constexpr bool RangeEnabled = true;
constexpr uint8_t RangeAddress = 0x29;
constexpr uint16_t RangePeriodMs = 100;
constexpr uint32_t RangeStaleMs = 500;
constexpr bool BatteryEnabled = true;
constexpr int BatteryPin = 3; // ADC1_CH3; external voltage divider required
constexpr uint32_t BatteryTopOhms = 100000; // 2S Li-ion/LiPo (8.4 V full): battery positive -> ADC
constexpr uint32_t BatteryBottomOhms = 22000; // ADC -> ground
constexpr float BatteryCalibration = 1.0f; // multimeter voltage / reported voltage
constexpr uint32_t BatterySampleMs = 25;
constexpr uint8_t BatterySamples = 8;
constexpr uint32_t BatteryAdcLimitMv = 2400; // conservative measurement ceiling
static_assert(BatteryTopOhms > 0 && BatteryBottomOhms > 0 && BatterySamples > 0);
static_assert(BatteryCalibration > 0 && BatterySampleMs > 0);
constexpr uint8_t ServoChannels[] = {0, 1, 2, 3, 4, 12, 13, 14};
constexpr uint8_t ServoCount = sizeof(ServoChannels) / sizeof(ServoChannels[0]);
constexpr bool isServoChannel(unsigned channel) {
    for (uint8_t configured : ServoChannels)
        if (channel == configured) return true;
    return false;
}
constexpr uint16_t MinPulseUs = 500;
constexpr uint16_t MaxPulseUs = 2500;
constexpr uint16_t PwmHz = 50;
constexpr uint32_t TimeoutMs = 2000;
constexpr char DeviceName[] = "Kame32";
constexpr char ServiceUuid[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char RxUuid[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char TxUuid[] = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char PoseUuid[] = "6e400004-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char WifiUuid[] = "6e400006-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char OtaUuid[] = "6e400005-b5a3-f393-e0a9-e50e24dcca9e";
}

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <Adafruit_SSD1306.h>
#include <NimBLEDevice.h>
#include <Adafruit_VL53L0X.h>
#include <esp_ota_ops.h>
#include <atomic>
#include "config.h"

// BLE callbacks only enqueue data; loop owns I2C, PWM and status notifications.
static_assert(config::ServoCount == 8, "The BLE pose format requires eight servos");
struct Command {
    bool isPose;
    union {
        char text[64];
        uint8_t pose[config::ServoCount];
    };
};
QueueHandle_t commands;
constexpr uint16_t OtaMtu = 247;
constexpr uint32_t OtaTimeoutMs = 15000;
struct OtaPacket {
    uint16_t length;
    uint8_t bytes[OtaMtu - 3];
};
QueueHandle_t otaPackets;
const esp_partition_t *otaPartition = nullptr;
esp_ota_handle_t otaHandle;
uint32_t otaRemaining, otaLastPacket, otaFinishedAt;
bool otaRebootPending = false;
std::atomic<bool> connected{false};
std::atomic<bool> stopRequested{false};
Adafruit_PWMServoDriver pwm(config::PcaAddress);
Adafruit_SSD1306 display(128, 32, &Wire, -1);
Adafruit_VL53L0X rangeSensor;
bool rangeReady = false, rangeValid = false;
uint16_t distanceMm = 0;
uint32_t lastRange = 0;
uint32_t batteryMv = 0;
bool batteryValid = false;
NimBLECharacteristic *tx;
bool pcaReady = false, displayReady = false, active = false;
uint16_t pulses[16] = {};
uint32_t lastCommand = 0;
bool startupLedPending = false;
uint32_t startupLedSince = 0;

void startupLed(uint8_t red, uint8_t green, uint8_t blue) {
    if (config::StartupLedEnabled) rgbLedWrite(RGB_BUILTIN, red, green, blue);
}
char lastStatus[64] = "BOOT";

void stopOutputs() {
    digitalWrite(config::OutputEnable, HIGH);
    if (pcaReady) {
        for (uint8_t i = 0; i < 16; ++i) pwm.setPWM(i, 0, 4096);
    }
    memset(pulses, 0, sizeof(pulses));
    active = false;
}

void reply(const char *message) {
    snprintf(lastStatus, sizeof(lastStatus), "%s", message);
    Serial.println(message);
    tx->setValue(message);
    if (connected.load()) tx->notify();
}

void abortOta() {
    if (otaPartition) {
        esp_ota_abort(otaHandle);
        otaPartition = nullptr;
    }
}

void failOta(const char *message) {
    abortOta();
    xQueueReset(commands);
    xQueueReset(otaPackets);
    reply(message);
}

void processOta(const OtaPacket &packet) {
    const uint8_t *bytes = packet.bytes;
    if (!otaPartition && packet.length == 5 && bytes[0] == 1) {
        if (digitalRead(config::BootButton) != LOW) {
            failOta("OTA HOLD BOOT");
            return;
        }
        const uint32_t size = uint32_t(bytes[1]) | (uint32_t(bytes[2]) << 8) |
                              (uint32_t(bytes[3]) << 16) | (uint32_t(bytes[4]) << 24);
        const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
        stopOutputs();
        xQueueReset(commands);
        if (!target || size == 0 || size > target->size ||
            esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle) != ESP_OK) {
            failOta("OTA ERR BEGIN");
            return;
        }
        otaPartition = target;
        otaRemaining = size;
        otaLastPacket = millis();
        reply("OTA READY");
    } else if (otaPartition && packet.length > 1 && bytes[0] == 2) {
        const uint16_t size = packet.length - 1;
        if (size > otaRemaining || esp_ota_write(otaHandle, bytes + 1, size) != ESP_OK) {
            failOta("OTA ERR WRITE");
            return;
        }
        otaRemaining -= size;
        otaLastPacket = millis();
        tx->setValue("OTA OK");
        if (connected.load()) tx->notify();
    } else if (otaPartition && otaRemaining == 0 && packet.length == 1 && bytes[0] == 3) {
        const esp_partition_t *target = otaPartition;
        otaPartition = nullptr; // esp_ota_end consumes the handle even on error.
        if (esp_ota_end(otaHandle) != ESP_OK || esp_ota_set_boot_partition(target) != ESP_OK) {
            failOta("OTA ERR IMAGE");
            return;
        }
        otaRebootPending = true;
        otaFinishedAt = millis();
        reply("OTA DONE");
    } else {
        failOta("OTA ERR PACKET");
    }
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, NimBLEConnInfo &) override {
        connected = true;
    }
    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
        connected = false;
        stopRequested = true;
        // Disable immediately, even if loop is currently updating the display.
        digitalWrite(config::OutputEnable, HIGH);
        NimBLEDevice::startAdvertising();
    }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &) override {
        const std::string value = characteristic->getValue();
        Command command{};
        if (value.empty() || value.size() >= sizeof(command.text) ||
            value.find('\0') != std::string::npos) {
            stopRequested = true;
            return;
        }
        memcpy(command.text, value.data(), value.size());
        if (xQueueSend(commands, &command, 0) != pdTRUE) stopRequested = true;
    }
};

class PoseCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &) override {
        const std::string value = characteristic->getValue();
        Command command{};
        if (value.size() != sizeof(command.pose)) {
            stopRequested = true;
            return;
        }
        command.isPose = true;
        memcpy(command.pose, value.data(), sizeof(command.pose));
        if (xQueueSend(commands, &command, 0) != pdTRUE) stopRequested = true;
    }
};

class OtaCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &) override {
        const auto value = characteristic->getValue();
        OtaPacket packet{};
        if (value.size() == 0 || value.size() > sizeof(packet.bytes)) {
            stopRequested = true;
            return;
        }
        packet.length = value.size();
        memcpy(packet.bytes, value.data(), packet.length);
        if (xQueueSend(otaPackets, &packet, 0) != pdTRUE) stopRequested = true;
    }
};

// Poll continuous measurements without waiting for a conversion in the control loop.
void updateRange() {
    if (!rangeReady) return;
    if (rangeSensor.isRangeComplete()) {
        if (rangeSensor.Status != VL53L0X_ERROR_NONE) {
            rangeReady = rangeValid = false;
            return;
        }
        VL53L0X_RangingMeasurementData_t measurement{};
        const auto result = rangeSensor.getRangingMeasurement(&measurement);
        const auto cleared = rangeSensor.clearInterruptMask();
        if (result != VL53L0X_ERROR_NONE || cleared != VL53L0X_ERROR_NONE) {
            rangeReady = rangeValid = false;
            return;
        }
        distanceMm = measurement.RangeMilliMeter;
        rangeValid = measurement.RangeStatus == 0;
        lastRange = millis();
    }
    if (millis() - lastRange > config::RangeStaleMs) rangeValid = false;
}

// Spread averaging across loop iterations so BLE commands remain responsive.
void updateBattery() {
    if (!config::BatteryEnabled) return;
    static uint32_t lastSample = 0, sumMv = 0;
    static uint8_t samples = 0;
    static bool clipped = false;
    const uint32_t now = millis();
    if (now - lastSample < config::BatterySampleMs) return;
    lastSample = now;
    const uint32_t sample = analogReadMilliVolts(config::BatteryPin);
    sumMv += sample;
    clipped |= sample >= config::BatteryAdcLimitMv;
    if (++samples == config::BatterySamples) {
        const float adcMv = static_cast<float>(sumMv) / samples;
        batteryMv = static_cast<uint32_t>(adcMv *
            (1.0f + static_cast<float>(config::BatteryTopOhms) / config::BatteryBottomOhms) *
            config::BatteryCalibration + 0.5f);
        batteryValid = !clipped;
        sumMv = 0;
        samples = 0;
        clipped = false;
    }
}

bool writeServo(uint8_t channel, uint16_t pulse) {
    const uint16_t ticks = (pulse * config::PwmHz * 4096UL + 500000UL) / 1000000UL;
    if (pwm.setPWM(channel, 0, ticks) != 0) {
        stopOutputs();
        reply("ERR I2C write");
        return false;
    }
    pulses[channel] = pulse;
    return true;
}

void enableOutputs() {
    lastCommand = millis();
    active = true;
    if (connected.load() && !stopRequested.load()) digitalWrite(config::OutputEnable, LOW);
}

void processPose(const uint8_t *pose) {
    bool allZero = true;
    for (uint8_t i = 0; i < config::ServoCount; ++i) allZero &= pose[i] == 0;
    if (allZero) {
        stopOutputs();
        reply("OK STOP");
        return;
    }
    if (!pcaReady) { reply("ERR PCA9685 missing"); return; }
    for (uint8_t i = 0; i < config::ServoCount; ++i) {
        const uint8_t position = pose[i] == 0 ? 0 : pose[i] - 1;
        const uint16_t pulse = config::MinPulseUs +
            (position * (config::MaxPulseUs - config::MinPulseUs) + 127U) / 254U;
        const uint8_t channel = config::ServoChannels[i];
        if (pulses[channel] != pulse && !writeServo(channel, pulse)) return;
    }
    enableOutputs();
    reply("OK POSE");
}

void processCommand(const char *text) {
    unsigned channel, pulse;
    char extra;
    // A trailing newline is allowed; any other extra token is rejected.
    if (strcmp(text, "STOP") == 0 || strcmp(text, "STOP\n") == 0) {
        stopOutputs();
        reply("OK STOP");
    } else if (strcmp(text, "PING") == 0 || strcmp(text, "PING\n") == 0) {
        lastCommand = millis();
        reply("OK PONG");
    } else if (strcmp(text, "RANGE") == 0 || strcmp(text, "RANGE\n") == 0) {
        if (!config::RangeEnabled) reply("ERR VL53L0X disabled");
        else if (!rangeReady) reply("ERR VL53L0X missing/fault");
        else if (!rangeValid) reply("ERR RANGE unavailable");
        else {
            char message[32];
            snprintf(message, sizeof(message), "OK RANGE %u mm", distanceMm);
            reply(message);
        }
    } else if (strcmp(text, "BATTERY") == 0 || strcmp(text, "BATTERY\n") == 0) {
        if (!config::BatteryEnabled) reply("ERR BATTERY disabled");
        else if (!batteryValid) reply("ERR BATTERY unavailable");
        else {
            char message[32];
            snprintf(message, sizeof(message), "OK BATTERY %lu mV", static_cast<unsigned long>(batteryMv));
            reply(message);
        }
    } else if (sscanf(text, "SET %3u %4u %c", &channel, &pulse, &extra) == 2 &&
               config::isServoChannel(channel) && pulse >= config::MinPulseUs &&
               pulse <= config::MaxPulseUs) {
        if (!pcaReady) { reply("ERR PCA9685 missing"); return; }
        if (writeServo(channel, pulse)) {
            enableOutputs();
            reply("OK SET");
        }
    } else {
        reply("ERR use SET ch us / STOP / PING / RANGE / BATTERY");
    }
}

void setup() {
    pinMode(config::OutputEnable, OUTPUT);
    digitalWrite(config::OutputEnable, HIGH);
    pinMode(config::BootButton, INPUT_PULLUP);
    startupLed(0, 0, config::StartupLedBrightness);
    Serial.begin(115200);
    if (config::BatteryEnabled) {
        pinMode(config::BatteryPin, INPUT);
        analogReadResolution(12);
        analogSetPinAttenuation(config::BatteryPin, ADC_11db);
    }
    Wire.begin(config::Sda, config::Scl, 100000);
    Wire.setTimeOut(25);
    pcaReady = pwm.begin();
    if (pcaReady) pwm.setPWMFreq(config::PwmHz);
    stopOutputs();
    if (config::DisplayEnabled) {
        Wire.beginTransmission(config::DisplayAddress);
        if (Wire.endTransmission() == 0) {
            displayReady = display.begin(SSD1306_SWITCHCAPVCC, config::DisplayAddress, false, false);
            if (displayReady) { display.setTextSize(1); display.setTextColor(SSD1306_WHITE); }
        }
    }
    if (config::RangeEnabled) {
        Wire.beginTransmission(config::RangeAddress);
        if (Wire.endTransmission() == 0) {
            rangeReady = rangeSensor.begin(config::RangeAddress, false, &Wire) &&
                         rangeSensor.startRangeContinuous(config::RangePeriodMs);
        }
        Serial.println(rangeReady ? "VL53L0X ready" : "ERR VL53L0X missing/fault");
    }
    commands = xQueueCreate(8, sizeof(Command));
    otaPackets = xQueueCreate(1, sizeof(OtaPacket));
    if (!commands || !otaPackets) {
        startupLed(config::StartupLedBrightness, 0, 0);
        Serial.println("ERR command queue");
        while (true) delay(1000);
    }
    NimBLEDevice::init(config::DeviceName);
    NimBLEDevice::setMTU(OtaMtu);
    auto server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());
    auto service = server->createService(config::ServiceUuid);
    auto rx = service->createCharacteristic(config::RxUuid, NIMBLE_PROPERTY::WRITE);
    rx->setCallbacks(new RxCallbacks());
    auto pose = service->createCharacteristic(config::PoseUuid, NIMBLE_PROPERTY::WRITE);
    pose->setCallbacks(new PoseCallbacks());
    auto ota = service->createCharacteristic(config::OtaUuid, NIMBLE_PROPERTY::WRITE);
    ota->setCallbacks(new OtaCallbacks());
    tx = service->createCharacteristic(config::TxUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    tx->setValue(pcaReady ? "READY" : "ERR PCA9685 missing");
    service->start();
    auto advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(config::ServiceUuid);
    advertising->setName(config::DeviceName);
    advertising->enableScanResponse(true);
    if (!advertising->start()) {
        startupLed(config::StartupLedBrightness, 0, 0);
        Serial.println("ERR BLE advertising");
    } else {
        const bool peripheralsReady = pcaReady &&
            (!config::DisplayEnabled || displayReady) && (!config::RangeEnabled || rangeReady);
        startupLed(peripheralsReady ? 0 : config::StartupLedBrightness,
                   config::StartupLedBrightness, 0);
        startupLedSince = millis();
        startupLedPending = true;
    }
}

void loop() {
    if (otaRebootPending) {
        if (millis() - otaFinishedAt >= 750) ESP.restart();
        delay(5);
        return;
    }
    if (startupLedPending && millis() - startupLedSince >= config::StartupLedHoldMs) {
        startupLed(0, 0, 0);
        startupLedPending = false;
    }
    if (stopRequested.exchange(false)) {
        xQueueReset(commands);
        xQueueReset(otaPackets);
        abortOta();
        stopOutputs();
        reply("STOP disconnect or RX fault");
    }
    OtaPacket otaPacket;
    if (xQueueReceive(otaPackets, &otaPacket, 0) == pdTRUE && connected.load() && !stopRequested.load()) {
        processOta(otaPacket);
    }
    if (otaRebootPending) return;
    if (otaPartition) {
        xQueueReset(commands);
        if (millis() - otaLastPacket > OtaTimeoutMs) failOta("OTA ERR TIMEOUT");
        delay(1);
        return;
    }
    updateRange();
    updateBattery();
    Command command{};
    if (xQueueReceive(commands, &command, 0) == pdTRUE && connected.load() && !stopRequested.load()) {
        if (command.isPose) processPose(command.pose);
        else processCommand(command.text);
    }
    if (active && (!connected.load() || millis() - lastCommand > config::TimeoutMs)) {
        stopOutputs();
        reply("STOP timeout/disconnect");
    }
    static uint32_t lastDisplay = 0;
    if (displayReady && millis() - lastDisplay >= 250) {
        lastDisplay = millis();
        display.clearDisplay();
        display.setCursor(0, 0);
        display.setTextWrap(false);
        // Four 8-pixel rows per page on the 128x32 OLED.
        if ((lastDisplay / 3000) % 2 == 0) {
            display.printf("BLE %s Bat ", connected.load() ? "ON" : "OFF");
            if (!config::BatteryEnabled) display.println("OFF");
            else if (!batteryValid) display.println("--");
            else display.printf("%lu.%02luV\n", static_cast<unsigned long>(batteryMv / 1000),
                                static_cast<unsigned long>((batteryMv % 1000) / 10));
            display.printf("PCA %s  PWM %s\n", pcaReady ? "OK" : "MISSING", active ? "ON" : "OFF");
            if (!config::RangeEnabled) display.println("ToF OFF");
            else if (!rangeReady) display.println("ToF MISSING/FAULT");
            else if (!rangeValid) display.println("ToF --");
            else display.printf("ToF %u mm\n", distanceMm);
            display.printf("%.21s", lastStatus);
        } else {
            for (uint8_t i = 0; i < config::ServoCount; i += 2) {
                const uint8_t first = config::ServoChannels[i];
                const uint8_t second = config::ServoChannels[i + 1];
                display.printf("%2u:%4u  %2u:%4u\n", first, pulses[first], second, pulses[second]);
            }
        }
        display.display();
    }
    delay(5);
}

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Adafruit_PWMServoDriver.h>
#include <Adafruit_SSD1306.h>
#include <NimBLEDevice.h>
#include <Adafruit_VL53L0X.h>
#include <esp_ota_ops.h>
#include <atomic>
#include "config.h"
#include "servo_page.h"

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
enum class WebAction : uint8_t { Set, Stop, Hold };
struct WebCommand {
    WebAction action;
    uint8_t channel = 0;
    uint16_t pulse = 0;
};
QueueHandle_t webCommands;
bool webHolding = false;
std::atomic<bool> webControlAvailable{false};
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
std::atomic<uint32_t> bleSession{0};
std::atomic<bool> stopRequested{false};
std::atomic<bool> disconnected{false};
#ifdef KAME32_DEBUG_HOLD_PWM
constexpr bool debugHoldPwm = true;
#else
constexpr bool debugHoldPwm = false;
#endif
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
struct PulseLimits {
    uint16_t minimum = config::MinPulseUs;
    uint16_t maximum = config::MaxPulseUs;
    bool allows(unsigned pulse) const { return pulse >= minimum && pulse <= maximum; }
};
struct ServoLimits {
    PulseLimits channel[16];
};
constexpr char LimitsKey[] = "limits-v1";
ServoLimits servoLimits{};
Preferences servoPreferences;
bool canSaveLimits = false;
QueueHandle_t limitsRequests;

bool validLimits(unsigned minimum, unsigned maximum) {
    return minimum >= config::MinPulseUs && minimum <= maximum && maximum <= config::MaxPulseUs;
}

void loadServoLimits() {
    canSaveLimits = servoPreferences.begin("servo", false);
    ServoLimits saved{};
    if (!canSaveLimits || servoPreferences.getBytesLength(LimitsKey) != sizeof(saved) ||
        servoPreferences.getBytes(LimitsKey, &saved, sizeof(saved)) != sizeof(saved)) return;
    for (const auto &servo : config::ServoLocations) {
        const auto &limit = saved.channel[servo.channel];
        if (!validLimits(limit.minimum, limit.maximum)) return;
    }
    servoLimits = saved;
}

struct LimitsRequest {
    ServoLimits limits;
    TaskHandle_t requester;
};

bool pulsesChanged = false;
uint32_t lastCommand = 0;
bool startupLedPending = false;
uint32_t startupLedSince = 0;

void startupLed(uint8_t red, uint8_t green, uint8_t blue) {
    if (config::StartupLedEnabled) rgbLedWrite(RGB_BUILTIN, red, green, blue);
}
char lastStatus[64] = "BOOT";

void stopOutputs() {
    digitalWrite(config::OutputEnable, HIGH);
    webHolding = false;
    if (pcaReady) {
        for (uint8_t i = 0; i < 16; ++i) pwm.setPWM(i, 0, 4096);
    }
    pulsesChanged |= active;
    memset(pulses, 0, sizeof(pulses));
    active = false;
}

void processLimitsRequest() {
    LimitsRequest request;
    if (!limitsRequests || xQueueReceive(limitsRequests, &request, 0) != pdTRUE) return;
    uint32_t status = 503;
    if (!otaPartition && canSaveLimits) {
        status = 500;
        if (servoPreferences.putBytes(LimitsKey, &request.limits, sizeof(ServoLimits)) == sizeof(ServoLimits)) {
            servoLimits = request.limits;
            for (const auto &servo : config::ServoLocations) {
                const uint8_t channel = servo.channel;
                if (pulses[channel] && !servoLimits.channel[channel].allows(pulses[channel])) {
                    stopOutputs();
                    break;
                }
            }
            status = 200;
        }
    }
    xTaskNotify(request.requester, status, eSetValueWithOverwrite);
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
        webControlAvailable = false;
        stopOutputs();
        xQueueReset(commands);
        if (webCommands) xQueueReset(webCommands);
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
        ++bleSession;
        connected = false;
        if (debugHoldPwm) {
            disconnected = true;
        } else {
            stopRequested = true;
            // Disable immediately, even if loop is currently updating the display.
            digitalWrite(config::OutputEnable, HIGH);
        }
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
    pulsesChanged |= pulses[channel] != pulse;
    pulses[channel] = pulse;
    return true;
}

void enableOutputs(bool fromWeb = false) {
    lastCommand = millis();
    active = true;
    webHolding = fromWeb;
    if ((fromWeb || debugHoldPwm || connected.load()) && !stopRequested.load()) {
        digitalWrite(config::OutputEnable, LOW);
    }
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
        const uint16_t requested = config::MinPulseUs +
            (position * (config::MaxPulseUs - config::MinPulseUs) + 127U) / 254U;
        const uint8_t channel = config::ServoLocations[i].channel;
        const auto &limit = servoLimits.channel[channel];
        const uint16_t pulse = constrain(requested, limit.minimum, limit.maximum);
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
        if (!webHolding) lastCommand = millis();
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
        if (!servoLimits.channel[channel].allows(pulse)) { reply("ERR servo limits"); return; }
        if (!pcaReady) { reply("ERR PCA9685 missing"); return; }
        if (writeServo(channel, pulse)) {
            enableOutputs();
            reply("OK SET");
        }
    } else {
        reply("ERR use SET ch us / STOP / PING / RANGE / BATTERY");
    }
}

struct WifiSettings {
    char ssid[33];
    char password[64];
};

bool validWifiSettings(const WifiSettings &settings) {
    const size_t ssidSize = strnlen(settings.ssid, sizeof(settings.ssid));
    const size_t keySize = strnlen(settings.password, sizeof(settings.password));
    if (ssidSize == 0 || ssidSize >= sizeof(settings.ssid) || (keySize != 0 && keySize < 8) ||
        keySize >= sizeof(settings.password)) return false;
    for (size_t i = 0; i < ssidSize; ++i) {
        const auto ch = static_cast<uint8_t>(settings.ssid[i]);
        if (ch < 32 || ch == 127) return false;
    }
    for (size_t i = 0; i < keySize; ++i) {
        const auto ch = static_cast<uint8_t>(settings.password[i]);
        if (ch < 32 || ch > 126) return false;
    }
    return true;
}

struct WifiRequest {
    WifiSettings settings{};
    bool save = false;
    uint32_t session = 0;
};
QueueHandle_t wifiRequests;

void wifiReply(const char *message, uint32_t session) {
    if (connected.load() && bleSession.load() == session) {
        tx->notify(reinterpret_cast<const uint8_t *>(message), strlen(message));
    }
}

class WifiCallbacks : public NimBLECharacteristicCallbacks {
    uint8_t bytes[95]{};
    size_t used = 0, ssidSize = 0, keySize = 0;
    uint32_t session = 0;

    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &) override {
        const std::string packet = characteristic->getValue();
        const auto currentSession = bleSession.load();
        if (packet.size() == 1 && packet[0] == 0) {
            const WifiRequest request{{}, false, currentSession};
            if (xQueueSend(wifiRequests, &request, 0) != pdTRUE) wifiReply("WF Busy", currentSession);
            return;
        }
        if (packet.size() == 3 && packet[0] == 1) {
            used = 0;
            ssidSize = static_cast<uint8_t>(packet[1]);
            keySize = static_cast<uint8_t>(packet[2]);
            session = currentSession;
            if (ssidSize >= 1 && ssidSize <= 32 && keySize <= 63) return;
        } else if (packet.size() >= 2 && packet.size() <= 20 && packet[0] == 2 &&
                   session == currentSession && ssidSize && used + packet.size() - 1 <= ssidSize + keySize) {
            memcpy(bytes + used, packet.data() + 1, packet.size() - 1);
            used += packet.size() - 1;
            if (used < ssidSize + keySize) return;
            WifiRequest request{{}, true, currentSession};
            memcpy(request.settings.ssid, bytes, ssidSize);
            memcpy(request.settings.password, bytes + ssidSize, keySize);
            ssidSize = 0;
            if (!memchr(bytes, 0, used) && validWifiSettings(request.settings)) {
                if (xQueueSend(wifiRequests, &request, 0) != pdTRUE) wifiReply("WF Busy", currentSession);
                return;
            }
        }
        ssidSize = 0;
        wifiReply("WF Invalid settings", currentSession);
    }
};

String renderServoPage(const ServoLimits &limits, bool calibration) {
    String page(ServoPageHead);
    page += calibration ? LimitsPageStart : ServoPageStart;
    constexpr struct {
        const char *name;
        config::ServoLocation servos[2];
    } legs[] = {
        {"Left front", {config::LFC, config::LFF}},
        {"Right front", {config::RFC, config::RFF}},
        {"Left back", {config::LBC, config::LBF}},
        {"Right back", {config::RBC, config::RBF}},
    };
    for (const auto &leg : legs) {
        page += "<fieldset><legend>";
        page += leg.name;
        page += "</legend>";
        for (const auto &servo : leg.servos) {
            const auto &limit = limits.channel[servo.channel];
            char controls[360];
            if (calibration) {
                snprintf(controls, sizeof(controls),
                         "<div>%s</div><div class=bounds>"
                         "<label>Min<input type=number name=%s-min aria-label='%s minimum pulse width' "
                         "min=%u max=%u value=%u required></label>"
                         "<label>Max<input type=number name=%s-max aria-label='%s maximum pulse width' "
                         "min=%u max=%u value=%u required></label></div>",
                         servo.name, servo.name, servo.name, config::MinPulseUs, config::MaxPulseUs,
                         limit.minimum, servo.name, servo.name, config::MinPulseUs, config::MaxPulseUs,
                         limit.maximum);
            } else {
                const unsigned center = (limit.minimum + limit.maximum) / 2;
                snprintf(controls, sizeof(controls),
                         "<label>%s <output>%u µs</output>"
                         "<input type=range aria-label='%s pulse width' data-channel=%u data-location=%s "
                         "min=%u max=%u value=%u></label>",
                         servo.name, center, servo.name, static_cast<unsigned>(servo.channel), servo.name,
                         limit.minimum, limit.maximum, center);
            }
            page += controls;
        }
        page += "</fieldset>";
    }
    page += calibration ? LimitsPageEnd : ServoPageEnd;
    return page;
}

unsigned limitField(const String &value) {
    if (value.isEmpty() || value.length() > 4) return 0;
    unsigned pulse = 0;
    for (char digit : value) {
        if (digit < '0' || digit > '9') return 0;
        pulse = pulse * 10 + digit - '0';
    }
    return pulse;
}

void serveWifi(void *) {
    ServoLimits webLimits = servoLimits;
    Preferences preferences;
    const bool canSave = preferences.begin("wifi-sta", false);
    WifiSettings saved{};
    bool configured = canSave && preferences.getBytesLength("settings-v1") == sizeof(saved) &&
        preferences.getBytes("settings-v1", &saved, sizeof(saved)) == sizeof(saved) && validWifiSettings(saved);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    if (configured) WiFi.begin(saved.ssid, saved.password);

    WebServer server(80);
    const auto servePage = [&](bool calibration) {
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "text/html; charset=utf-8", renderServoPage(webLimits, calibration));
    };
    server.on("/", HTTP_GET, [&] { servePage(false); });
    server.on("/limits", HTTP_GET, [&] { servePage(true); });
    const auto submit = [&](WebCommand command) {
        if (server.header("X-Kame32-Control") != "1") {
            server.send(403);
        } else if (!webControlAvailable.load()) {
            server.send(503);
        } else if (xQueueSend(webCommands, &command, 0) == pdTRUE) {
            server.send(202);
        } else {
            server.send(503);
        }
    };
    const char *headers[] = {"X-Kame32-Control"};
    server.collectHeaders(headers, 1);
    server.on("/limits", HTTP_POST, [&] {
        if (server.header("X-Kame32-Control") != "1") {
            server.send(403, "text/plain", "Request rejected.");
            return;
        }
        LimitsRequest request{webLimits, xTaskGetCurrentTaskHandle()};
        for (const auto &servo : config::ServoLocations) {
            const unsigned minimum = limitField(server.arg(String(servo.name) + "-min"));
            const unsigned maximum = limitField(server.arg(String(servo.name) + "-max"));
            if (!validLimits(minimum, maximum)) {
                server.send(400, "text/plain", String(servo.name) +
                            ": enter 500–2500 µs with minimum at or below maximum.");
                return;
            }
            request.limits.channel[servo.channel] = {
                static_cast<uint16_t>(minimum), static_cast<uint16_t>(maximum),
            };
        }
        if (!limitsRequests || xQueueSend(limitsRequests, &request, 0) != pdTRUE) {
            server.send(503, "text/plain", "Limits could not be saved. Try again.");
            return;
        }
        // The control loop commits the limits and checks the active outputs before acknowledging.
        uint32_t status;
        xTaskNotifyWait(0, UINT32_MAX, &status, portMAX_DELAY);
        if (status == 200) {
            webLimits = request.limits;
            server.send(200, "text/plain", "Servo limits saved.");
        } else {
            server.send(status, "text/plain", "Limits could not be saved. Try again.");
        }
    });
    server.on("/servo", HTTP_POST, [&] {
        const String body = server.arg("plain");
        unsigned channel, pulse;
        char extra;
        if (sscanf(body.c_str(), "%3u %4u %c", &channel, &pulse, &extra) != 2 ||
            !config::isServoChannel(channel) || !webLimits.channel[channel].allows(pulse)) {
            server.send(400);
            return;
        }
        submit({WebAction::Set, static_cast<uint8_t>(channel), static_cast<uint16_t>(pulse)});
    });
    server.on("/stop", HTTP_POST, [&] { submit({WebAction::Stop}); });
    server.on("/hold", HTTP_POST, [&] { submit({WebAction::Hold}); });
    IPAddress lastIp;
    uint32_t reportingSession = 0;
    uint32_t joiningSince = millis();
    bool reportJoin = false;
    for (;;) {
        WifiRequest request;
        if (xQueueReceive(wifiRequests, &request, 0) == pdTRUE) {
            if (request.save) {
                if (!canSave || preferences.putBytes("settings-v1", &request.settings, sizeof(saved)) != sizeof(saved)) {
                    wifiReply("WF Save failed", request.session);
                } else {
                    saved = request.settings;
                    configured = true;
                    WiFi.disconnect(false, false, 1000);
                    WiFi.begin(saved.ssid, saved.password);
                    joiningSince = millis();
                    reportingSession = request.session;
                    reportJoin = true;
                    wifiReply("WF Saved, joining", request.session);
                }
            } else if (WiFi.status() == WL_CONNECTED) {
                wifiReply((String("IP ") + WiFi.localIP().toString()).c_str(), request.session);
            } else {
                wifiReply(configured ? "WF Not connected" : "WF Setup required", request.session);
                reportingSession = request.session;
                joiningSince = millis();
                reportJoin = configured;
            }
        }
        const IPAddress ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP() : IPAddress();
        if (ip != lastIp) {
            if (lastIp != IPAddress()) server.stop();
            if (ip != IPAddress()) {
                server.begin();
                Serial.printf("Wi-Fi servo control: http://%s/\n", ip.toString().c_str());
            }
            lastIp = ip;
        }
        if (reportJoin && ip != IPAddress()) {
            wifiReply((String("IP ") + ip.toString()).c_str(), reportingSession);
            reportJoin = false;
        } else if (reportJoin && millis() - joiningSince >= 20000) {
            wifiReply("WF Join timed out", reportingSession);
            reportJoin = false;
        }
        if (ip != IPAddress()) server.handleClient();
        vTaskDelay(1);
    }
}

void setup() {
    pinMode(config::OutputEnable, OUTPUT);
    digitalWrite(config::OutputEnable, HIGH);
    pinMode(config::BootButton, INPUT_PULLUP);
    startupLed(0, 0, config::StartupLedBrightness);
    Serial.begin(115200);
    loadServoLimits();
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
    NimBLEDevice::setSecurityAuth(true, false, true);
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
    if (debugHoldPwm && pcaReady) {
        uint8_t centeredPose[config::ServoCount];
        memset(centeredPose, 128, sizeof(centeredPose));
        processPose(centeredPose);
    }
    wifiRequests = xQueueCreate(2, sizeof(WifiRequest));
    if (wifiRequests) {
        auto wifi = service->createCharacteristic(config::WifiUuid,
            NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC, 20);
        wifi->setCallbacks(new WifiCallbacks());
    }
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
    webCommands = xQueueCreate(8, sizeof(WebCommand));
    limitsRequests = xQueueCreate(1, sizeof(LimitsRequest));
    webControlAvailable = pcaReady;
    if (!webCommands || !wifiRequests || xTaskCreate(serveWifi, "servo-wifi", 8192, nullptr, 1, nullptr) != pdPASS) {
        Serial.println("ERR Wi-Fi worker");
    }
}

void loop() {
    if (otaRebootPending) {
        if (millis() - otaFinishedAt >= 750) ESP.restart();
        delay(5);
        return;
    }
    processLimitsRequest();
    if (startupLedPending && millis() - startupLedSince >= config::StartupLedHoldMs) {
        startupLed(0, 0, 0);
        startupLedPending = false;
    }
    const bool linkLost = disconnected.exchange(false);
    const bool rxFault = stopRequested.exchange(false);
    if (linkLost || rxFault) {
        xQueueReset(commands);
        xQueueReset(otaPackets);
        abortOta();
        if (rxFault) {
            stopOutputs();
            reply("STOP disconnect or RX fault");
        }
    }
    OtaPacket otaPacket;
    if (xQueueReceive(otaPackets, &otaPacket, 0) == pdTRUE && connected.load() && !stopRequested.load()) {
        processOta(otaPacket);
    }
    if (otaRebootPending) return;
    if (otaPartition) {
        xQueueReset(commands);
        if (webCommands) xQueueReset(webCommands);
        if (millis() - otaLastPacket > OtaTimeoutMs) failOta("OTA ERR TIMEOUT");
        delay(1);
        return;
    }
    webControlAvailable = pcaReady;
    updateRange();
    updateBattery();
    Command command{};
    if (xQueueReceive(commands, &command, 0) == pdTRUE && connected.load() && !stopRequested.load()) {
        if (command.isPose) processPose(command.pose);
        else processCommand(command.text);
    }
    WebCommand webCommand;
    if (webCommands && !stopRequested.load() && xQueueReceive(webCommands, &webCommand, 0) == pdTRUE) {
        switch (webCommand.action) {
            case WebAction::Set:
                if (pcaReady && servoLimits.channel[webCommand.channel].allows(webCommand.pulse) &&
                    writeServo(webCommand.channel, webCommand.pulse)) enableOutputs(true);
                break;
            case WebAction::Stop:
                stopOutputs();
                break;
            case WebAction::Hold:
                if (webHolding) lastCommand = millis();
                break;
        }
    }
    if (!debugHoldPwm && active && ((!webHolding && !connected.load()) ||
                                   millis() - lastCommand > config::TimeoutMs)) {
        stopOutputs();
        reply("STOP timeout/disconnect");
    }
    static uint32_t lastDisplay = 0;
    static uint32_t displayCycleStart = 0;
    if (displayReady && millis() - lastDisplay >= 250) {
        lastDisplay = millis();
        constexpr uint32_t pageDurationMs = 3000;
        if (pulsesChanged) {
            displayCycleStart = lastDisplay - pageDurationMs;
            pulsesChanged = false;
        }
        display.clearDisplay();
        display.setCursor(0, 0);
        display.setTextWrap(false);
        // Four 8-pixel rows per page on the 128x32 OLED.
        if (((lastDisplay - displayCycleStart) / pageDurationMs) % 2 == 0) {
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
                const auto &first = config::ServoLocations[i];
                const auto &second = config::ServoLocations[i + 1];
                display.printf("%s:%4u  %s:%4u\n", first.name, pulses[first.channel],
                               second.name, pulses[second.channel]);
            }
        }
        display.display();
    }
    delay(5);
}

#include <Arduino.h>
#include "Zigbee.h"
#include <Wire.h>

// ====== Zigbee ===========================
#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected"
#endif
ZigbeeAnalog epPm1(12);
ZigbeePM25Sensor epPm2_5(13);
ZigbeeAnalog epPm4(14);
ZigbeeAnalog epPm10(15);
ZigbeeTempSensor epTempRh(11);
ZigbeeAnalog epVocI(16);
ZigbeeAnalog epNoxI(17);
ZigbeeCarbonDioxideSensor epCo2(18);

// ====== Seeed XIAO ESP32-C6 Pins ========
constexpr int SDA_PIN = 22; // D4
constexpr int SCL_PIN = 23; // D5
constexpr int BOARD_LED_PIN = 15;

// ====== SEN66 (I2C) =====================
constexpr uint8_t SEN66_ADDR = 0x6B;
constexpr int I2C_SPEED = 100000; // 100 kHz, 100 kbit/s (standard mode)

// CRC-8 (poly 0x31), init 0xFF — verification every 2 data bytes
static uint8_t crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; ++b)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

static bool sendCmd(uint16_t cmd)
{
    Wire.beginTransmission(SEN66_ADDR);
    Wire.write(uint8_t(cmd >> 8));
    Wire.write(uint8_t(cmd & 0xFF));
    return Wire.endTransmission() == 0;
}

// Send command, wait, read n bytes
static bool readCmd(uint16_t cmd, uint8_t n, uint8_t *buf, uint16_t wait_ms = 25)
{
    if (!sendCmd(cmd))
        return false;
    delay(wait_ms);
    uint8_t got = Wire.requestFrom(SEN66_ADDR, n);
    if (got != n)
        return false;
    for (uint8_t i = 0; i < n; ++i)
        buf[i] = Wire.read();
    return true;
}

// Read one 16-bit + CRC from rx buffer at offset i
static bool readU16Crc(const uint8_t *rx, int i, uint16_t &out)
{
    if (crc8(&rx[i], 2) != rx[i + 2])
        return false;
    out = (uint16_t(rx[i]) << 8) | rx[i + 1];
    return true;
}

struct Sen66Data
{
    float pm1;     // µg/m³
    float pm2p5;   // µg/m³
    float pm4;     // µg/m³
    float pm10;    // µg/m³
    float rh;      // %
    float temp;    // °C
    uint16_t vocI; // index
    uint16_t noxI; // index
    uint16_t co2;  // ppm
};

static bool sen66ReadOnce(Sen66Data &d)
{
    // Data ready? → 3 bytes (pad, ready, crc)
    uint8_t dr[3];
    if (!readCmd(0x0202, 3, dr))
        return false;
    if (crc8(dr, 2) != dr[2])
        return false;
    if (dr[1] != 0x01)
        return false; // not ready

    // Read measured values -> 27 bytes (9 x (MSB LSB CRC))
    uint8_t rx[27];
    if (!readCmd(0x0300, 27, rx))
        return false;

    uint16_t u;
    if (!readU16Crc(rx, 0, u))
        return false;
    d.pm1 = u / 10.0f;
    if (!readU16Crc(rx, 3, u))
        return false;
    d.pm2p5 = u / 10.0f;
    if (!readU16Crc(rx, 6, u))
        return false;
    d.pm4 = u / 10.0f;
    if (!readU16Crc(rx, 9, u))
        return false;
    d.pm10 = u / 10.0f;
    if (!readU16Crc(rx, 12, u))
        return false;
    d.rh = u / 100.0f;
    if (!readU16Crc(rx, 15, u))
        return false;
    d.temp = u / 200.0f;
    if (!readU16Crc(rx, 18, u))
        return false;
    d.vocI = u / 10.0f;
    if (!readU16Crc(rx, 21, u))
        return false;
    d.noxI = u / 10.0f;
    if (!readU16Crc(rx, 24, u))
        return false;
    d.co2 = u;

    return true;
}

// ====== Arduino code =====================

void setup()
{
    // TODO: exclude serial when not debugging

    Serial.begin(115200);
    delay(3000); // wait for serial monitor

    epTempRh.setManufacturerAndModel("Custom", "SEN66+ESP32-C6"); // common for device
    epTempRh.setVersion(1);

    auto addAnalogInputEndpoint = [](ZigbeeAnalog &ep, const char *name, float res)
    {
        ep.addAnalogInput();
        ep.setAnalogInputDescription(name);
        ep.setAnalogInputResolution(res);
        // ep.setAnalogInputApplication
        // ep.setAnalogInputMinMax
        // ep.setEpConfig();
        Zigbee.addEndpoint(&ep);
    };
    addAnalogInputEndpoint(epPm1, "PM1", 0.1);

    Zigbee.addEndpoint(&epPm2_5);

    addAnalogInputEndpoint(epPm4, "PM4", 0.1);
    addAnalogInputEndpoint(epPm10, "PM10", 0.1);

    epTempRh.addHumiditySensor(0, 100, 0.1);
    Zigbee.addEndpoint(&epTempRh);

    addAnalogInputEndpoint(epNoxI, "NOx_Index", 1);
    addAnalogInputEndpoint(epVocI, "VOC_Index", 1);

    Zigbee.addEndpoint(&epCo2);

    Serial.println("Starting Zigbee...");
    if (!Zigbee.begin())
    {
        Serial.println("Zigbee start fail");
        Serial.println("Rebooting...");
        ESP.restart();
    }

    Serial.println("Connecting to Zigbee network (permit join if not added)...");
    while (!Zigbee.connected())
    {
        Serial.print(".");
        delay(1000);
    }
    Serial.println("Connected to Zigbee network");

    uint16_t min_rep_int = 60;
    uint16_t max_rep_int = 60 * 5;
    // Reporting must be called after Zigbee.begin
    epPm1.setAnalogInputReporting(min_rep_int, max_rep_int, 0.1);
    epPm2_5.setReporting(min_rep_int, max_rep_int, 0.1);
    epPm4.setAnalogInputReporting(min_rep_int, max_rep_int, 0.1);
    epPm10.setAnalogInputReporting(min_rep_int, max_rep_int, 0.1);
    epTempRh.setHumidityReporting(min_rep_int, max_rep_int, 0.1);
    epTempRh.setReporting(min_rep_int, max_rep_int, 0.1);
    epVocI.setAnalogInputReporting(min_rep_int, max_rep_int, 1);
    epNoxI.setAnalogInputReporting(min_rep_int, max_rep_int, 1);
    epCo2.setReporting(min_rep_int, max_rep_int, 1);

    Serial.println("\nInitialized Zigbee");

    pinMode(BOARD_LED_PIN, OUTPUT);
    digitalWrite(BOARD_LED_PIN, HIGH); // turn off
    Serial.println("Initialized LED");

    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(I2C_SPEED);
    Serial.println("Initialized I2C");

    Serial.println("Starting SEN66 measurements...");
    // TODO: commands as variables
    if (!sendCmd(0x0021)) // Start continuous measurement
    {
        Serial.println("Start measurement FAIL");
        return;
    }

    Serial.println("Waiting for measurement start...");
    delay(1200);

    // TODO: exit if read successful 1-3 times is true (don't send, treat as a warmup, due to initial measurement value spikes?)
}

unsigned long previousMillis = 0;
const long interval_ms = 5 * 1000;
void loop()
{
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis < interval_ms) // non blocking delay
    {
        return;
    }
    previousMillis = currentMillis;

    // TODO: return if not in measurement mode?

    Sen66Data d{};
    if (!sen66ReadOnce(d))
    {
        Serial.println("SEN66 read: not ready or error");
        return;
    }

    // TODO: add zigbee led on/off control
    digitalWrite(BOARD_LED_PIN, LOW);
    delay(30);
    digitalWrite(BOARD_LED_PIN, HIGH);

    Serial.println("\nSEN66 values:");
    Serial.printf("PM1    : %.1f ug/m3\n", d.pm1);
    Serial.printf("PM2.5  : %.1f ug/m3\n", d.pm2p5);
    Serial.printf("PM4    : %.1f ug/m3\n", d.pm4);
    Serial.printf("PM10   : %.1f ug/m3\n", d.pm10);
    Serial.printf("RH     : %.2f %%\n", d.rh);
    Serial.printf("Temp.  : %.2f C\n", d.temp);
    Serial.printf("VOC I. : %u\n", d.vocI);
    Serial.printf("NOx I. : %u\n", d.noxI);
    Serial.printf("CO2    : %u ppm\n", d.co2);

    epPm1.setAnalogInput(d.pm1);
    epPm2_5.setPM25(d.pm2p5);
    epPm4.setAnalogInput(d.pm4);
    epPm10.setAnalogInput(d.pm10);
    epTempRh.setHumidity(d.rh);
    epTempRh.setTemperature(d.temp);
    epVocI.setAnalogInput(d.vocI);
    epNoxI.setAnalogInput(d.noxI);
    epCo2.setCarbonDioxide(d.co2);

    // TODO: reduce power usage? (example "Zigbee_Temp_Hum_Sensor_Sleepy") with "Stop Measurement SEN6x" command?
    // TODO: "Start Fan Cleaning SEN6x" (command) every week?
}

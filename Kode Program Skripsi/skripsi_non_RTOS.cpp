/*
  All-in-one (updated CSV header & filename):
  - DHT22
  - Tipping-bucket rain (interrupt)
  - Rain analog (AO_PIN GPIO36)
  - Wind direction (Serial2 RX=33/TX=32)
  - Wind speed (RS485 UART1 RX=16/TX=17, DE/RE pin 4)
  - NTP (GMT+7)
  - FreeRTOS tasks: sensor, rain analog, wind dir, wind RS485, aggregator, sd writer
  - Writes CSV to SD card every kelipatan 3 menit
  - CSV file: /cuaca_dataset.csv
  - CSV header (order): time,suhu,humidity,kec. angin,arah angin,curah hujan,raindrop
*/
 
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>    // ADDED: MQTT client
#include <ArduinoJson.h>     // ADDED: JSON builder
#include "time.h"
#include "esp_sntp.h"
 
#include "DHT.h"
#include <SPI.h>
#include <SD.h>
#include <HardwareSerial.h>
#include <sys/time.h>
 
// ------------------ Konfigurasi ------------------
/* WiFi */
const char *ssid = "your_wifi";
const char *password = "your_password";

 
/* NTP */
const char *ntpServer1 = "pool.ntp.org";
const char *ntpServer2 = "time.nist.gov";
const long gmtOffset_sec = 7 * 3600;   // GMT+7 (WIB)
const int daylightOffset_sec = 0;      // no DST for WIB
 
/* Sensor DHT */
#define DHTPIN 26
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);
 
/* Rain tipping-bucket */
const int rainSensorPin = 34;  
volatile unsigned long lastInterruptTime = 0;
volatile unsigned long tipCount = 0;    // total tip sejak boot / reset
const float mmPerTip = 0.7;            // kalibrasi: 1 tip = 0.7 mm
const unsigned long DEBOUNCE_MS = 500UL;
 
/* Rain analog sensor (AO) */
#define AO_PIN   39// ADC1_CH0 (GPIO36)
 
/* Wind Direction (Serial2) */
#define RX2_PIN 33 // pin RX2
#define TX2_PIN 32 // pin TX2
 
/* Wind Speed RS485 (UART1) */
#define RX_RS485 16
#define TX_RS485 17
//#define DERE_pin 4    // RS485 Direction control pin (DE/RE)
HardwareSerial RS485Serial(1); // UART1
 
// RS485/Anemometer registers
#define DEFAULT_DEVICE_ADDRESS  0x02
#define WIND_SPEED_REG_ADDR 0x002A
#define READ_HOLDING_REG  0x03
#define WRITE_SINGLE_REG  0x06
 
/* SD Card */
#define SD_CS 5     
const char *logFilename = "/cuaca-dataset-tanpa-rtos.csv"; 
 
/* Timing variables for non-RTOS */
unsigned long lastSensorRead = 0;
unsigned long lastRainAnalogRead = 0;
unsigned long lastWindDirRead = 0;
unsigned long lastWindSpeedRead = 0;
unsigned long lastDataSave = 0;
unsigned long lastMqttPublish = 0;

// Intervals in milliseconds
#define SENSOR_INTERVAL 10000      // 10 seconds
#define RAIN_STATUS_INTERVAL 2000  // 2 seconds
#define WIND_DIR_INTERVAL 15000    // 15 seconds
#define WIND_SPEED_INTERVAL 5000   // 5 seconds
#define DATA_SAVE_INTERVAL 180000  // 3 minutes (180 seconds)
#define MQTT_PUBLISH_INTERVAL 180000 // 3 minutes
 
// latest reading (updated oleh taskSensor & taskRainAnalog)
struct {
  float temperature;
  float humidity;
  time_t timestamp;
  int rain_status;
} latestReading = {NAN, NAN, 0, -1};

// Variables to reduce repetitive serial output
float lastPrintedTemp = NAN;
float lastPrintedHum = NAN;
int lastPrintedRainAnalog = -999;
float lastPrintedWindSpeed = NAN;
unsigned long lastSensorPrintTime = 0;
unsigned long lastRainAnalogPrintTime = 0;
unsigned long lastWindSpeedPrintTime = 0;
#define SENSOR_PRINT_INTERVAL 10000      // Print sensor data every 10 seconds
#define RAIN_STATUS_PRINT_INTERVAL 2000  // Print rain analog every 2 seconds  
#define WIND_SPEED_PRINT_INTERVAL 5000   // Print wind speed every 5 seconds
 
// latest wind (updated oleh taskWindDir & taskWindRS485)
struct {
  uint8_t code;
  char dir[16];
  time_t timestamp;
  float speed; // m/s
  time_t speed_ts;
} latestWind = {0, "", 0, NAN, 0};
 
 
// ------------------ MQTT (ADDED) ------------------
const char* mqttServer = "broker.hivemq.com"; 
const int mqttPort = 1883;
const char* mqttUser = "";
const char* mqttPassword = "";
const char* mqttTopic = "sensor/cuaca"; // topic publish

WiFiClient espClient;              
PubSubClient mqttClient(espClient);

void reconnectMQTT() {
  if (mqttClient.connected()) return;
  unsigned long start = millis();
  while (!mqttClient.connected()) {
    Serial.print("[MQTT] Connecting...");
    if (strlen(mqttUser) == 0) {
      if (mqttClient.connect("ESP32_Cuaca")) {
        Serial.println(" CONNECTED");
        break;
      }
    } else {
      if (mqttClient.connect("ESP32_Cuaca", mqttUser, mqttPassword)) {
        Serial.println(" CONNECTED");
        break;
      }
    }
    Serial.printf(" failed rc=%d. retry in 3s\n", mqttClient.state());
    delay(3000);
    if (millis() - start > 30000) {
      Serial.println("[MQTT] giving up for now (will retry later)");
      break;
    }
  }
}

// ------------------ CRC16 fungsi ------------------
unsigned int calculateCRC(unsigned char * frame, unsigned char bufferSize)
{
  unsigned int temp, temp2, flag;
  temp = 0xFFFF;
  for (unsigned char i = 0; i < bufferSize; i++)
  {
    temp = temp ^ frame[i];
    for (unsigned char j = 1; j <= 8; j++)
    {
      flag = temp & 0x0001;
      temp >>= 1;
      if (flag)
        temp ^= 0xA001;
    }
  }
  // Reverse byte order.
  temp2 = temp >> 8;
  temp = (temp << 8) | temp2;
  temp &= 0xFFFF;
  return temp;
}
 
// ------------------ RS485: getWindSpeed ------------------
float getWindSpeed(byte address){
  float windSpeed = NAN;
  byte Anemometer_buf[8];
  byte Anemometer_request[] = {address, READ_HOLDING_REG, 0x00, (byte)WIND_SPEED_REG_ADDR, 0x00, 0x01, 0x00, 0x00}; // Request frame
 
  unsigned int crc16 = calculateCRC(Anemometer_request, sizeof(Anemometer_request) - 2);
  Anemometer_request[sizeof(Anemometer_request) - 2] = crc16 >> 8; // crc hi
  Anemometer_request[sizeof(Anemometer_request) - 1] = crc16 & 0xFF; // crc lo
 
  //digitalWrite(DERE_pin, HIGH);     
  RS485Serial.write(Anemometer_request, sizeof(Anemometer_request));
  RS485Serial.flush();
  //digitalWrite(DERE_pin, LOW);     
 
  unsigned long start = millis();
  size_t idx = 0;
  while (idx < 7 && (millis() - start) < 200) {
    if (RS485Serial.available()) {
      Anemometer_buf[idx++] = RS485Serial.read();
    }
  }

  if (idx >= 5) {
    byte dataH = Anemometer_buf[3];
    byte dataL = Anemometer_buf[4];
    windSpeed = ((dataH << 8) | dataL ) / 100.0;  // raw m/s
  } else {
    windSpeed = NAN;
  }
  return windSpeed;
}

// ------------------ NTP Callback ------------------
void timeavailable(struct timeval *t) {
  Serial.println("Got time adjustment from NTP!");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.printf("Local time: %s\n", buf);
  }
}
 
// ------------------ Helper: format time ------------------
bool getISOTimeStrFromSec(time_t sec, char *outBuf, size_t len) {
  struct tm timeinfo;
  if (localtime_r(&sec, &timeinfo) == NULL) return false;
  if (strftime(outBuf, len, "%Y-%m-%d %H:%M:%S", &timeinfo) == 0) return false;
  return true;
}
 
// ------------------ ISR for tipping bucket ------------------
void IRAM_ATTR ISR_countTip() {
  unsigned long now = millis();
  if (now - lastInterruptTime >= DEBOUNCE_MS) {
    tipCount++;
    lastInterruptTime = now;
  }
}
 
// ------------------ Function: Read Sensor (DHT) ------------------
void readSensorData() {
  unsigned long lastSensorMillisPrinted = 0;
  float h = dht.readHumidity() - 13.46;
  float t = dht.readTemperature() + 0.6;
 
  if (isnan(h) || isnan(t)) {
    Serial.println("[Sensor] Gagal membaca DHT22");
  } else {
    time_t now;
    time(&now);
 
    latestReading.temperature = t;
    latestReading.humidity = h;
    latestReading.timestamp = now;
 
    // Only print if values changed significantly or at regular intervals
    unsigned long currentTime = millis();
    bool tempChanged = isnan(lastPrintedTemp) || abs(t - lastPrintedTemp) > 0.5;
    bool humChanged = isnan(lastPrintedHum) || abs(h - lastPrintedHum) > 2.0;
    bool timeToprint = (currentTime - lastSensorPrintTime >= SENSOR_PRINT_INTERVAL);
    
    if (tempChanged || humChanged || timeToprint) {
      char timestr[32];
      getISOTimeStrFromSec(now, timestr, sizeof(timestr));
      Serial.printf("[Sensor] %s | ms=%lu | Temp: %.2f C, Hum: %.2f %%\n",
              timestr,
              millis(),
              t, h);
      lastPrintedTemp = t;
      lastPrintedHum = h;
      lastSensorPrintTime = currentTime;
    }
  }
}
 
// ------------------ Function: Read Rain Analog (AO_PIN) ------------------
void readRainAnalog() {
  int raw = analogRead(AO_PIN);

  int hujan = (raw < 3000) ? 1 : 0;

  latestReading.rain_status = hujan;   // 1 = hujan, 0 = tidak
  latestReading.timestamp = time(nullptr);

  unsigned long currentTime = millis();
  bool valueChanged = (hujan != lastPrintedRainAnalog);
  bool timeToprint = (currentTime - lastRainAnalogPrintTime >= RAIN_STATUS_PRINT_INTERVAL);

  if (valueChanged || timeToprint) {
    Serial.printf("[RainAnalog] ms=%lu | raw=%d -> hujan=%d\n",
              millis(), raw, hujan);
    lastPrintedRainAnalog = hujan;
    lastRainAnalogPrintTime = currentTime;
  }
}

 
// ------------------ Function: Read Wind Direction (Serial2) ------------------
String windDataBuf = "";
uint8_t lastPrintedWindCode = 255; // Initialize with invalid value
unsigned long lastWindPrintTime = 0;
#define WIND_PRINT_INTERVAL 15000 // Print wind direction every 15 seconds

void readWindDirection() {
  String s_angin;
  char bufDir[16];
  uint8_t code = 0;
  
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    windDataBuf += c;
    int a = windDataBuf.indexOf('*');
    int b = windDataBuf.indexOf('#');
    if (a >= 0 && b > a) {
      s_angin = windDataBuf.substring(a + 1, b);
      windDataBuf = windDataBuf.substring(b + 1);
 
      code = 0;
      strncpy(bufDir, "unknown", sizeof(bufDir));
      bufDir[sizeof(bufDir)-1] = '\0';
 
      if (s_angin.equals("1")) { code = 1; strncpy(bufDir, "utara", sizeof(bufDir)); }
      else if (s_angin.equals("2")) { code = 2; strncpy(bufDir, "timur laut", sizeof(bufDir)); }
      else if (s_angin.equals("3")) { code = 3; strncpy(bufDir, "timur", sizeof(bufDir)); }
      else if (s_angin.equals("4")) { code = 4; strncpy(bufDir, "tenggara", sizeof(bufDir)); }
      else if (s_angin.equals("5")) { code = 5; strncpy(bufDir, "selatan", sizeof(bufDir)); }
      else if (s_angin.equals("6")) { code = 6; strncpy(bufDir, "barat daya", sizeof(bufDir)); }
      else if (s_angin.equals("7")) { code = 7; strncpy(bufDir, "barat", sizeof(bufDir)); }
      else if (s_angin.equals("8")) { code = 8; strncpy(bufDir, "barat laut", sizeof(bufDir)); }
 
      latestWind.code = code;
      strncpy(latestWind.dir, bufDir, sizeof(latestWind.dir));
      latestWind.dir[sizeof(latestWind.dir)-1] = '\0';
      time(&latestWind.timestamp);
      
      // Only print if wind direction changed or at regular intervals
      unsigned long currentTime = millis();
      if (code != lastPrintedWindCode || (currentTime - lastWindPrintTime >= WIND_PRINT_INTERVAL)) {
        Serial.printf("[WindDir] ms=%lu | kode=%u, arah=%s\n",
              millis(), code, bufDir);

        lastPrintedWindCode = code;
        lastWindPrintTime = currentTime;
      }
    }
  }
}
 
// ------------------ Function: Read Wind Speed (RS485) ------------------
void readWindSpeed() {
  float spd = getWindSpeed(DEFAULT_DEVICE_ADDRESS);
  if (!isnan(spd)) {
    latestWind.speed = spd;
    latestWind.speed_ts = time(nullptr);
    
    // Only print if value changed significantly or at regular intervals
    unsigned long currentTime = millis();
    bool valueChanged = isnan(lastPrintedWindSpeed) || abs(spd - lastPrintedWindSpeed) > 0.1;
    bool timeToprint = (currentTime - lastWindSpeedPrintTime >= WIND_SPEED_PRINT_INTERVAL);
    
    if (valueChanged || timeToprint) {
      Serial.printf("[WindRS485] ms=%lu | speed=%.2f m/s\n",
              millis(), spd);
      lastPrintedWindSpeed = spd;
      lastWindSpeedPrintTime = currentTime;
    }
  } else {
    // Only print NAN message occasionally
    unsigned long currentTime = millis();
    if (currentTime - lastWindSpeedPrintTime >= WIND_SPEED_PRINT_INTERVAL) {
      Serial.println("[WindRS485] gagal baca (NAN)");
      lastWindSpeedPrintTime = currentTime;
    }
  }
}
 
// ------------------ Function: Save and Publish Data ------------------
unsigned long lastSavedTotalTip = 0;

void saveAndPublishData() {
  noInterrupts();
  unsigned long snapshotTip = tipCount;
  interrupts();
 
  // Get latest reading & wind data
  float t = latestReading.temperature;
  float hhum = latestReading.humidity;
  time_t ts = latestReading.timestamp;
  int rain_status = latestReading.rain_status;
  uint8_t wcode = latestWind.code;
  char wdir[16];
  strncpy(wdir, latestWind.dir, sizeof(wdir));
  wdir[sizeof(wdir)-1] = '\0';
  float wspeed = latestWind.speed;
 
  unsigned long deltaTip = 0;
  if (snapshotTip >= lastSavedTotalTip) deltaTip = snapshotTip - lastSavedTotalTip;
  else deltaTip = snapshotTip;
 
  float rain_mm_interval = deltaTip * mmPerTip;
  float rain_mm_total = snapshotTip * mmPerTip;
 
  struct timeval tv;
  gettimeofday(&tv, NULL);
  time_t currentTime = (time_t)tv.tv_sec;
 
  // Prepare data for SD card
  char timestr[64];
  if (!getISOTimeStrFromSec(currentTime, timestr, sizeof(timestr))) {
    strcpy(timestr, "1970-01-01 00:00:00");
  }
 
  // Write to SD card
  String line = String(timestr) + "," +
                String(t, 2) + "," +
                String(hhum, 2) + "," +
                String(wspeed, 2) + "," +
                String(wdir) + "," +
                String(rain_mm_interval, 2) + "," +
                String(rain_status);
 
  File f = SD.open(logFilename, FILE_APPEND);
  if (f) {
    f.println(line);
    f.close();
    Serial.printf("[SD] Ditulis: %s\n", line.c_str());
  } else {
    Serial.println("[SD] Gagal buka file untuk append!");
  }
 
  Serial.printf("[Data] Snapshot @%s -> tip_delta=%lu, rain=%.2f mm, rainAO=%d, wind=%u/%s, spd=%.2f\n",
                timestr, deltaTip, rain_mm_interval, rain_status, wcode, wdir, wspeed);
 
  // ------------- MQTT PUBLISH -------------
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  StaticJsonDocument<256> jsonDoc;
  jsonDoc["time"] = timestr;
  if (!isnan(t)) jsonDoc["temperature"] = t;
  else jsonDoc["temperature"] = nullptr;
  if (!isnan(hhum)) jsonDoc["humidity"] = hhum;
  else jsonDoc["humidity"] = nullptr;
  if (!isnan(wspeed)) jsonDoc["wind_speed"] = wspeed;
  else jsonDoc["wind_speed"] = nullptr;
  jsonDoc["wind_dir"] = wcode;
  jsonDoc["rain_mm"] = rain_mm_interval;
  jsonDoc["rain_status"] = rain_status;

  char payload[256];
  size_t n = serializeJson(jsonDoc, payload, sizeof(payload));
  if (n > 0 && mqttClient.connected()) {
    bool ok = mqttClient.publish(mqttTopic, payload);
    Serial.printf("[MQTT] Publish %s -> %s (ok=%d)\n", mqttTopic, payload, ok ? 1 : 0);
  } else {
    Serial.println("[MQTT] Payload empty or MQTT not connected, skip publish.");
  }
 
  lastSavedTotalTip = snapshotTip;
}
 
// ------------------ Function: Initialize SD Card Header ------------------
void initializeSDCard() {
  if (!SD.exists(logFilename)) {
    File f = SD.open(logFilename, FILE_WRITE);
    if (f) {
      f.println("time,suhu,humidity,kec. angin,arah angin,curah hujan,raindrop");
      f.close();
      Serial.println("[SD] Header file dibuat: cuaca_dataset.csv");
    } else {
      Serial.println("[SD] Gagal membuat file header.");
    }
  }
}
 
// ------------------ setup() ------------------
void setup() {
  Serial.begin(9600);
  delay(100);
 
  // DHT
  dht.begin();
  Serial.println("DHT22 ready.");
  // ADC attenuation untuk AO_PIN
  analogSetPinAttenuation(AO_PIN, ADC_11db);
  Serial.println("ADC AO_PIN attenuation set to 11db.");
 
  // Serial2 untuk arah angin
  Serial2.begin(9600, SERIAL_8N1, RX2_PIN, TX2_PIN);
  Serial.println("Serial2 (wind dir) started.");
 
  // RS485 UART1
  RS485Serial.begin(9600, SERIAL_8N1, RX_RS485, TX_RS485);
  Serial.println("RS485 (anemometer) started.");
 
  // SD card
  if (!SD.begin(SD_CS)) {
    Serial.println("Gagal inisialisasi SD card! Periksa wiring & tegangan (3.3V).");
  } else {
    Serial.println("SD card siap.");
    initializeSDCard(); // Create header if needed
  }
 
  // WiFi + NTP
  Serial.printf("Connecting to %s ", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  unsigned long startWiFi = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - startWiFi > 20000) {
      Serial.println("\n[WiFi] Timeout connecting.");
      break;
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" CONNECTED");
  } else {
    Serial.println(" Not connected, continuing without WiFi.");
  }

  // MQTT setup (ADDED)
  mqttClient.setServer(mqttServer, mqttPort);
  reconnectMQTT(); // attempt immediate connect (non-blocking if fails; will retry later on publish)

  sntp_set_time_sync_notification_cb(timeavailable);
  esp_sntp_servermode_dhcp(1);
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
 
  // Tunggu SNTP sinkron (15s timeout)
  unsigned long tstart = millis();
  Serial.println("Menunggu SNTP sinkron...");
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - tstart < 15000) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
    struct timeval tv; gettimeofday(&tv, NULL);
    char buf[64]; getISOTimeStrFromSec(tv.tv_sec, buf, sizeof(buf));
    Serial.printf("SNTP sinkron: %s (.%06ld)\n", buf, (long)tv.tv_usec);
  } else {
    Serial.println("SNTP belum sinkron (timeout). Waktu mungkin belum akurat.");
  }
 
  // Interrupt rain sensor
  pinMode(rainSensorPin, INPUT);
  attachInterrupt(digitalPinToInterrupt(rainSensorPin), ISR_countTip, RISING);
 
  // Initialize timing variables
  lastSensorRead = millis();
  lastRainAnalogRead = millis();
  lastWindDirRead = millis();
  lastWindSpeedRead = millis();
  lastDataSave = millis();
  lastMqttPublish = millis();
 
  Serial.println("Setup selesai. Non-RTOS loop dimulai.");
}
 
// ------------------ loop() ------------------
void loop() {
  unsigned long currentTime = millis();
  
  // Read sensor data (DHT22) every 10 seconds
  if (currentTime - lastSensorRead >= SENSOR_INTERVAL) {
    readSensorData();
    lastSensorRead = currentTime;
  }
  
  // Read rain analog sensor every 2 seconds
  if (currentTime - lastRainAnalogRead >= RAIN_STATUS_INTERVAL) {
    readRainAnalog();
    lastRainAnalogRead = currentTime;
  }
  
  // Read wind direction every 15 seconds
  if (currentTime - lastWindDirRead >= WIND_DIR_INTERVAL) {
    readWindDirection();
    lastWindDirRead = currentTime;
  }
  
  // Read wind speed every 5 seconds
  if (currentTime - lastWindSpeedRead >= WIND_SPEED_INTERVAL) {
    readWindSpeed();
    lastWindSpeedRead = currentTime;
  }
  
  // Save data and publish via MQTT every 3 minutes
  if (currentTime - lastDataSave >= DATA_SAVE_INTERVAL) {
    saveAndPublishData();
    lastDataSave = currentTime;
  }
  
  // Keep MQTT client alive
  if (!mqttClient.connected()) {
    reconnectMQTT();
  } else {
    mqttClient.loop();
  }
  
  // Small delay to prevent overwhelming the processor
  delay(100);
}
 
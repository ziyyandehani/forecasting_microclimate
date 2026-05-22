/*
  - DHT22
  - Tipping-bucket rain (interrupt)
  - Rain analog (AO_PIN GPIO39)
  - Wind direction (Serial2 RX=33/TX=32)
  - Wind speed (RS485 UART1 RX=16/TX=17)
  - NTP (GMT+7)
  - FreeRTOS tasks: sensor, rain analog, wind dir, wind RS485, aggregator, sd writer
  - Writes CSV to SD card setiap kelipatan 3 menit
  - CSV file: /cuaca-dataset.csv
  - CSV header (order): time,suhu,humidity,kec. angin,arah angin,curah hujan,raindrop
*/

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "time.h"
#include "esp_sntp.h"

#include "DHT.h"
#include <SPI.h>
#include <SD.h>
#include <HardwareSerial.h>
#include <sys/time.h>

/* WiFi */
const char *ssid = "ROBOTIIK";
const char *password = "81895656";

/* NTP */
const char *ntpServer1 = "pool.ntp.org";
const char *ntpServer2 = "time.nist.gov";
const long gmtOffset_sec = 7 * 3600;
const int daylightOffset_sec = 0;

/* Sensor DHT */
#define DHTPIN 26
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);
// Koefisien hasil regresi 
const float HUM_A = 0.3587537;     // slope
const float HUM_B = 43.1950459;    // intercept

const float TMP_A = 0.4540112;     // slope
const float TMP_B = 13.6536084;    // intercept


/* Rain tipping-bucket */
const int rainSensorPin = 34;
volatile unsigned long lastInterruptTime = 0;
volatile unsigned long tipCount = 0;
const float mmPerTip = 0.7;
const unsigned long DEBOUNCE_MS = 500UL;

/* Rain analog sensor (AO) */
#define AO_PIN 39

/* Wind Direction (Serial2) */
#define RX2_PIN 33
#define TX2_PIN 32

/* Wind Speed RS485 (UART1) */
#define RX_RS485 16
#define TX_RS485 17
HardwareSerial RS485Serial(1);

/* RS485/Anemometer registers */
#define DEFAULT_DEVICE_ADDRESS  0x02
#define WIND_SPEED_REG_ADDR 0x002A
#define READ_HOLDING_REG  0x03

/* SD Card */
#define SD_CS 5
const char *logFilename = "/cuaca-dataset.csv";

/* RTOS */
#define QUEUE_LENGTH 10

typedef struct {
  float temperature;
  float humidity;
  time_t timestamp;
  unsigned long tip_delta;
  float rain_mm_interval;
  float rain_mm_total;
  int rain_status;
  uint8_t wind_code;
  char wind_dir[16];
  float wind_speed;
  /* Data dari API */
  float api_temperature;
  float api_humidity;
  float api_wind_speed;
  uint8_t api_wind_code;
  char api_wind_dir[16];
  float api_rain_mm;
} sensor_msg_t;

QueueHandle_t sensorQueue;
SemaphoreHandle_t sdMutex;
SemaphoreHandle_t dataMutex;

struct {
  float temperature;
  float humidity;
  time_t timestamp;
  int rain_status;
} latestReading = {NAN, NAN, 0, -1};

struct {
  uint8_t code;
  char dir[16];
  time_t timestamp;
  float speed;
  time_t speed_ts;
} latestWind = {0, "", 0, NAN, 0};

struct {
  float temperature;
  float humidity;
  float wind_speed;
  uint8_t wind_code;
  char wind_dir[16];
  float rain_mm;
  time_t timestamp;
} latestAPIReading = {NAN, NAN, NAN, 0, "", 0.0, 0};

/* OpenWeatherMap API */
const char* API_KEY = "689bd19c7d61c8c14869750378aaa033";
const float LAT = -8.036446882144972;
const float LON = 112.73506632854176;
const char* API_URL = "https://api.openweathermap.org/data/2.5/weather";

/* MQTT */
const char* mqttServer = "broker.hivemq.com";
const int mqttPort = 1883;
const char* mqttUser = "";
const char* mqttPassword = "";
const char* mqttTopic = "zizi12345/weather/station01";
float a = 0.86226; 

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

/* CRC16 */
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
  temp2 = temp >> 8;
  temp = (temp << 8) | temp2;
  temp &= 0xFFFF;
  return temp;
}

float calibrate(float raw) {

  if (raw < 1.5) raw = 0.0;

  if (raw < 3.99)
    return 0.688 * raw + 0.598;

  return 0.709 * raw + 0.204;
}


/* RS485: getWindSpeed */
float getWindSpeed(byte address){
  float windSpeed = NAN;
  byte Anemometer_buf[8];
  byte Anemometer_request[] = {address, READ_HOLDING_REG, 0x00, (byte)WIND_SPEED_REG_ADDR, 0x00, 0x01, 0x00, 0x00};

  unsigned int crc16 = calculateCRC(Anemometer_request, sizeof(Anemometer_request) - 2);
  Anemometer_request[sizeof(Anemometer_request) - 2] = crc16 >> 8;
  Anemometer_request[sizeof(Anemometer_request) - 1] = crc16 & 0xFF;

  RS485Serial.write(Anemometer_request, sizeof(Anemometer_request));
  RS485Serial.flush();

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
    windSpeed = ((dataH << 8) | dataL ) / 100.0;
  } else {
    windSpeed = NAN;
  }
  return windSpeed;
}

/* NTP callback */
void timeavailable(struct timeval *t) {
  Serial.println("Got time adjustment from NTP!");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.printf("Local time: %s\n", buf);
  }
}

/* Format time helper */
bool getISOTimeStrFromSec(time_t sec, char *outBuf, size_t len) {
  struct tm timeinfo;
  if (localtime_r(&sec, &timeinfo) == NULL) return false;
  if (strftime(outBuf, len, "%Y-%m-%d %H:%M:%S", &timeinfo) == 0) return false;
  return true;
}

/* ISR for tipping bucket */
void IRAM_ATTR ISR_countTip() {
  unsigned long now = millis();
  if (now - lastInterruptTime >= DEBOUNCE_MS) {
    tipCount++;
    lastInterruptTime = now;
  }
}

/* Fungsi untuk membaca data dari OpenWeatherMap API */
void getWeatherFromAPI() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[API] WiFi tidak terhubung, skip API call");
    return;
  }

  String url = String(API_URL) + "?lat=" + String(LAT, 6) + "&lon=" + String(LON, 6) + 
               "&appid=" + String(API_KEY) + "&units=metric";
  
  Serial.printf("[API] Requesting: %s\n", url.c_str());
  
  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();
  
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[API] HTTP Error: %d\n", httpCode);
    http.end();
    return;
  }
  
  String payload = http.getString();
  http.end();
  
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    Serial.printf("[API] JSON parse error: %s\n", error.c_str());
    return;
  }
  
  if (!doc.containsKey("main") || !doc.containsKey("wind")) {
    Serial.println("[API] Missing main or wind data in response");
    return;
  }
  
  float api_temp = doc["main"]["temp"] | NAN;
  float api_hum = doc["main"]["humidity"] | 0;
  float api_wind_spd = doc["wind"]["speed"] | NAN;
  float api_rain = doc["rain"]["1h"] | 0.0;
  
  uint8_t api_wcode = 0;
  char api_wdir[16] = "unknown";
  
  // Parse wind direction dari main.deg (derajat)
  if (doc.containsKey("wind") && doc["wind"].containsKey("deg")) {
    int deg = doc["wind"]["deg"] | -1;
    if (deg >= 0) {
      // Konversi derajat ke arah 8 mata angin
      if ((deg >= 348.75) || (deg < 11.25)) { api_wcode = 1; strncpy(api_wdir, "utara", sizeof(api_wdir)); }
      else if (deg < 56.25) { api_wcode = 2; strncpy(api_wdir, "timur laut", sizeof(api_wdir)); }
      else if (deg < 101.25) { api_wcode = 3; strncpy(api_wdir, "timur", sizeof(api_wdir)); }
      else if (deg < 146.25) { api_wcode = 4; strncpy(api_wdir, "tenggara", sizeof(api_wdir)); }
      else if (deg < 191.25) { api_wcode = 5; strncpy(api_wdir, "selatan", sizeof(api_wdir)); }
      else if (deg < 236.25) { api_wcode = 6; strncpy(api_wdir, "barat daya", sizeof(api_wdir)); }
      else if (deg < 281.25) { api_wcode = 7; strncpy(api_wdir, "barat", sizeof(api_wdir)); }
      else if (deg < 326.25) { api_wcode = 8; strncpy(api_wdir, "barat laut", sizeof(api_wdir)); }
    }
  }
  api_wdir[sizeof(api_wdir)-1] = '\0';
  
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    latestAPIReading.temperature = api_temp;
    latestAPIReading.humidity = api_hum;
    latestAPIReading.wind_speed = api_wind_spd;
    latestAPIReading.wind_code = api_wcode;
    strncpy(latestAPIReading.wind_dir, api_wdir, sizeof(latestAPIReading.wind_dir));
    latestAPIReading.wind_dir[sizeof(latestAPIReading.wind_dir)-1] = '\0';
    latestAPIReading.rain_mm = api_rain;
    time(&latestAPIReading.timestamp);
    xSemaphoreGive(dataMutex);
  }
  
  Serial.printf("[API] Updated: temp=%.2f, hum=%.0f, wind=%.2f m/s, dir=%s, rain=%.2f\n",
                api_temp, api_hum, api_wind_spd, api_wdir, api_rain);
}

/* Task: Sensor Reader (DHT) */
void taskdht(void *pvParameters) {
  static unsigned long prev = 0;
  const TickType_t delayTicks = pdMS_TO_TICKS(10000);

  for (;;) {
    unsigned long nowMs = millis();
    unsigned long dt = nowMs - prev;
    prev = nowMs;

    float h_raw = dht.readHumidity();
    float t_raw = dht.readTemperature();

    float h = HUM_A * h_raw + HUM_B;
    float t = TMP_A * t_raw + TMP_B;

    if (isnan(h) || isnan(t)) {
      Serial.println("[Sensor] Gagal membaca DHT22");
    } else {
      time_t now;
      time(&now);
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        latestReading.temperature = t;
        latestReading.humidity = h;
        latestReading.timestamp = now;
        xSemaphoreGive(dataMutex);
      }
      char timestr[32];
      getISOTimeStrFromSec(now, timestr, sizeof(timestr));
      Serial.printf("[Sensor] %s - Temp: %.2f C, Hum: %.2f %% (dt=%lu ms)\n",
                    timestr, t, h, dt);
    }
    vTaskDelay(delayTicks);
  }
}

/* Task: Rain Analog Reader (AO_PIN) */
void taskRainAnalog(void *pvParameters) {
  static unsigned long prev = 0;
  const TickType_t delayTicks = pdMS_TO_TICKS(2000);

  for (;;) {
    unsigned long nowMs = millis();
    unsigned long dt = nowMs - prev;
    prev = nowMs;

    int raw = analogRead(AO_PIN);
    int hujan = (raw < 3000) ? 1 : 0;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      latestReading.rain_status = hujan;
      latestReading.timestamp = time(nullptr);
      xSemaphoreGive(dataMutex);
    }

    Serial.printf("[RainAnalog] raw=%d hujan=%d (dt=%lu ms)\n",
                  raw, hujan, dt);

    vTaskDelay(delayTicks);
  }
}

/* Task: Wind Direction Reader (Serial2) */
void taskWindDir(void *pvParameters) {
  String dataBuf;
  String s_angin;
  char bufDir[16];
  uint8_t code = 0;
  static unsigned long prev = 0;
  const TickType_t delayTicks = pdMS_TO_TICKS(15000);

  for (;;) {
    unsigned long nowMs = millis();
    unsigned long dt = nowMs - prev;
    prev = nowMs;

    while (Serial2.available()) {
      char c = (char)Serial2.read();
      dataBuf += c;
      int a = dataBuf.indexOf('*');
      int b = dataBuf.indexOf('#');
      if (a >= 0 && b > a) {
        s_angin = dataBuf.substring(a + 1, b);
        dataBuf = dataBuf.substring(b + 1);

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

        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          latestWind.code = code;
          strncpy(latestWind.dir, bufDir, sizeof(latestWind.dir));
          latestWind.dir[sizeof(latestWind.dir)-1] = '\0';
          time(&latestWind.timestamp);
          xSemaphoreGive(dataMutex);
        }
      }
    }
    Serial.printf("[WindDir] kode=%u, arah=%s (dt=%lu ms)\n", code, bufDir, dt);
    vTaskDelay(delayTicks);
  }
}

/* Task: Wind Speed Reader (RS485) */
void taskWindRS485(void *pvParameters) {
  static unsigned long prev = 0;
  const TickType_t delayTicks = pdMS_TO_TICKS(5000);

  for (;;) {
    unsigned long nowMs = millis();
    unsigned long dt = nowMs - prev;
    prev = nowMs;

    float spd = getWindSpeed(DEFAULT_DEVICE_ADDRESS);
    float v_corr = calibrate(spd);
    if (v_corr < 0.05) v_corr = 0.0;

    if (!isnan(v_corr)) {
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        latestWind.speed = v_corr;
        latestWind.speed_ts = time(nullptr);
        xSemaphoreGive(dataMutex);
      }
      Serial.printf("[WindRS485] speed=%.2f m/s (dt=%lu ms)\n", v_corr, dt);
    } else {
      Serial.printf("[WindRS485] gagal baca (NAN) (dt=%lu ms)\n", dt);
    }

    vTaskDelay(delayTicks);
  }
}

/* Task: API Weather Reader (OpenWeatherMap) */
void taskAPIWeather(void *pvParameters) {
  const TickType_t delayTicks = pdMS_TO_TICKS(900000); // 15 menit
  
  for (;;) {
    Serial.println("[API Task] Calling getWeatherFromAPI()");
    getWeatherFromAPI();
    
    // Pertama kali tunggu 2 menit sebelum call berikutnya
    vTaskDelay(delayTicks);
  }
}

/* Task: Aggregator */
void taskAggregator(void *pvParameters) {
  unsigned long lastSavedTotalTip = 0;
  int lastSavedMinute = -1;
  int lastSavedSecond = -1;

  for (;;) {
    struct tm timeinfo;
    bool hasRealTime = getLocalTime(&timeinfo, 1000);

    int h, m, s;
    if (hasRealTime) {
      h = timeinfo.tm_hour;
      m = timeinfo.tm_min;
      s = timeinfo.tm_sec;
    } else {
      unsigned long totalSeconds = millis() / 1000UL;
      s = totalSeconds % 60;
      unsigned long totalMinutes = totalSeconds / 60UL;
      m = totalMinutes % 60;
      unsigned long totalHours = (totalMinutes / 60UL) % 24UL;
      h = totalHours;
    }

    if ((s == 0) && ((m % 3) == 0) && !(m == lastSavedMinute && s == lastSavedSecond)) {
      noInterrupts();
      unsigned long snapshotTip = tipCount;
      interrupts();

      float t = NAN, hhum = NAN;
      time_t ts = 0;
      int rain_status_val = -1;
      uint8_t wcode = 0;
      char wdir[16] = "";
      float wspeed = NAN;
      
      /* Data dari API */
      float api_t = NAN, api_hhum = NAN;
      float api_wspeed = NAN;
      uint8_t api_wcode = 0;
      char api_wdir[16] = "";
      float api_rain = 0.0;

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        t = latestReading.temperature;
        hhum = latestReading.humidity;
        ts = latestReading.timestamp;
        rain_status_val = latestReading.rain_status;
        wcode = latestWind.code;
        strncpy(wdir, latestWind.dir, sizeof(wdir));
        wdir[sizeof(wdir)-1] = '\0';
        wspeed = latestWind.speed;
        
        /* Copy data API */
        api_t = latestAPIReading.temperature;
        api_hhum = latestAPIReading.humidity;
        api_wspeed = latestAPIReading.wind_speed;
        api_wcode = latestAPIReading.wind_code;
        strncpy(api_wdir, latestAPIReading.wind_dir, sizeof(api_wdir));
        api_wdir[sizeof(api_wdir)-1] = '\0';
        api_rain = latestAPIReading.rain_mm;
        
        xSemaphoreGive(dataMutex);
      }

      unsigned long deltaTip = 0;
      if (snapshotTip >= lastSavedTotalTip) deltaTip = snapshotTip - lastSavedTotalTip;
      else deltaTip = snapshotTip;

      float rain_mm_interval = deltaTip * mmPerTip;
      float rain_mm_total = snapshotTip * mmPerTip;

      struct timeval tv;
      gettimeofday(&tv, NULL);

      sensor_msg_t msg;
      msg.temperature = t;
      msg.humidity = hhum;
      msg.timestamp = (time_t)tv.tv_sec;
      msg.tip_delta = deltaTip;
      msg.rain_mm_interval = rain_mm_interval;
      msg.rain_mm_total = rain_mm_total;
      msg.rain_status = rain_status_val;
      msg.wind_code = wcode;
      strncpy(msg.wind_dir, wdir, sizeof(msg.wind_dir));
      msg.wind_dir[sizeof(msg.wind_dir)-1] = '\0';
      msg.wind_speed = wspeed;
      
      /* API data */
      msg.api_temperature = api_t;
      msg.api_humidity = api_hhum;
      msg.api_wind_speed = api_wspeed;
      msg.api_wind_code = api_wcode;
      strncpy(msg.api_wind_dir, api_wdir, sizeof(msg.api_wind_dir));
      msg.api_wind_dir[sizeof(msg.api_wind_dir)-1] = '\0';
      msg.api_rain_mm = api_rain;

      if (xQueueSend(sensorQueue, &msg, pdMS_TO_TICKS(1000)) != pdPASS) {
        Serial.println("[Aggregator] Queue penuh, gagal kirim snapshot.");
      } else {
        char timestr[64];
        getISOTimeStrFromSec(msg.timestamp, timestr, sizeof(timestr));
        Serial.printf("[Aggregator] Snapshot @%s -> tip_delta=%lu, rain=%.2f mm, rainAO=%d, wind=%u/%s, spd=%.2f\n",
                      timestr, deltaTip, rain_mm_interval, msg.rain_status, msg.wind_code, msg.wind_dir, msg.wind_speed);
        Serial.printf("            -> API: temp=%.2f, hum=%.0f, wind=%.2f, rain=%.2f\n",
                      msg.api_temperature, msg.api_humidity, msg.api_wind_speed, msg.api_rain_mm);
      }

      /* MQTT publish */
      if (!mqttClient.connected()) {
        reconnectMQTT();
      }
      mqttClient.loop();

      char timestr2[64];
      if (!getISOTimeStrFromSec(msg.timestamp, timestr2, sizeof(timestr2))) {
        strcpy(timestr2, "1970-01-01 00:00:00");
      }

      StaticJsonDocument<512> jsonDoc;
      jsonDoc["time"] = timestr2;
      if (!isnan(msg.temperature)) jsonDoc["temperature"] = msg.temperature;
      else jsonDoc["temperature"] = nullptr;
      if (!isnan(msg.humidity)) jsonDoc["humidity"] = msg.humidity;
      else jsonDoc["humidity"] = nullptr;
      if (!isnan(msg.wind_speed)) jsonDoc["wind_speed"] = msg.wind_speed;
      else jsonDoc["wind_speed"] = nullptr;
      jsonDoc["wind_dir"] = msg.wind_code;
      jsonDoc["rain_mm"] = msg.rain_mm_interval;
      jsonDoc["rain_status"] = msg.rain_status;
      
      /* API fields */
      if (!isnan(msg.api_temperature)) jsonDoc["api_temperature"] = msg.api_temperature;
      else jsonDoc["api_temperature"] = nullptr;
      if (!isnan(msg.api_humidity)) jsonDoc["api_humidity"] = msg.api_humidity;
      else jsonDoc["api_humidity"] = nullptr;
      if (!isnan(msg.api_wind_speed)) jsonDoc["api_wind_speed"] = msg.api_wind_speed;
      else jsonDoc["api_wind_speed"] = nullptr;
      jsonDoc["api_wind_dir"] = msg.api_wind_code;
      jsonDoc["api_rain_mm"] = msg.api_rain_mm;

      char payload[512];
      size_t n = serializeJson(jsonDoc, payload, sizeof(payload));
      if (n > 0 && mqttClient.connected()) {
        bool ok = mqttClient.publish(mqttTopic, payload);
        Serial.printf("[MQTT] Publish %s -> %s (ok=%d)\n", mqttTopic, payload, ok ? 1 : 0);
      } else {
        Serial.println("[MQTT] Payload empty or MQTT not connected, skip publish.");
      }

      lastSavedTotalTip = snapshotTip;
      lastSavedMinute = m;
      lastSavedSecond = s;
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

/* Task: SD Writer */
void taskSDWriter(void *pvParameters) {
  sensor_msg_t rcv;
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    if (!SD.exists(logFilename)) {
      File f = SD.open(logFilename, FILE_WRITE);
      if (f) {
        f.println("time,suhu,humidity,kec. angin,arah angin,curah hujan,raindrop,suhu_api,humidity_api,kec_angin_api,arah_angin_api,curah_hujan_api");
        f.close();
        Serial.println("[SD] Header file dibuat: cuaca_dataset.csv");
      } else {
        Serial.println("[SD] Gagal membuat file header.");
      }
    }
    xSemaphoreGive(sdMutex);
  } else {
    Serial.println("[SD] Gagal ambil mutex untuk membuat header.");
  }

  for (;;) {
    if (xQueueReceive(sensorQueue, &rcv, portMAX_DELAY) == pdPASS) {
      char timestr[64];
      if (!getISOTimeStrFromSec(rcv.timestamp, timestr, sizeof(timestr))) {
        strcpy(timestr, "1970-01-01 00:00:00");
      }

      String line = String(timestr) + "," +
                    String(rcv.temperature, 2) + "," +
                    String(rcv.humidity, 2) + "," +
                    String(rcv.wind_speed, 2) + "," +
                    String(rcv.wind_dir) + "," +
                    String(rcv.rain_mm_interval, 2) + "," +
                    String(rcv.rain_status == 1 ? "hujan" : "tidak") + "," +
                    /* API data */
                    String(rcv.api_temperature, 2) + "," +
                    String(rcv.api_humidity, 2) + "," +
                    String(rcv.api_wind_speed, 2) + "," +
                    String(rcv.api_wind_dir) + "," +
                    String(rcv.api_rain_mm, 2);

      if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        File f = SD.open(logFilename, FILE_APPEND);
        if (f) {
          f.println(line);
          f.close();
          Serial.printf("[SD] Ditulis: %s\n", line.c_str());
        } else {
          Serial.println("[SD] Gagal buka file untuk append!");
        }
        xSemaphoreGive(sdMutex);
      } else {
        Serial.println("[SD] Gagal ambil mutex (timeout). Data tidak ditulis.");
      }
    }
  }
}

/* setup() */
void setup() {
  Serial.begin(9600);
  delay(100);

  dht.begin();
  Serial.println("DHT22 ready.");

  Serial2.begin(9600, SERIAL_8N1, RX2_PIN, TX2_PIN);
  Serial.println("Serial2 (wind dir) started.");

  RS485Serial.begin(9600, SERIAL_8N1, RX_RS485, TX_RS485);
  Serial.println("RS485 (anemometer) started.");

  if (!SD.begin(SD_CS)) {
    Serial.println("Gagal inisialisasi SD card! Periksa wiring & tegangan (3.3V).");
  } else {
    Serial.println("SD card siap.");
  }

  sdMutex = xSemaphoreCreateMutex();
  dataMutex = xSemaphoreCreateMutex();
  if (sdMutex == NULL) Serial.println("Gagal membuat sdMutex!");
  if (dataMutex == NULL) Serial.println("Gagal membuat dataMutex!");
  sensorQueue = xQueueCreate(QUEUE_LENGTH, sizeof(sensor_msg_t));
  if (sensorQueue == NULL) Serial.println("Gagal membuat queue!");

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

  mqttClient.setServer(mqttServer, mqttPort);
  reconnectMQTT();
  
  /* Get initial API data jika WiFi siap */
  if (WiFi.status() == WL_CONNECTED) {
    delay(2000);
    getWeatherFromAPI();
  }

  sntp_set_time_sync_notification_cb(timeavailable);
  esp_sntp_servermode_dhcp(1);
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);

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

  pinMode(rainSensorPin, INPUT);
  attachInterrupt(digitalPinToInterrupt(rainSensorPin), ISR_countTip, RISING);

  BaseType_t ok;
  ok = xTaskCreatePinnedToCore(taskdht, "taskdht", 4096, NULL, 1, NULL, 1);
  if (ok != pdPASS) Serial.println("Gagal buat taskdht");
  ok = xTaskCreatePinnedToCore(taskRainAnalog, "TaskRainAnalog", 4096, NULL, 1, NULL, 1);
  if (ok != pdPASS) Serial.println("Gagal buat TaskRainAnalog");
  ok = xTaskCreatePinnedToCore(taskWindDir, "TaskWindDir", 4096, NULL, 1, NULL, 1);
  if (ok != pdPASS) Serial.println("Gagal buat TaskWindDir");
  ok = xTaskCreatePinnedToCore(taskWindRS485, "TaskWindRS485", 4096, NULL, 1, NULL, 1);
  if (ok != pdPASS) Serial.println("Gagal buat TaskWindRS485");
  ok = xTaskCreatePinnedToCore(taskAggregator, "TaskAggregator", 4096, NULL, 1, NULL, 1);
  if (ok != pdPASS) Serial.println("Gagal buat TaskAggregator");
  ok = xTaskCreatePinnedToCore(taskAPIWeather, "TaskAPIWeather", 8192, NULL, 1, NULL, 0);
  if (ok != pdPASS) Serial.println("Gagal buat TaskAPIWeather");
  ok = xTaskCreatePinnedToCore(taskSDWriter, "TaskSD", 8192, NULL, 1, NULL, 1);
  if (ok != pdPASS) Serial.println("Gagal buat TaskSDWriter");

  Serial.println("Setup selesai. Tasks dijalankan.");
}

/* loop() */
void loop() {
  if (!mqttClient.connected()) {
    reconnectMQTT();
  } else {
    mqttClient.loop();
  }

  vTaskDelay(pdMS_TO_TICKS(1000));
}
/*
  Test DHT22 Sensor
  Membaca suhu dan kelembaban dari sensor DHT22
  Pin: GPIO26 (DHTPIN)
*/

#include <Arduino.h>
#include "DHT.h"

#define DHTPIN 26
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// Koefisien hasil regresi (dari kode utama)
const float HUM_A = 0.3587537;     // slope
const float HUM_B = 43.1950459;    // intercept

const float TMP_A = 0.4540112;     // slope
const float TMP_B = 13.6536084;    // intercept

void setup() {
  Serial.begin(9600);
  delay(1000);
  
  Serial.println("=== TEST DHT22 SENSOR ===");
  Serial.printf("Pin: %d\n", DHTPIN);
  Serial.println("Menginisialisasi DHT22...");
  
  dht.begin();
  delay(2000);
  
  Serial.println("DHT22 siap!");
  Serial.println("Mulai membaca data...\n");
}

void loop() {
  // Baca data raw
  float h_raw = dht.readHumidity();
  float t_raw = dht.readTemperature();
  
  if (isnan(h_raw) || isnan(t_raw)) {
    Serial.println("Gagal membaca DHT22! Periksa koneksi dan pin.");
    delay(2000);
    return;
  }
  
  // Terapkan kalibrasi (regresi)
  float h_calibrated = HUM_A * h_raw + HUM_B;
  float t_calibrated = TMP_A * t_raw + TMP_B;
  
  // Tampilkan hasil
  Serial.printf("Waktu: %ld ms\n", millis());
  Serial.printf("Raw: Temp=%.2f°C, Humidity=%.2f%%\n", t_raw, h_raw);
  Serial.printf("Calibrated: Temp=%.2f°C, Humidity=%.2f%%\n", t_calibrated, h_calibrated);
  Serial.printf("Selisih Temp: %.2f°C, Humidity: %.2f%%\n", 
                t_calibrated - t_raw, h_calibrated - h_raw);
  Serial.println("---");
  
  delay(10000); // Baca setiap 10 detik (sesuai sampling rate)
}

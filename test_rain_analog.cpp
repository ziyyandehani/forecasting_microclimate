/*
  Test Rain Analog Sensor (Intensity/Raindrop Detection)
  Membaca ADC dari pin AO_PIN (GPIO39)
  Nilai raw: 0-4095
  Status hujan: < 3000 = hujan, >= 3000 = tidak hujan
*/

#include <Arduino.h>

#define AO_PIN 39
const int RAIN_THRESHOLD = 3000;
const TickType_t SAMPLE_INTERVAL = 2000;  // 2 detik (sesuai sampling rate)

void setup() {
  Serial.begin(9600);
  delay(1000);
  
  Serial.println("=== TEST RAIN ANALOG SENSOR (RAINDROP INTENSITY) ===");
  Serial.printf("Pin: GPIO%d (ADC)\n", AO_PIN);
  Serial.printf("Rain threshold: %d\n", RAIN_THRESHOLD);
  Serial.printf("Sample interval: %lu ms\n", SAMPLE_INTERVAL);
  Serial.println("< 3000 = HUJAN, >= 3000 = TIDAK HUJAN\n");
}

void loop() {
  static unsigned long lastReadTime = 0;
  unsigned long now = millis();
  
  if (now - lastReadTime >= SAMPLE_INTERVAL) {
    // Baca raw ADC value
    int rawValue = analogRead(AO_PIN);
    
    // Konversi ke voltage (asumsi ADC 12-bit, ref 3.3V)
    float voltage = (rawValue / 4095.0) * 3.3;
    
    // Tentukan status hujan
    int rainStatus = (rawValue < RAIN_THRESHOLD) ? 1 : 0;  // 1=hujan, 0=tidak hujan
    String statusStr = (rainStatus == 1) ? "HUJAN" : "TIDAK HUJAN";
    
    // Hitung intensitas (0-100%)
    float intensity = 0.0;
    if (rawValue < RAIN_THRESHOLD) {
      intensity = ((RAIN_THRESHOLD - rawValue) / (float)RAIN_THRESHOLD) * 100.0;
    }
    
    // Tampilkan hasil
    Serial.printf("Waktu: %ld ms\n", now);
    Serial.printf("ADC Raw: %d\n", rawValue);
    Serial.printf("Voltage: %.3f V\n", voltage);
    Serial.printf("Status: %s\n", statusStr.c_str());
    Serial.printf("Intensitas: %.1f%%\n", intensity);
    Serial.println("---\n");
    
    lastReadTime = now;
  }
  
  delay(100);
}

/*
  Test Rain Tipping Bucket Sensor
  Menghitung jumlah tip (tips) dan konversi ke mm
  Pin: GPIO34 (rainSensorPin)
*/

#include <Arduino.h>

#define RAIN_SENSOR_PIN 34
const float MM_PER_TIP = 0.7;  // Setiap tip = 0.7 mm
const unsigned long DEBOUNCE_MS = 500UL;

volatile unsigned long tipCount = 0;
volatile unsigned long lastInterruptTime = 0;

void IRAM_ATTR ISR_countTip() {
  unsigned long now = millis();
  if (now - lastInterruptTime >= DEBOUNCE_MS) {
    tipCount++;
    lastInterruptTime = now;
    Serial.printf("[ISR] Tip detected! Total tips: %lu\n", tipCount);
  }
}

void setup() {
  Serial.begin(9600);
  delay(1000);
  
  Serial.println("=== TEST RAIN TIPPING BUCKET SENSOR ===");
  Serial.printf("Pin: %d\n", RAIN_SENSOR_PIN);
  Serial.printf("MM per tip: %.1f mm\n", MM_PER_TIP);
  Serial.printf("Debounce time: %lu ms\n", DEBOUNCE_MS);
  Serial.println("Tunggu sinyal dari sensor...\n");
  
  pinMode(RAIN_SENSOR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(RAIN_SENSOR_PIN), ISR_countTip, RISING);
  
  Serial.println("Interrupt attached. Monitor readings...\n");
}

void loop() {
  static unsigned long lastDisplayTime = 0;
  static unsigned long lastResetTime = 0;
  static unsigned long lastTipCount = 0;
  
  unsigned long now = millis();
  
  // Tampilkan data setiap 10 detik
  if (now - lastDisplayTime >= 10000) {
    noInterrupts();
    unsigned long currentTipCount = tipCount;
    interrupts();
    
    unsigned long tipsDelta = (currentTipCount >= lastTipCount) 
                               ? (currentTipCount - lastTipCount) 
                               : currentTipCount;
    
    float rainTotal = currentTipCount * MM_PER_TIP;
    float rainInterval = tipsDelta * MM_PER_TIP;
    
    Serial.printf("Waktu: %ld ms\n", now);
    Serial.printf("Total tips (sejak startup): %lu\n", currentTipCount);
    Serial.printf("Tips dalam interval 10s terakhir: %lu\n", tipsDelta);
    Serial.printf("Total curah hujan: %.2f mm\n", rainTotal);
    Serial.printf("Curah hujan interval: %.2f mm\n", rainInterval);
    Serial.println("---\n");
    
    lastDisplayTime = now;
    lastTipCount = currentTipCount;
  }
  
  // Reset counter setiap 1 jam (untuk testing)
  if (now - lastResetTime >= 3600000) {
    noInterrupts();
    tipCount = 0;
    interrupts();
    lastTipCount = 0;
    Serial.println("[INFO] Counter di-reset setelah 1 jam\n");
    lastResetTime = now;
  }
  
  delay(100);
}

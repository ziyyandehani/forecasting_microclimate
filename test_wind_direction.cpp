/*
  Test Wind Direction Sensor
  Membaca arah angin dari Serial2 (RX=33, TX=32)
  Format data: *code# (contoh: *1#, *2#, ..., *8#)
*/

#include <Arduino.h>

#define RX2_PIN 33
#define TX2_PIN 32

void setup() {
  Serial.begin(9600);
  Serial2.begin(9600, SERIAL_8N1, RX2_PIN, TX2_PIN);
  
  delay(1000);
  
  Serial.println("=== TEST WIND DIRECTION SENSOR ===");
  Serial.printf("Serial2 RX: GPIO%d, TX: GPIO%d\n", RX2_PIN, TX2_PIN);
  Serial.println("Format: *code# (1-8)");
  Serial.println("1=Utara, 2=Timur Laut, 3=Timur, 4=Tenggara,");
  Serial.println("5=Selatan, 6=Barat Daya, 7=Barat, 8=Barat Laut");
  Serial.println("Menunggu data dari sensor...\n");
}

// Konversi code ke arah mata angin
String getWindDirection(uint8_t code) {
  switch(code) {
    case 1: return "UTARA";
    case 2: return "TIMUR LAUT";
    case 3: return "TIMUR";
    case 4: return "TENGGARA";
    case 5: return "SELATAN";
    case 6: return "BARAT DAYA";
    case 7: return "BARAT";
    case 8: return "BARAT LAUT";
    default: return "UNKNOWN";
  }
}

void loop() {
  static String dataBuf = "";
  static unsigned long lastValidReadTime = 0;
  
  // Baca data dari Serial2
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    dataBuf += c;
    
    // Cari frame: *code#
    int startIdx = dataBuf.indexOf('*');
    int endIdx = dataBuf.indexOf('#');
    
    if (startIdx >= 0 && endIdx > startIdx) {
      String codeStr = dataBuf.substring(startIdx + 1, endIdx);
      dataBuf = dataBuf.substring(endIdx + 1);  // Bersihkan buffer
      
      // Parsing code
      uint8_t code = codeStr.toInt();
      if (code >= 1 && code <= 8) {
        String direction = getWindDirection(code);
        unsigned long now = millis();
        
        Serial.printf("Waktu: %ld ms\n", now);
        Serial.printf("Raw code: %u\n", code);
        Serial.printf("Arah: %s\n", direction.c_str());
        Serial.printf("Buffer: %s (panjang=%d)\n", dataBuf.c_str(), dataBuf.length());
        Serial.println("---");
        
        lastValidReadTime = now;
      } else {
        Serial.printf("[ERROR] Invalid code: %s\n", codeStr.c_str());
      }
    }
  }
  
  // Tampilkan status jika 15 detik tidak ada data (sampling rate seharusnya 15s)
  static unsigned long lastStatusTime = 0;
  if (millis() - lastStatusTime > 15000) {
    unsigned long timeSinceLastRead = millis() - lastValidReadTime;
    Serial.printf("[STATUS] Tidak ada data selama %lu ms\n", timeSinceLastRead);
    Serial.printf("[DEBUG] Buffer length: %d\n\n", dataBuf.length());
    lastStatusTime = millis();
  }
  
  delay(100);
}

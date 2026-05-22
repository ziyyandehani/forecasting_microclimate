/*
  Test Wind Speed Sensor (RS485 Anemometer)
  Membaca kecepatan angin via Modbus RTU (RS485 UART1)
  RX: GPIO16, TX: GPIO17
  Device address: 0x02
  Register: 0x002A
  Dengan kalibrasi/calibration
*/

#include <Arduino.h>
#include <HardwareSerial.h>

#define RX_RS485 16
#define TX_RS485 17
HardwareSerial RS485Serial(1);

#define DEFAULT_DEVICE_ADDRESS  0x02
#define WIND_SPEED_REG_ADDR 0x002A
#define READ_HOLDING_REG  0x03

const TickType_t SAMPLE_INTERVAL = 5000;  // 5 detik (sesuai sampling rate)

// Fungsi untuk hitung CRC16
unsigned int calculateCRC(unsigned char *frame, unsigned char bufferSize) {
  unsigned int temp, temp2, flag;
  temp = 0xFFFF;
  for (unsigned char i = 0; i < bufferSize; i++) {
    temp = temp ^ frame[i];
    for (unsigned char j = 1; j <= 8; j++) {
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

// Fungsi kalibrasi (sesuai dengan kode utama)
float calibrate(float raw) {
  if (raw < 1.5) raw = 0.0;
  
  if (raw < 3.99)
    return 0.688 * raw + 0.598;
  
  return 0.709 * raw + 0.204;
}

// Fungsi untuk membaca wind speed via Modbus RTU
float getWindSpeed(byte address) {
  float windSpeed = NAN;
  byte Anemometer_buf[8];
  
  // Buat request frame Modbus RTU
  byte Anemometer_request[] = {
    address, 
    READ_HOLDING_REG, 
    0x00, 
    (byte)WIND_SPEED_REG_ADDR, 
    0x00, 
    0x01, 
    0x00, 
    0x00
  };
  
  // Hitung dan tambahkan CRC16
  unsigned int crc16 = calculateCRC(Anemometer_request, sizeof(Anemometer_request) - 2);
  Anemometer_request[sizeof(Anemometer_request) - 2] = crc16 >> 8;
  Anemometer_request[sizeof(Anemometer_request) - 1] = crc16 & 0xFF;
  
  // Kirim request
  RS485Serial.write(Anemometer_request, sizeof(Anemometer_request));
  RS485Serial.flush();
  
  // Tunggu response (max 200ms)
  unsigned long start = millis();
  size_t idx = 0;
  while (idx < 7 && (millis() - start) < 200) {
    if (RS485Serial.available()) {
      Anemometer_buf[idx++] = RS485Serial.read();
    }
  }
  
  // Parse response
  if (idx >= 5) {
    byte dataH = Anemometer_buf[3];
    byte dataL = Anemometer_buf[4];
    windSpeed = ((dataH << 8) | dataL) / 100.0;
  } else {
    windSpeed = NAN;
  }
  
  return windSpeed;
}

void setup() {
  Serial.begin(9600);
  RS485Serial.begin(9600, SERIAL_8N1, RX_RS485, TX_RS485);
  
  delay(2000);
  
  Serial.println("=== TEST WIND SPEED SENSOR (RS485 ANEMOMETER) ===");
  Serial.printf("UART1 RX: GPIO%d, TX: GPIO%d\n", RX_RS485, TX_RS485);
  Serial.printf("Device address: 0x%02X\n", DEFAULT_DEVICE_ADDRESS);
  Serial.printf("Modbus register: 0x%04X\n", WIND_SPEED_REG_ADDR);
  Serial.printf("Sample interval: %lu ms\n", SAMPLE_INTERVAL);
  Serial.println("Memulai pembacaan...\n");
}

void loop() {
  static unsigned long lastReadTime = 0;
  unsigned long now = millis();
  
  if (now - lastReadTime >= SAMPLE_INTERVAL) {
    // Baca wind speed mentah
    float rawSpeed = getWindSpeed(DEFAULT_DEVICE_ADDRESS);
    
    if (!isnan(rawSpeed)) {
      // Terapkan kalibrasi
      float calibratedSpeed = calibrate(rawSpeed);
      
      // Set minimum threshold
      if (calibratedSpeed < 0.05) {
        calibratedSpeed = 0.0;
      }
      
      // Tampilkan hasil
      Serial.printf("Waktu: %ld ms\n", now);
      Serial.printf("Raw speed: %.2f m/s\n", rawSpeed);
      Serial.printf("Calibrated speed: %.2f m/s\n", calibratedSpeed);
      Serial.printf("Status: OK\n");
    } else {
      Serial.printf("Waktu: %ld ms\n", now);
      Serial.printf("Raw speed: NAN\n");
      Serial.printf("Status: GAGAL BACA (timeout/error)\n");
    }
    
    Serial.println("---\n");
    lastReadTime = now;
  }
  
  delay(100);
}

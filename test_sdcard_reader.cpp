/*
  Test pembacaan SD Card ESP32
  - Pin CS: GPIO 5
  - SPI default (MOSI=23, MISO=19, CLK=18)
  - Fungsi: List files, baca file, write file
*/

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

/* SD Card */
#define SD_CS 5

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=== SD Card Test ===");
  Serial.printf("Inisialisasi SD card dengan pin CS=%d...\n", SD_CS);
  
  if (!SD.begin(SD_CS)) {
    Serial.println("ERROR: Gagal inisialisasi SD card!");
    Serial.println("Periksa:");
    Serial.println("  1. Koneksi wiring");
    Serial.println("  2. Voltase 3.3V");
    Serial.println("  3. Kondisi SD card");
    while(1) {
      delay(1000);
    }
  }
  
  Serial.println("SD card READY!");
  
  // List semua file di root
  listFiles(SD, "/", 0);
  
  // Test write file
  testWriteFile();
  
  // Test read file
  testReadFile();
}

void loop() {
  Serial.println("\n--- Menu ---");
  Serial.println("1. List files");
  Serial.println("2. Read file");
  Serial.println("3. Write file");
  Serial.println("4. Delete file");
  Serial.println("Masukkan command: ");
  
  delay(5000);
}

/* List semua file */
void listFiles(fs::FS &fs, const char * dirname, uint8_t levels) {
  Serial.printf("Listing directory: %s\n", dirname);
  
  File root = fs.open(dirname);
  if (!root) {
    Serial.println("Failed to open directory");
    return;
  }
  if (!root.isDirectory()) {
    Serial.println("Not a directory");
    return;
  }
  
  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      Serial.print("  DIR : ");
      Serial.println(file.name());
      if (levels) {
        listFiles(fs, file.path(), levels - 1);
      }
    } else {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print(" | SIZE: ");
      Serial.print(file.size());
      Serial.println(" bytes");
    }
    file = root.openNextFile();
  }
}

/* Test write file */
void testWriteFile() {
  const char *filename = "/test.txt";
  
  Serial.printf("\nWrite test ke %s...\n", filename);
  
  File file = SD.open(filename, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }
  
  file.println("=== SD Card Test ===");
  file.println("Timestamp: 2026-05-15 10:30:00");
  file.println("Test write berhasil!");
  file.println("Temp: 25.5C");
  file.println("Humidity: 65%");
  
  file.close();
  Serial.println("File written successfully!");
}

/* Test read file */
void testReadFile() {
  const char *filename = "/test.txt";
  
  Serial.printf("\nRead test dari %s...\n", filename);
  
  if (!SD.exists(filename)) {
    Serial.printf("File %s tidak ada\n", filename);
    return;
  }
  
  File file = SD.open(filename);
  if (!file) {
    Serial.println("Failed to open file for reading");
    return;
  }
  
  Serial.println("--- Content ---");
  while (file.available()) {
    Serial.write(file.read());
  }
  Serial.println("\n--- End ---");
  
  file.close();
}

/* Delete file */
void deleteFile(fs::FS &fs, const char * path) {
  Serial.printf("Deleting file: %s\n", path);
  if (fs.remove(path)) {
    Serial.println("File deleted");
  } else {
    Serial.println("Delete failed");
  }
}

/* Append ke file */
void appendFile(fs::FS &fs, const char * path, const char * message) {
  Serial.printf("Appending to file: %s\n", path);
  
  File file = fs.open(path, FILE_APPEND);
  if (!file) {
    Serial.println("Failed to open file for appending");
    return;
  }
  
  if (file.print(message)) {
    Serial.println("Message appended");
  } else {
    Serial.println("Append failed");
  }
  
  file.close();
}

/* Check file size */
void checkFileSize(fs::FS &fs, const char * path) {
  File file = fs.open(path);
  if (!file) {
    Serial.println("File not found");
    return;
  }
  
  Serial.printf("File: %s\n", path);
  Serial.printf("Size: %d bytes\n", file.size());
  
  file.close();
}

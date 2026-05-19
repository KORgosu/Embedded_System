#include <SPI.h>
#include <SdFat.h>

#define SD_CS_PIN 4

SdFat sd;
FatFile dir;
FatFile file;

void setup() {
  Serial.begin(9600);
  delay(2000);

  if (!sd.begin(SD_CS_PIN, SD_SCK_MHZ(1))) {
    Serial.println("SD card 초기화 실패!");
    while(1);
  }
  Serial.println("SD card 초기화 성공!");

  // /HMC 폴더 생성
  if (!sd.exists("/HMC")) {
    sd.mkdir("/HMC");
    Serial.println("/HMC 폴더 생성");
  } else {
    Serial.println("/HMC 폴더 이미 존재");
  }

  // /MPU 폴더 생성
  if (!sd.exists("/MPU")) {
    sd.mkdir("/MPU");
    Serial.println("/MPU 폴더 생성");
  } else {
    Serial.println("/MPU 폴더 이미 존재");
  }

  // /HMC/data.txt 파일 쓰기
  if (file.open("/HMC/data.txt", O_WRITE | O_CREAT | O_APPEND)) {
    const char* msg1 = "HMC5883 test data\n";
    file.write(msg1, strlen(msg1)); // ← println 대신 write 사용
    file.close();
    Serial.println("/HMC/data.txt 쓰기 성공");
  } else {
    Serial.println("/HMC/data.txt 쓰기 실패");
  }

  // /MPU/data.txt 파일 쓰기
  if (file.open("/MPU/data.txt", O_WRITE | O_CREAT | O_APPEND)) {
    const char* msg2 = "MPU6050 test data\n";
    file.write(msg2, strlen(msg2)); // ← println 대신 write 사용
    file.close();
    Serial.println("/MPU/data.txt 쓰기 성공");
  } else {
    Serial.println("/MPU/data.txt 쓰기 실패");
  }

  // /HMC/data.txt 읽기
  Serial.println("=== /HMC/data.txt 읽기 ===");
  if (file.open("/HMC/data.txt", O_READ)) {
    while (file.available()) {
      Serial.write(file.read());
    }
    file.close();
  }

  // /MPU/data.txt 읽기
  Serial.println("=== /MPU/data.txt 읽기 ===");
  if (file.open("/MPU/data.txt", O_READ)) {
    while (file.available()) {
      Serial.write(file.read());
    }
    file.close();
  }
}

void loop() {}
#include <FreeRTOS_ARM.h>
#include <Wire.h>
#include <SPI.h>
#include <SdFat.h>

// 핀 정의
#define SD_CS_PIN    10  // D4 → D10으로 변경 (I2C 간섭 방지)
#define HMC5883_ADDR 0x2C
#define MPU6050_ADDR 0x68

// 전방 선언
void readHMC5883();
void readMPU6050();

// SdFat 객체
SdFat sd;
FatFile file;

// 태스크 핸들
TaskHandle_t handle_hmc5883_task;
TaskHandle_t handle_mpu6050_task;
TaskHandle_t handle_sd_task;

// Mutex 핸들
SemaphoreHandle_t i2cMutex; // I2C 버스 보호
SemaphoreHandle_t spiMutex; // SPI 버스 보호

// 센서 데이터 전역 변수
volatile int16_t g_hx = 0, g_hy = 0, g_hz = 0;
volatile int16_t g_ax = 0, g_ay = 0, g_az = 0;
volatile int16_t g_gx = 0, g_gy = 0, g_gz = 0;


// 센서 초기화
void initHMC5883() {
  // X,Y,Z 축 부호 정의
  Wire.beginTransmission(HMC5883_ADDR);
  Wire.write(0x29); Wire.write(0x06);
  Wire.endTransmission();

  // Set/Reset On, Field Range 8Gauss
  Wire.beginTransmission(HMC5883_ADDR);
  Wire.write(0x0B); Wire.write(0x08);
  Wire.endTransmission();

  // Continuous Mode 전환 (Suspend → Continuous)
  Wire.beginTransmission(HMC5883_ADDR);
  Wire.write(0x0A); Wire.write(0xC3);
  Wire.endTransmission();
}

void initMPU6050() {
  // Sleep Mode 해제
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x6B); Wire.write(0x00);
  Wire.endTransmission();
}


// 센서 읽기
void readHMC5883() {
  uint8_t status = 0;
  int16_t x, y, z;

  Wire.beginTransmission(HMC5883_ADDR);
  Wire.write(0x09);
  Wire.endTransmission(false);
  Wire.requestFrom(HMC5883_ADDR, 1);
  if (Wire.available()) status = Wire.read();

  if (status & 0x01) {
    Wire.beginTransmission(HMC5883_ADDR);
    Wire.write(0x00);
    Wire.endTransmission(false);
    Wire.requestFrom(HMC5883_ADDR, 6);
    if (Wire.available() == 6) {
      uint8_t xLSB = Wire.read(); uint8_t xMSB = Wire.read();
      uint8_t yLSB = Wire.read(); uint8_t yMSB = Wire.read();
      uint8_t zLSB = Wire.read(); uint8_t zMSB = Wire.read();
      g_hx = (int16_t)(xMSB << 8 | xLSB);
      g_hy = (int16_t)(yMSB << 8 | yLSB);
      g_hz = (int16_t)(zMSB << 8 | zLSB);
    }
  }
}

void readMPU6050() {
  // 가속도 읽기
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU6050_ADDR, 6);
  if (Wire.available() == 6) {
    g_ax = (int16_t)(Wire.read() << 8 | Wire.read());
    g_ay = (int16_t)(Wire.read() << 8 | Wire.read());
    g_az = (int16_t)(Wire.read() << 8 | Wire.read());
  }

  // 자이로 읽기
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x43);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU6050_ADDR, 6);
  if (Wire.available() == 6) {
    g_gx = (int16_t)(Wire.read() << 8 | Wire.read());
    g_gy = (int16_t)(Wire.read() << 8 | Wire.read());
    g_gz = (int16_t)(Wire.read() << 8 | Wire.read());
  }
}


// Task 1: HMC5883 읽기
void task_hmc5883(void *pvParameters) {
  Serial.println("HMC5883 task started");

  for (;;) {
    if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
      readHMC5883();
      Serial.print("[HMC] X:"); Serial.print(g_hx);
      Serial.print(" Y:"); Serial.print(g_hy);
      Serial.print(" Z:"); Serial.println(g_hz);
      xSemaphoreGive(i2cMutex);
    }
    vTaskDelay(5000);
  }
}


// Task 2: MPU6050 읽기
void task_mpu6050(void *pvParameters) {
  Serial.println("MPU6050 task started");

  for (;;) {
    if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
      readMPU6050();
      Serial.print("[MPU Acc] X:"); Serial.print(g_ax);
      Serial.print(" Y:"); Serial.print(g_ay);
      Serial.print(" Z:"); Serial.println(g_az);
      Serial.print("[MPU Gyro] X:"); Serial.print(g_gx);
      Serial.print(" Y:"); Serial.print(g_gy);
      Serial.print(" Z:"); Serial.println(g_gz);
      xSemaphoreGive(i2cMutex);
    }
    vTaskDelay(5000);
  }
}


// Task 3: SD card 저장 + UART 명령
// 'r' → 파일 읽기
// 'c' → 파일 초기화
void task_sd(void *pvParameters) {
  Serial.println("SD task started");

  TickType_t lastSave = xTaskGetTickCount();

  for (;;) {
    // 10초마다 저장
    if ((xTaskGetTickCount() - lastSave) >= pdMS_TO_TICKS(10000)) {
      if (xSemaphoreTake(spiMutex, portMAX_DELAY) == pdTRUE) {

        // HMC5883 데이터 저장
        if (file.open("/HMC/data.txt", O_WRITE | O_CREAT | O_APPEND)) {
          char buf[64];
          snprintf(buf, sizeof(buf), "X:%d Y:%d Z:%d\n", g_hx, g_hy, g_hz);
          file.write(buf, strlen(buf));
          file.sync();
          file.close();
        }

        // MPU6050 데이터 저장
        if (file.open("/MPU/data.txt", O_WRITE | O_CREAT | O_APPEND)) {
          char buf[128];
          snprintf(buf, sizeof(buf),
            "Acc X:%d Y:%d Z:%d Gyro X:%d Y:%d Z:%d\n",
            g_ax, g_ay, g_az, g_gx, g_gy, g_gz);
          file.write(buf, strlen(buf));
          file.sync();
          file.close();
        }

        Serial.println("SD task: saved");
        xSemaphoreGive(spiMutex);
      }
      lastSave = xTaskGetTickCount();
    }

    // UART 명령 처리
    if (Serial.available()) {
      char cmd = Serial.read();

      // 'r': 저장된 데이터 출력
      if (cmd == 'r') {
        if (xSemaphoreTake(spiMutex, portMAX_DELAY) == pdTRUE) {

          Serial.println("=== /HMC/data.txt ===");
          if (file.open("/HMC/data.txt", O_READ)) {
            while (file.available()) Serial.write(file.read());
            file.close();
          } else {
            Serial.println("파일 없음");
          }

          Serial.println("=== /MPU/data.txt ===");
          if (file.open("/MPU/data.txt", O_READ)) {
            while (file.available()) Serial.write(file.read());
            file.close();
          } else {
            Serial.println("파일 없음");
          }

          Serial.println("=== 출력 완료 ===");
          xSemaphoreGive(spiMutex);
        }
      }

      // 'c': 파일 내용 초기화 (O_TRUNC: 기존 내용 삭제)
      if (cmd == 'c') {
        if (xSemaphoreTake(spiMutex, portMAX_DELAY) == pdTRUE) {

          if (file.open("/HMC/data.txt", O_WRITE | O_CREAT | O_TRUNC)) {
            file.close();
            Serial.println("/HMC/data.txt 초기화 완료");
          }
          if (file.open("/MPU/data.txt", O_WRITE | O_CREAT | O_TRUNC)) {
            file.close();
            Serial.println("/MPU/data.txt 초기화 완료");
          }
          Serial.println("데이터 초기화 완료");
          xSemaphoreGive(spiMutex);
        }
      }
    }
    vTaskDelay(100);
  }
}

void setup() {
  Serial.begin(9600);
  Wire.begin();

  // Mutex 먼저 생성
  i2cMutex = xSemaphoreCreateMutex();
  spiMutex = xSemaphoreCreateMutex();

  // 센서 초기화
  initHMC5883();
  initMPU6050();

  // SD card 초기화
  if (!sd.begin(SD_CS_PIN, SD_SCK_MHZ(1))) {
    Serial.println("SD card 초기화 실패!");
    while(1);
  }
  Serial.println("SD card 초기화 성공!");

  // 폴더 생성
  if (!sd.exists("/HMC")) sd.mkdir("/HMC");
  if (!sd.exists("/MPU")) sd.mkdir("/MPU");

  if (i2cMutex != NULL && spiMutex != NULL) {
    xTaskCreate(task_hmc5883, "HMC5883 Task", 256,  NULL, 2, &handle_hmc5883_task);
    xTaskCreate(task_mpu6050, "MPU6050 Task", 256,  NULL, 2, &handle_mpu6050_task);
    xTaskCreate(task_sd,      "SD Task",      1024, NULL, 2, &handle_sd_task);
    vTaskStartScheduler();
  }

  Serial.println("Failed to start FreeRTOS scheduler");
  while(1);
}

void loop() {
  // FreeRTOS 사용할땐 loop 사용 안 함
}

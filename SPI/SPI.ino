#include <FreeRTOS_ARM.h>
#include <Wire.h>
#include <SPI.h>

// 핀 정의
#define CS_PIN       10
#define HMC5883_ADDR 0x2C
#define MPU6050_ADDR 0x68
#define DATA_SIZE    32  // 32바이트 = 18바이트 데이터 + 14바이트 패딩

// 전방 선언
void waitBusy();
void writeEnable();
void sectorErase(uint32_t addr);
void pageProgram(uint32_t addr, uint8_t* buf, uint32_t len);
void readData(uint32_t addr, uint8_t* buf, uint32_t len);
void findLastAddr();

// 태스크 핸들
TaskHandle_t handle_write_task;
TaskHandle_t handle_read_task;

// Mutex 핸들
SemaphoreHandle_t spiMutex;

// Flash 쓰기 주소 (전역)
uint32_t flashAddr = 0x000000;

// 센서 초기화
void initHMC5883() {
  // X,Y,Z 축 부호 정의
  Wire.beginTransmission(HMC5883_ADDR);
  Wire.write(0x29);
  Wire.write(0x06);
  Wire.endTransmission();

  // Set/Reset On, Field Range 8Gauss
  Wire.beginTransmission(HMC5883_ADDR);
  Wire.write(0x0B);
  Wire.write(0x08);
  Wire.endTransmission();

  // Continuous Mode 전환
  Wire.beginTransmission(HMC5883_ADDR);
  Wire.write(0x0A);
  Wire.write(0xC3);
  Wire.endTransmission();
}

void initMPU6050() {
  // Sleep Mode 해제
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission();
}


// 센서 읽기 함수
void readHMC5883(int16_t* x, int16_t* y, int16_t* z) {
  // 0x00부터 6바이트 읽기
  Wire.beginTransmission(HMC5883_ADDR);
  Wire.write(0x00);
  Wire.endTransmission(false);
  Wire.requestFrom(HMC5883_ADDR, 6);
  if (Wire.available() == 6) {
    uint8_t xLSB = Wire.read(); // 00H : X LSB
    uint8_t xMSB = Wire.read(); // 01H : X MSB
    uint8_t yLSB = Wire.read(); // 02H : Y LSB
    uint8_t yMSB = Wire.read(); // 03H : Y MSB
    uint8_t zLSB = Wire.read(); // 04H : Z LSB
    uint8_t zMSB = Wire.read(); // 05H : Z MSB
    *x = (int16_t)(xMSB << 8 | xLSB);
    *y = (int16_t)(yMSB << 8 | yLSB);
    *z = (int16_t)(zMSB << 8 | zLSB);
  }
}

void readMPU6050(int16_t* ax, int16_t* ay, int16_t* az,
                 int16_t* gx, int16_t* gy, int16_t* gz) {
  // 가속도 읽기
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU6050_ADDR, 6);
  if (Wire.available() == 6) {
    *ax = (int16_t)(Wire.read() << 8 | Wire.read());
    *ay = (int16_t)(Wire.read() << 8 | Wire.read());
    *az = (int16_t)(Wire.read() << 8 | Wire.read());
  }

  // 자이로 읽기
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x43);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU6050_ADDR, 6);
  if (Wire.available() == 6) {
    *gx = (int16_t)(Wire.read() << 8 | Wire.read());
    *gy = (int16_t)(Wire.read() << 8 | Wire.read());
    *gz = (int16_t)(Wire.read() << 8 | Wire.read());
  }
}


// Flash 기본 함수
void waitBusy() { // Flash가 명령을 받으면 내부적으로 처리하는 시간이 필요함
  // BUSY bit = 0 될 때까지 대기
  uint8_t status;
  do {
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(CS_PIN, LOW);
    SPI.transfer(0x05);          // Read Status Register-1
    status = SPI.transfer(0x00);
    digitalWrite(CS_PIN, HIGH);
    SPI.endTransaction();
  } while (status & 0x01);       // BUSY bit(bit0) = 1이면 대기
}

void writeEnable() { // 전원 최초 인가 시, 기본값이 Write Disable이기 때문에, 이를 호출하여 Write Enable로 전환
  // Erase/Program 전에 반드시 호출
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(0x06);            // Write Enable 명령어
  digitalWrite(CS_PIN, HIGH);
  SPI.endTransaction();
}

void sectorErase(uint32_t addr) { // Flash는 데이터 덮어쓰기가 안됨 -> 새 데이터 쓰기 전에 지워야함
  // addr이 포함된 sector(4KB) 전체를 0xFF로 초기화
  writeEnable();
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(0x20);                    // Sector Erase 명령어
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8)  & 0xFF);
  SPI.transfer((addr >> 0)  & 0xFF);
  digitalWrite(CS_PIN, HIGH);
  SPI.endTransaction();
  waitBusy();
}

void pageProgram(uint32_t addr, uint8_t* buf, uint32_t len) { // 실제 데이터를 Flash에 저장
  // addr 주소부터 buf의 len 바이트를 Flash에 저장
  // 주의: len은 256 이하, page 경계(XXXX00h) 넘으면 안됨
  writeEnable();
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(0x02);                    // Page Program 명령어
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8)  & 0xFF);
  SPI.transfer((addr >> 0)  & 0xFF);
  for (uint32_t i = 0; i < len; i++) {
    SPI.transfer(buf[i]);
  }
  digitalWrite(CS_PIN, HIGH);
  SPI.endTransaction();
  waitBusy();
}

void readData(uint32_t addr, uint8_t* buf, uint32_t len) { // 데이터를 Flash로부터 읽어오는 함수
  // addr 주소부터 len 바이트 읽기
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(0x03);                    // Read Data 명령어
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8)  & 0xFF);
  SPI.transfer((addr >> 0)  & 0xFF);
  for (uint32_t i = 0; i < len; i++) {
    buf[i] = SPI.transfer(0x00);
  }
  digitalWrite(CS_PIN, HIGH);
  SPI.endTransaction();
}

void findLastAddr() {
  // Flash 스캔해서 마지막으로 저장된 주소 찾기
  // 전부 0xFF면 빈 공간 → 거기서부터 저장 시작
  uint8_t buf[DATA_SIZE];
  flashAddr = 0x000000;

  while (flashAddr < 0x800000) {
    readData(flashAddr, buf, DATA_SIZE);

    bool isEmpty = true;
    for (int i = 0; i < DATA_SIZE; i++) {
      if (buf[i] != 0xFF) {
        isEmpty = false;
        break;
      }
    }
    if (isEmpty) break;
    flashAddr += DATA_SIZE;
  }

  Serial.print("마지막 주소 복원: 0x");
  Serial.println(flashAddr, HEX);
}


// Task 1: 센서 데이터 주기적으로 Flash 저장
void task_write(void *pvParameters) {
  Serial.println("Write task started");

  for (;;) {
    // I2C로 센서 읽기 (Mutex 밖, I2C는 별도 버스)
    int16_t hx = 0, hy = 0, hz = 0;
    int16_t ax = 0, ay = 0, az = 0;
    int16_t gx = 0, gy = 0, gz = 0;

    readHMC5883(&hx, &hy, &hz);
    readMPU6050(&ax, &ay, &az, &gx, &gy, &gz);

    // 32바이트 버퍼 구성
    // [0~17]  실제 데이터 (18바이트)
    // [18~31] 패딩 0x00 (14바이트)
    // → page 경계(XXXX00h) 계산 단순화
    // → 256바이트 page 안에 정확히 8개 레코드 저장 가능
    uint8_t data[DATA_SIZE] = {0};

    data[0]  = (hx >> 8) & 0xFF; data[1]  = hx & 0xFF;
    data[2]  = (hy >> 8) & 0xFF; data[3]  = hy & 0xFF;
    data[4]  = (hz >> 8) & 0xFF; data[5]  = hz & 0xFF;
    data[6]  = (ax >> 8) & 0xFF; data[7]  = ax & 0xFF;
    data[8]  = (ay >> 8) & 0xFF; data[9]  = ay & 0xFF;
    data[10] = (az >> 8) & 0xFF; data[11] = az & 0xFF;
    data[12] = (gx >> 8) & 0xFF; data[13] = gx & 0xFF;
    data[14] = (gy >> 8) & 0xFF; data[15] = gy & 0xFF;
    data[16] = (gz >> 8) & 0xFF; data[17] = gz & 0xFF;
    // [18~31] 패딩 → 이미 0x00으로 초기화됨

    // SPI Mutex로 Flash 저장
    if (xSemaphoreTake(spiMutex, portMAX_DELAY) == pdTRUE) {

      // sector 경계마다 Erase (4KB = 0x1000)
      if ((flashAddr % 0x1000) == 0) {
        sectorErase(flashAddr);
      }

      pageProgram(flashAddr, data, DATA_SIZE);
      Serial.print("Write task: saved at 0x");
      Serial.println(flashAddr, HEX);

      flashAddr += DATA_SIZE; // 32바이트씩 증가

      xSemaphoreGive(spiMutex);
    }

    vTaskDelay(1000); // 1초 주기
  }
}


// Task 2: UART 명령 수신
// 'r' → Flash 저장값 출력
// 'c' → Flash 전체 초기화
void task_read(void *pvParameters) {
  Serial.println("Read task started");

  for (;;) {
    if (Serial.available()) {
      char cmd = Serial.read();

      // 'r': 저장된 데이터 전부 출력
      if (cmd == 'r') {
        if (xSemaphoreTake(spiMutex, portMAX_DELAY) == pdTRUE) {

          Serial.println("=== Flash 저장 데이터 출력 ===");
          uint8_t buf[DATA_SIZE];

          for (uint32_t addr = 0x000000; addr < flashAddr; addr += DATA_SIZE) {
            readData(addr, buf, DATA_SIZE);

            // 16bit 값 복원
            int16_t hx = (int16_t)(buf[0]  << 8 | buf[1]);
            int16_t hy = (int16_t)(buf[2]  << 8 | buf[3]);
            int16_t hz = (int16_t)(buf[4]  << 8 | buf[5]);
            int16_t ax = (int16_t)(buf[6]  << 8 | buf[7]);
            int16_t ay = (int16_t)(buf[8]  << 8 | buf[9]);
            int16_t az = (int16_t)(buf[10] << 8 | buf[11]);
            int16_t gx = (int16_t)(buf[12] << 8 | buf[13]);
            int16_t gy = (int16_t)(buf[14] << 8 | buf[15]);
            int16_t gz = (int16_t)(buf[16] << 8 | buf[17]);

            Serial.print("Addr 0x"); Serial.print(addr, HEX); Serial.print(" | ");
            Serial.print("HMC X:"); Serial.print(hx);
            Serial.print(" Y:"); Serial.print(hy);
            Serial.print(" Z:"); Serial.print(hz);
            Serial.print(" | Acc X:"); Serial.print(ax);
            Serial.print(" Y:"); Serial.print(ay);
            Serial.print(" Z:"); Serial.print(az);
            Serial.print(" | Gyro X:"); Serial.print(gx);
            Serial.print(" Y:"); Serial.print(gy);
            Serial.print(" Z:"); Serial.println(gz);
          }

          Serial.println("=== 출력 완료 ===");
          xSemaphoreGive(spiMutex);
        }
      }

      // 'c': Flash 전체 초기화 (Chip Erase)
      if (cmd == 'c') {
        if (xSemaphoreTake(spiMutex, portMAX_DELAY) == pdTRUE) {
          Serial.println("Flash 전체 초기화 중...");

          writeEnable();
          SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
          digitalWrite(CS_PIN, LOW);
          SPI.transfer(0xC7);    // Chip Erase 명령어
          digitalWrite(CS_PIN, HIGH);
          SPI.endTransaction();
          waitBusy();            // 완료까지 대기 (약 20~100초)

          flashAddr = 0x000000;
          Serial.println("Flash 초기화 완료");
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
  delay(2000);

  // 센서 초기화
  initHMC5883();
  initMPU6050();

  // SPI 초기화
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);
  SPI.begin();
  delay(100);

  // 리셋 후 마지막 주소 복원
  findLastAddr();
  Serial.println("초기화 완료");

  // Mutex 생성
  spiMutex = xSemaphoreCreateMutex();

  if (spiMutex != NULL) {
    xTaskCreate(task_write, "Write Task", 256, NULL, 2, &handle_write_task);
    xTaskCreate(task_read,  "Read Task",  256, NULL, 2, &handle_read_task);
    vTaskStartScheduler();
  }

  Serial.println("Failed to start FreeRTOS scheduler");
  while(1);
}

void loop() {
  // FreeRTOS 사용할땐 loop 사용 안 함
}
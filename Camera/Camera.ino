/*
 * ArduCAM Mini 2MP OV2640 + microSD + FreeRTOS_ARM
 * Board : Arduino Due
 * Version : v1.2
 *
 * 과제 목적:
 *  - Arduino Due 기반 SPI / I2C 하드웨어 연결
 *  - ArduCAM 예제 코드를 FreeRTOS Task 기반으로 수정
 *  - SPI 버스 접근을 Mutex로 보호
 *  - Host App 없이 Serial 명령으로 촬영 후 SD-card에 *.jpg 저장
 *
 * Serial 명령:
 *  s : 단일 촬영 후 SD-card에 JPG 저장
 *
 * 해상도:
 *  - 320x240 고정
 *
 * 구조:
 *  - Command Task : Serial 명령 수신
 *  - Capture Task : ArduCAM 촬영 수행
 *  - Save Task    : FIFO → RAM → SD-card 저장
 *  - SPI Mutex    : ArduCAM과 SD-card가 공유하는 SPI 버스 보호
 *
 * 참고:
 *  - I2C는 OV2640 센서 초기 설정에 사용됨.
 *  - 해상도 변경 기능을 제거했으므로, 런타임 중 I2C 접근은 없음.
 */

#include <FreeRTOS_ARM.h>
#include <Wire.h>
#include <SPI.h>
#include <ArduCAM.h>
#include "memorysaver.h"
#include <SdFat.h>

#if !(defined OV2640_MINI_2MP)
  #error "memorysaver.h에서 OV2640_MINI_2MP를 활성화하세요"
#endif

// ============================================================
// 1. Pin / SPI / SD 설정
// ============================================================

const int CAM_CS = 7;
const int SD_CS  = 10;

#define SD_FAT_TYPE 1

#if SD_FAT_TYPE == 0
  SdFat sd;
  typedef File FileT;
#elif SD_FAT_TYPE == 1
  SdFat32 sd;
  typedef File32 FileT;
#elif SD_FAT_TYPE == 2
  SdExFat sd;
  typedef ExFile FileT;
#else
  SdFs sd;
  typedef FsFile FileT;
#endif

/*
 * SD 속도:
 * 0KB 저장 문제를 줄이기 위해 안정성 우선으로 4MHz 사용.
 * 저장이 안정적으로 확인되면 8MHz 정도로 올려볼 수 있음.
 */
#define SD_CONFIG        SdSpiConfig(SD_CS, SHARED_SPI, SD_SCK_MHZ(4))
#define CAM_SPI_SETTINGS SPISettings(2000000, MSBFIRST, SPI_MODE0)

// 320x240 JPEG 저장용 RAM 버퍼
#define IMG_BUF_SIZE 16384

// ============================================================
// 2. 전역 객체 / 버퍼 / FreeRTOS 객체
// ============================================================

ArduCAM myCAM(OV2640, CAM_CS);

uint8_t  imgBuf[IMG_BUF_SIZE];
uint32_t imgLen = 0;

SemaphoreHandle_t xSpiMutex    = NULL;
SemaphoreHandle_t xSerialMutex = NULL;

QueueHandle_t xCaptureQueue = NULL;
QueueHandle_t xSaveQueue    = NULL;

volatile bool gBusy = false;

// ============================================================
// 3. Queue 메시지 타입
// ============================================================

typedef struct {
  uint8_t request;
} CaptureRequest;

typedef struct {
  uint8_t ready;
} SaveRequest;

// 4. 로그 함수
void logMsg(const char* tag, const char* msg) {
  if (xSerialMutex) xSemaphoreTake(xSerialMutex, portMAX_DELAY);

  Serial.print('[');
  Serial.print(tag);
  Serial.print(F("] "));
  Serial.println(msg);

  if (xSerialMutex) xSemaphoreGive(xSerialMutex);
}

void logVal(const char* tag, const char* msg, long val) {
  if (xSerialMutex) xSemaphoreTake(xSerialMutex, portMAX_DELAY);

  Serial.print('[');
  Serial.print(tag);
  Serial.print(F("] "));
  Serial.print(msg);
  Serial.println(val);

  if (xSerialMutex) xSemaphoreGive(xSerialMutex);
}

void logHexByte(const char* tag, const char* msg, uint8_t val) {
  if (xSerialMutex) xSemaphoreTake(xSerialMutex, portMAX_DELAY);

  Serial.print('[');
  Serial.print(tag);
  Serial.print(F("] "));
  Serial.print(msg);
  Serial.print(F("0x"));
  if (val < 0x10) Serial.print('0');
  Serial.println(val, HEX);

  if (xSerialMutex) xSemaphoreGive(xSerialMutex);
}

void printHelp() {
  logMsg("CMD", "명령어 안내");
  logMsg("CMD", "s : 단일 촬영 후 SD-card에 JPG 저장");
}

// ============================================================
// 5. SPI 공유 장치 CS 상태 정리
// ============================================================

void deselectAllSpiDevices() {
  digitalWrite(CAM_CS, HIGH);
  digitalWrite(SD_CS, HIGH);
  delayMicroseconds(10);
}

// ============================================================
// 6. 파일명 생성 함수
// ============================================================

bool nextFileName(char* buf) {
  static uint16_t idx = 0;

  for (; idx < 10000; idx++) {
    buf[0] = 'I';
    buf[1] = 'M';
    buf[2] = 'G';
    buf[3] = '0' + (idx / 1000) % 10;
    buf[4] = '0' + (idx / 100)  % 10;
    buf[5] = '0' + (idx / 10)   % 10;
    buf[6] = '0' + (idx)        % 10;
    strcpy(buf + 7, ".JPG");

    if (!sd.exists(buf)) {
      idx++;
      return true;
    }
  }

  return false;
}

// ============================================================
// 7. 촬영 요청 함수
// ============================================================

bool requestCapture() {
  if (gBusy) {
    logMsg("CMD", "이전 촬영/저장 처리 중 → 요청 무시");
    return false;
  }

  gBusy = true;

  CaptureRequest req;
  req.request = 1;

  if (xQueueSend(xCaptureQueue, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
    logMsg("CMD", "Capture Queue 전송 실패");
    gBusy = false;
    return false;
  }

  logMsg("CMD", "단일 촬영 요청 전송");
  return true;
}

// ============================================================
// 8. 카메라 캡처 함수
// ============================================================

bool captureToFifo() {
  xSemaphoreTake(xSpiMutex, portMAX_DELAY);

  logMsg("CAP", "SPI Mutex 획득 → 카메라 캡처 시작");

  deselectAllSpiDevices();

  SPI.beginTransaction(CAM_SPI_SETTINGS);

  myCAM.flush_fifo();
  myCAM.clear_fifo_flag();
  myCAM.start_capture();

  while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK)) {
    vTaskDelay(pdMS_TO_TICKS(2));
  }

  myCAM.clear_fifo_flag();

  SPI.endTransaction();

  deselectAllSpiDevices();

  xSemaphoreGive(xSpiMutex);

  logMsg("CAP", "카메라 캡처 완료 → FIFO에 이미지 저장됨");

  return true;
}

// ============================================================
// 9. FIFO → RAM 버퍼 읽기 함수
// ============================================================

bool readFifoToBuffer() {
  deselectAllSpiDevices();

  uint32_t length = 0;

  SPI.beginTransaction(CAM_SPI_SETTINGS);
  length = myCAM.read_fifo_length();
  SPI.endTransaction();

  deselectAllSpiDevices();

  logVal("SAVE", "FIFO 길이(byte) = ", (long)length);

  if (length == 0 || length >= MAX_FIFO_SIZE) {
    logMsg("SAVE", "FIFO 길이 이상 → 저장 취소");
    return false;
  }

  imgLen = 0;

  uint8_t temp = 0;
  uint8_t temp_last = 0;

  bool header   = false;
  bool eoi      = false;
  bool overflow = false;

  deselectAllSpiDevices();

  myCAM.CS_LOW();
  SPI.beginTransaction(CAM_SPI_SETTINGS);
  myCAM.set_fifo_burst();

  for (uint32_t i = 0; i < length; i++) {
    temp_last = temp;
    temp = SPI.transfer(0x00);

    if (header) {
      if (imgLen < IMG_BUF_SIZE) {
        imgBuf[imgLen++] = temp;
      } else {
        overflow = true;
        break;
      }

      if (temp_last == 0xFF && temp == 0xD9) {
        eoi = true;
        break;
      }
    } else {
      if (temp_last == 0xFF && temp == 0xD8) {
        header = true;

        if (imgLen + 2 <= IMG_BUF_SIZE) {
          imgBuf[imgLen++] = temp_last;
          imgBuf[imgLen++] = temp;
        } else {
          overflow = true;
          break;
        }
      }
    }
  }

  SPI.endTransaction();
  myCAM.CS_HIGH();

  deselectAllSpiDevices();

  if (!header) {
    logMsg("SAVE", "SOI 미발견 → 저장 취소");
    return false;
  }

  if (overflow) {
    logMsg("SAVE", "버퍼 초과 → 저장 취소");
    logVal("SAVE", "현재 버퍼 크기(byte) = ", IMG_BUF_SIZE);
    return false;
  }

  if (!eoi) {
    logMsg("SAVE", "EOI 미발견 → 저장 취소");
    return false;
  }

  if (imgLen < 4) {
    logMsg("SAVE", "JPEG 길이가 너무 짧음 → 저장 취소");
    return false;
  }

  logHexByte("SAVE", "JPEG 첫 바이트 = ", imgBuf[0]);
  logHexByte("SAVE", "JPEG 둘째 바이트 = ", imgBuf[1]);
  logHexByte("SAVE", "JPEG 끝-1 바이트 = ", imgBuf[imgLen - 2]);
  logHexByte("SAVE", "JPEG 끝 바이트 = ", imgBuf[imgLen - 1]);

  if (!(imgBuf[0] == 0xFF && imgBuf[1] == 0xD8)) {
    logMsg("SAVE", "JPEG SOI 마커 불일치 → 저장 취소");
    return false;
  }

  if (!(imgBuf[imgLen - 2] == 0xFF && imgBuf[imgLen - 1] == 0xD9)) {
    logMsg("SAVE", "JPEG EOI 마커 불일치 → 저장 취소");
    return false;
  }

  logVal("SAVE", "RAM 버퍼 저장 바이트 = ", (long)imgLen);
  return true;
}

// ============================================================
// 10. RAM 버퍼 → SD-card 저장 함수
// ============================================================

bool writeBufferToSD() {
  char name[16];

  if (!nextFileName(name)) {
    logMsg("SAVE", "저장 가능한 파일명 없음");
    return false;
  }

  if (imgLen == 0) {
    logMsg("SAVE", "imgLen이 0 → 저장 취소");
    return false;
  }

  deselectAllSpiDevices();

  FileT file;

  if (!file.open(name, O_WRITE | O_CREAT | O_TRUNC)) {
    logMsg("SAVE", "파일 열기 실패");
    return false;
  }

  logMsg("SAVE", "파일 열기 성공");
  logMsg("SAVE", name);
  logVal("SAVE", "저장 시도 바이트 = ", (long)imgLen);

  size_t written = file.write(imgBuf, imgLen);

  logVal("SAVE", "file.write 반환 바이트 = ", (long)written);

  if (written != imgLen) {
    logMsg("SAVE", "파일 쓰기 불완전");
    file.close();
    deselectAllSpiDevices();
    return false;
  }

  if (!file.sync()) {
    logMsg("SAVE", "file.sync 실패");
    file.close();
    deselectAllSpiDevices();
    return false;
  }

  uint32_t sizeBeforeClose = file.fileSize();
  logVal("SAVE", "close 전 파일 크기 = ", (long)sizeBeforeClose);

  if (!file.close()) {
    logMsg("SAVE", "file.close 실패");
    deselectAllSpiDevices();
    return false;
  }

  deselectAllSpiDevices();

  FileT checkFile;

  if (!checkFile.open(name, O_READ)) {
    logMsg("SAVE", "저장 확인용 파일 열기 실패");
    return false;
  }

  uint32_t sizeAfterClose = checkFile.fileSize();
  checkFile.close();

  logVal("SAVE", "close 후 파일 크기 = ", (long)sizeAfterClose);

  if (sizeAfterClose != imgLen) {
    logMsg("SAVE", "파일 크기 불일치 → 저장 실패");
    return false;
  }

  logMsg("SAVE", "SD-card 저장 완료");
  return true;
}

// ============================================================
// 11. Command Task
//     Serial 명령 수신 담당
// ============================================================

void vCommandTask(void* pv) {
  for (;;) {
    if (Serial.available()) {
      char c = Serial.read();

      if (c == 's' || c == 'S') {
        requestCapture();
      } else if (c != '\r' && c != '\n') {
        printHelp();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ============================================================
// 12. Capture Task
//     카메라 촬영 담당
// ============================================================

void vCaptureTask(void* pv) {
  CaptureRequest req;

  for (;;) {
    if (xQueueReceive(xCaptureQueue, &req, portMAX_DELAY) == pdTRUE) {
      logMsg("CAP", "Capture Queue 수신");

      bool ok = captureToFifo();

      if (ok) {
        SaveRequest saveReq;
        saveReq.ready = 1;

        if (xQueueSend(xSaveQueue, &saveReq, pdMS_TO_TICKS(100)) != pdTRUE) {
          logMsg("CAP", "Save Queue 전송 실패");
          gBusy = false;
        } else {
          logMsg("CAP", "Save Queue 전송 완료");
        }
      } else {
        logMsg("CAP", "캡처 실패");
        gBusy = false;
      }

      logVal("CAP", "남은 스택(words) = ", (long)uxTaskGetStackHighWaterMark(NULL));
    }
  }
}

// ============================================================
// 13. Save Task
//     FIFO → RAM → SD 저장 담당
// ============================================================

void vSaveTask(void* pv) {
  SaveRequest req;

  for (;;) {
    logMsg("SAVE", "Save Queue 대기 중...");

    if (xQueueReceive(xSaveQueue, &req, portMAX_DELAY) == pdTRUE) {
      bool readOk  = false;
      bool writeOk = false;

      // 1단계: 카메라 FIFO → RAM 버퍼
      xSemaphoreTake(xSpiMutex, portMAX_DELAY);

      logMsg("SAVE", "SPI Mutex 획득 → FIFO를 RAM으로 읽기");
      readOk = readFifoToBuffer();

      xSemaphoreGive(xSpiMutex);
      logMsg("SAVE", "SPI Mutex 반납");

      // 2단계: RAM 버퍼 → SD-card
      if (readOk && imgLen > 0) {
        xSemaphoreTake(xSpiMutex, portMAX_DELAY);

        logMsg("SAVE", "SPI Mutex 획득 → SD-card에 쓰기");
        writeOk = writeBufferToSD();

        xSemaphoreGive(xSpiMutex);
        logMsg("SAVE", "SPI Mutex 반납");
      }

      if (readOk && writeOk) {
        logMsg("SAVE", "===== 1장 저장 완료 =====");
      } else {
        logMsg("SAVE", "===== 저장 실패 또는 취소 =====");
      }

      logVal("SAVE", "남은 스택(words) = ", (long)uxTaskGetStackHighWaterMark(NULL));

      gBusy = false;
    }
  }
}

// ============================================================
// 14. 초기화 함수
// ============================================================

void initHardwarePins() {
  pinMode(CAM_CS, OUTPUT);
  pinMode(SD_CS, OUTPUT);

  digitalWrite(CAM_CS, HIGH);
  digitalWrite(SD_CS, HIGH);
}

void initCamera() {
  deselectAllSpiDevices();

  // ArduCAM CPLD Reset
  myCAM.write_reg(0x07, 0x80);
  delay(100);
  myCAM.write_reg(0x07, 0x00);
  delay(100);

  deselectAllSpiDevices();

  // SPI 통신 확인
  myCAM.write_reg(ARDUCHIP_TEST1, 0x55);

  if (myCAM.read_reg(ARDUCHIP_TEST1) != 0x55) {
    Serial.println(F("[정지] 카메라 SPI 오류"));
    while (1);
  }

  Serial.println(F("[OK] 카메라 SPI 통신 확인"));

  // OV2640 ID 확인
  uint8_t vid = 0;
  uint8_t pid = 0;

  myCAM.wrSensorReg8_8(0xff, 0x01);
  myCAM.rdSensorReg8_8(OV2640_CHIPID_HIGH, &vid);
  myCAM.rdSensorReg8_8(OV2640_CHIPID_LOW,  &pid);

  Serial.print(F("[INFO] OV2640 VID = 0x"));
  Serial.println(vid, HEX);
  Serial.print(F("[INFO] OV2640 PID = 0x"));
  Serial.println(pid, HEX);

  if (vid != 0x26 || (pid != 0x41 && pid != 0x42)) {
    Serial.println(F("[정지] OV2640 미검출"));
    Serial.println(F("확인: SDA/SCL 배선, Wire/Wire1 사용 여부, ArduCAM 라이브러리 내부 설정"));
    while (1);
  }

  Serial.println(F("[OK] OV2640 감지 완료"));

  // JPEG 모드 초기화
  myCAM.set_format(JPEG);
  myCAM.InitCAM();

  // 해상도 320x240 고정
  // 초기화 단계이므로 Mutex 사용하지 않음
  myCAM.OV2640_set_JPEG_size(OV2640_320x240);

  delay(1000);

  myCAM.clear_fifo_flag();

  deselectAllSpiDevices();

  Serial.println(F("[OK] 카메라 JPEG 320x240 고정 초기화 완료"));
}

void initSDCard() {
  deselectAllSpiDevices();

  if (!sd.begin(SD_CONFIG)) {
    Serial.println(F("[정지] SdFat 초기화 실패"));
    Serial.println(F("확인: SD_CS, 배선, SD 모듈 전원, SPI 속도"));
    while (1);
  }

  deselectAllSpiDevices();

  Serial.println(F("[OK] SD-card 초기화 완료"));
}

void initRTOSObjects() {
  xSerialMutex = xSemaphoreCreateMutex();
  xSpiMutex    = xSemaphoreCreateMutex();

  xCaptureQueue = xQueueCreate(1, sizeof(CaptureRequest));
  xSaveQueue    = xQueueCreate(1, sizeof(SaveRequest));

  if (!xSerialMutex || !xSpiMutex || !xCaptureQueue || !xSaveQueue) {
    Serial.println(F("[정지] FreeRTOS 객체 생성 실패"));
    while (1);
  }

  Serial.println(F("[OK] FreeRTOS Mutex / Queue 생성 완료"));
}

// ============================================================
// 15. setup / loop
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println(F("============================================="));
  Serial.println(F(" ArduCAM Mini 2MP OV2640 + SD + FreeRTOS v1.2"));
  Serial.println(F(" Board: Arduino Due"));
  Serial.println(F(" Resolution: 320x240 fixed"));
  Serial.println(F(" SD SPI Clock: 4 MHz"));
  Serial.println(F("============================================="));

  initHardwarePins();

  /*
   * 현재 환경에서 OV2640 VID/PID가 정상 출력되었으므로 Wire1 사용 가능.
   * 만약 OV2640 미검출이 발생하면 ArduCAM 라이브러리의 Wire/Wire1 사용 여부 확인.
   */
  Wire1.begin();

  SPI.begin();

  /*
   * 중요:
   * 하드웨어 초기화를 먼저 끝낸 뒤 FreeRTOS 객체를 생성하고 Task를 시작한다.
   * setup() 단계에서는 아직 Task들이 동시에 실행되지 않으므로 Mutex가 필요 없다.
   */
  initCamera();
  initSDCard();
  initRTOSObjects();

  Serial.println();
  Serial.println(F("준비 완료"));
  Serial.println(F("명령:"));
  Serial.println(F("  s : 단일 촬영 후 SD-card에 JPG 저장"));
  Serial.println(F("해상도: 320x240 고정"));
  Serial.println();

  /*
   * Task 우선순위 설계:
   *  - Capture Task: 카메라 캡처 타이밍 중요 → 높은 우선순위
   *  - Save Task   : SD 저장 I/O 처리 → 중간 우선순위
   *  - Command Task: Serial 명령 처리 → 낮은 우선순위
   */
  xTaskCreate(vCommandTask, "cmd",  512,  NULL, 1, NULL);
  xTaskCreate(vCaptureTask, "cap",  512,  NULL, 3, NULL);
  xTaskCreate(vSaveTask,    "save", 2048, NULL, 2, NULL);

  vTaskStartScheduler();

  Serial.println(F("[정지] 스케줄러 시작 실패"));
  while (1);
}

void loop() {
  // FreeRTOS 사용 시 loop는 사용하지 않음
}